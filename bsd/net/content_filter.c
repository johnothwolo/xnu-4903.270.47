/*
 * Copyright (c) 2013-2018 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * THEORY OF OPERATION
 *
 * The socket content filter subsystem provides a way for user space agents to
 * make filtering decisions based on the content of the data being sent and
 * received by TCP/IP sockets.
 *
 * A content filter user space agents gets a copy of the data and the data is
 * also kept in kernel buffer until the user space agents makes a pass or drop
 * decision. This unidirectional flow of content avoids unnecessary data copies
 * back to the kernel.
 *
 * A user space filter agent opens a kernel control socket with the name
 * CONTENT_FILTER_CONTROL_NAME to attach to the socket content filter subsystem.
 * When connected, a "struct content_filter" is created and set as the
 * "unitinfo" of the corresponding kernel control socket instance.
 *
 * The socket content filter subsystem exchanges messages with the user space
 * filter agent until an ultimate pass or drop decision is made by the
 * user space filter agent.
 *
 * It should be noted that messages about many TCP/IP sockets can be multiplexed
 * over a single kernel control socket.
 *
 * Notes:
 * - The current implementation is limited to TCP sockets.
 * - The current implementation supports up to two simultaneous content filters
 *   for the sake of simplicity of the implementation.
 *
 *
 * NECP FILTER CONTROL UNIT
 *
 * A user space filter agent uses the Network Extension Control Policy (NECP)
 * database to specify which TCP/IP sockets need to be filtered. The NECP
 * criteria may be based on a variety of properties like user ID or proc UUID.
 *
 * The NECP "filter control unit" is used by the socket content filter subsystem
 * to deliver the relevant TCP/IP content information to the appropriate
 * user space filter agent via its kernel control socket instance.
 * This works as follows:
 *
 * 1) The user space filter agent specifies an NECP filter control unit when
 *    in adds its filtering rules to the NECP database.
 *
 * 2) The user space filter agent also sets its NECP filter control unit on the
 *    content filter kernel control socket via the socket option
 *    CFIL_OPT_NECP_CONTROL_UNIT.
 *
 * 3) The NECP database is consulted to find out if a given TCP/IP socket
 *    needs to be subjected to content filtering and returns the corresponding
 *    NECP filter control unit  -- the NECP filter control unit is actually
 *    stored in the TCP/IP socket structure so the NECP lookup is really simple.
 *
 * 4) The NECP filter control unit is then used to find the corresponding
 *    kernel control socket instance.
 *
 * Note: NECP currently supports a single filter control unit per TCP/IP socket
 *       but this restriction may be soon lifted.
 *
 *
 * THE MESSAGING PROTOCOL
 *
 * The socket content filter subsystem and a user space filter agent
 * communicate over the kernel control socket via an asynchronous
 * messaging protocol (this is not a request-response protocol).
 * The socket content filter subsystem sends event messages to the user
 * space filter agent about the TCP/IP sockets it is interested to filter.
 * The user space filter agent sends action messages to either allow
 * data to pass or to disallow the data flow (and drop the connection).
 *
 * All messages over a content filter kernel control socket share the same
 * common header of type "struct cfil_msg_hdr". The message type tells if
 * it's a event message "CFM_TYPE_EVENT" or a action message "CFM_TYPE_ACTION".
 * The message header field "cfm_sock_id" identifies a given TCP/IP socket.
 * Note the message header length field may be padded for alignment and can
 * be larger than the actual content of the message.
 * The field "cfm_op" describe the kind of event or action.
 *
 * Here are the kinds of content filter events:
 * - CFM_OP_SOCKET_ATTACHED: a new TCP/IP socket is being filtered
 * - CFM_OP_SOCKET_CLOSED: A TCP/IP socket is closed
 * - CFM_OP_DATA_OUT: A span of data is being sent on a TCP/IP socket
 * - CFM_OP_DATA_IN: A span of data is being or received on a TCP/IP socket
 *
 *
 * EVENT MESSAGES
 *
 * The CFM_OP_DATA_OUT and CFM_OP_DATA_IN event messages contains a span of
 * data that is being sent or received. The position of this span of data
 * in the data flow is described by a set of start and end offsets. These
 * are absolute 64 bits offsets. The first byte sent (or received) starts
 * at offset 0 and ends at offset 1. The length of the content data
 * is given by the difference between the end offset and the start offset.
 *
 * After a CFM_OP_SOCKET_ATTACHED is delivered, CFM_OP_DATA_OUT and
 * CFM_OP_DATA_OUT events are not delivered until a CFM_OP_DATA_UPDATE
 * action message is sent by the user space filter agent.
 *
 * Note: absolute 64 bits offsets should be large enough for the foreseeable
 * future.  A 64-bits counter will wrap after 468 years at 10 Gbit/sec:
 *   2E64 / ((10E9 / 8) * 60 * 60 * 24 * 365.25) = 467.63
 *
 * They are two kinds of primary content filter actions:
 * - CFM_OP_DATA_UPDATE: to update pass or peek offsets for each direction.
 * - CFM_OP_DROP: to shutdown socket and disallow further data flow
 *
 * There is also an action to mark a given client flow as already filtered
 * at a higher level, CFM_OP_BLESS_CLIENT.
 *
 *
 * ACTION MESSAGES
 *
 * The CFM_OP_DATA_UPDATE action messages let the user space filter
 * agent allow data to flow up to the specified pass offset -- there
 * is a pass offset for outgoing data and  a pass offset for incoming data.
 * When a new TCP/IP socket is attached to the content filter, each pass offset
 * is initially set to 0 so not data is allowed to pass by default.
 * When the pass offset is set to CFM_MAX_OFFSET via a CFM_OP_DATA_UPDATE
 * then the data flow becomes unrestricted.
 *
 * Note that pass offsets can only be incremented. A CFM_OP_DATA_UPDATE message
 * with a pass offset smaller than the pass offset of a previous
 * CFM_OP_DATA_UPDATE message is silently ignored.
 *
 * A user space filter agent also uses CFM_OP_DATA_UPDATE action messages
 * to tell the kernel how much data it wants to see by using the peek offsets.
 * Just like pass offsets, there is a peek offset for each direction.
 * When a new TCP/IP socket is attached to the content filter, each peek offset
 * is initially set to 0 so no CFM_OP_DATA_OUT and CFM_OP_DATA_IN event
 * messages are dispatched by default until a CFM_OP_DATA_UPDATE action message
 * with a greater than 0 peek offset is sent by the user space filter agent.
 * When the peek offset is set to CFM_MAX_OFFSET via a CFM_OP_DATA_UPDATE
 * then the flow of update data events becomes unrestricted.
 *
 * Note that peek offsets cannot be smaller than the corresponding pass offset.
 * Also a peek offsets cannot be smaller than the corresponding end offset
 * of the last CFM_OP_DATA_OUT/CFM_OP_DATA_IN message dispatched. Trying
 * to set a too small peek value is silently ignored.
 *
 *
 * PER SOCKET "struct cfil_info"
 *
 * As soon as a TCP/IP socket gets attached to a content filter, a
 * "struct cfil_info" is created to hold the content filtering state for this
 * socket.
 *
 * The content filtering state is made of the following information
 * for each direction:
 * - The current pass offset;
 * - The first and last offsets of the data pending, waiting for a filtering
 *   decision;
 * - The inject queue for data that passed the filters and that needs
 *   to be re-injected;
 * - A content filter specific state in a set of  "struct cfil_entry"
 *
 *
 * CONTENT FILTER STATE "struct cfil_entry"
 *
 * The "struct cfil_entry" maintains the information most relevant to the
 * message handling over a kernel control socket with a user space filter agent.
 *
 * The "struct cfil_entry" holds the NECP filter control unit that corresponds
 * to the kernel control socket unit it corresponds to and also has a pointer
 * to the corresponding "struct content_filter".
 *
 * For each direction, "struct cfil_entry" maintains the following information:
 * - The pass offset
 * - The peek offset
 * - The offset of the last data peeked at by the filter
 * - A queue of data that's waiting to be delivered to the  user space filter
 *   agent on the kernel control socket
 * - A queue of data for which event messages have been sent on the kernel
 *   control socket and are pending for a filtering decision.
 *
 *
 * CONTENT FILTER QUEUES
 *
 * Data that is being filtered is steered away from the TCP/IP socket buffer
 * and instead will sit in one of three content filter queues until the data
 * can be re-injected into the TCP/IP socket buffer.
 *
 * A content filter queue is represented by "struct cfil_queue" that contains
 * a list of mbufs and the start and end offset of the data span of
 * the list of mbufs.
 *
 * The data moves into the three content filter queues according to this
 * sequence:
 * a) The "cfe_ctl_q" of "struct cfil_entry"
 * b) The "cfe_pending_q" of "struct cfil_entry"
 * c) The "cfi_inject_q" of "struct cfil_info"
 *
 * Note: The sequence (a),(b) may be repeated several times if there is more
 * than one content filter attached to the TCP/IP socket.
 *
 * The "cfe_ctl_q" queue holds data than cannot be delivered to the
 * kernel conntrol socket for two reasons:
 * - The peek offset is less that the end offset of the mbuf data
 * - The kernel control socket is flow controlled
 *
 * The "cfe_pending_q" queue holds data for which CFM_OP_DATA_OUT or
 * CFM_OP_DATA_IN have been successfully dispatched to the kernel control
 * socket and are waiting for a pass action message fromn the user space
 * filter agent. An mbuf length must be fully allowed to pass to be removed
 * from the cfe_pending_q.
 *
 * The "cfi_inject_q" queue holds data that has been fully allowed to pass
 * by the user space filter agent and that needs to be re-injected into the
 * TCP/IP socket.
 *
 *
 * IMPACT ON FLOW CONTROL
 *
 * An essential aspect of the content filer subsystem is to minimize the
 * impact on flow control of the TCP/IP sockets being filtered.
 *
 * The processing overhead of the content filtering may have an effect on
 * flow control by adding noticeable delays and cannot be eliminated --
 * care must be taken by the user space filter agent to minimize the
 * processing delays.
 *
 * The amount of data being filtered is kept in buffers while waiting for
 * a decision by the user space filter agent. This amount of data pending
 * needs to be subtracted from the amount of data available in the
 * corresponding TCP/IP socket buffer. This is done by modifying
 * sbspace() and tcp_sbspace() to account for amount of data pending
 * in the content filter.
 *
 *
 * LOCKING STRATEGY
 *
 * The global state of content filter subsystem is protected by a single
 * read-write lock "cfil_lck_rw". The data flow can be done with the
 * cfil read-write lock held as shared so it can be re-entered from multiple
 * threads.
 *
 * The per TCP/IP socket content filterstate -- "struct cfil_info" -- is
 * protected by the socket lock.
 *
 * A TCP/IP socket lock cannot be taken while the cfil read-write lock
 * is held. That's why we have some sequences where we drop the cfil read-write
 * lock before taking the TCP/IP lock.
 *
 * It is also important to lock the TCP/IP socket buffer while the content
 * filter is modifying the amount of pending data. Otherwise the calculations
 * in sbspace() and tcp_sbspace()  could be wrong.
 *
 * The "cfil_lck_rw" protects "struct content_filter" and also the fields
 * "cfe_link" and "cfe_filter" of "struct cfil_entry".
 *
 * Actually "cfe_link" and "cfe_filter" are protected by both by
 * "cfil_lck_rw" and the socket lock: they may be modified only when
 * "cfil_lck_rw" is exclusive and the socket is locked.
 *
 * To read the other fields of "struct content_filter" we have to take
 * "cfil_lck_rw" in shared mode.
 *
 *
 * LIMITATIONS
 *
 * - For TCP sockets only
 *
 * - Does not support TCP unordered messages
 */

/*
 *	TO DO LIST
 *
 *	SOONER:
 *
 *	Deal with OOB
 *
 *	LATER:
 *
 *	If support datagram, enqueue control and address mbufs as well
 */

#include <sys/types.h>
#include <sys/kern_control.h>
#include <sys/queue.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/syslog.h>
#include <sys/systm.h>
#include <sys/param.h>
#include <sys/mbuf.h>

#include <kern/locks.h>
#include <kern/zalloc.h>
#include <kern/debug.h>

#include <net/content_filter.h>

#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#include <string.h>
#include <libkern/libkern.h>
#include <kern/sched_prim.h>

#define MAX_CONTENT_FILTER 2

struct cfil_entry;

/*
 * The structure content_filter represents a user space content filter
 * It's created and associated with a kernel control socket instance
 */
struct content_filter {
	kern_ctl_ref            cf_kcref;
	u_int32_t               cf_kcunit;
	u_int32_t               cf_flags;

	uint32_t                cf_necp_control_unit;

	uint32_t                cf_sock_count;
	TAILQ_HEAD(, cfil_entry) cf_sock_entries;
};

#define CFF_ACTIVE              0x01
#define CFF_DETACHING           0x02
#define CFF_FLOW_CONTROLLED     0x04

struct content_filter **content_filters = NULL;
uint32_t cfil_active_count = 0; /* Number of active content filters */
uint32_t cfil_sock_attached_count = 0;  /* Number of sockets attachements */
uint32_t cfil_sock_udp_attached_count = 0;      /* Number of UDP sockets attachements */
uint32_t cfil_close_wait_timeout = 1000; /* in milliseconds */

static kern_ctl_ref cfil_kctlref = NULL;

static lck_grp_attr_t *cfil_lck_grp_attr = NULL;
static lck_attr_t *cfil_lck_attr = NULL;
static lck_grp_t *cfil_lck_grp = NULL;
decl_lck_rw_data(static, cfil_lck_rw);

#define CFIL_RW_LCK_MAX 8

int cfil_rw_nxt_lck = 0;
void* cfil_rw_lock_history[CFIL_RW_LCK_MAX];

int cfil_rw_nxt_unlck = 0;
void* cfil_rw_unlock_history[CFIL_RW_LCK_MAX];

#define CONTENT_FILTER_ZONE_NAME        "content_filter"
#define CONTENT_FILTER_ZONE_MAX         10
static struct zone *content_filter_zone = NULL; /* zone for content_filter */


#define CFIL_INFO_ZONE_NAME     "cfil_info"
#define CFIL_INFO_ZONE_MAX      1024
static struct zone *cfil_info_zone = NULL;      /* zone for cfil_info */

MBUFQ_HEAD(cfil_mqhead);

struct cfil_queue {
	uint64_t                q_start; /* offset of first byte in queue */
	uint64_t                q_end; /* offset of last byte in queue */
	struct cfil_mqhead      q_mq;
};

/*
 * struct cfil_entry
 *
 * The is one entry per content filter
 */
struct cfil_entry {
	TAILQ_ENTRY(cfil_entry) cfe_link;
	struct content_filter   *cfe_filter;

	struct cfil_info        *cfe_cfil_info;
	uint32_t                cfe_flags;
	uint32_t                cfe_necp_control_unit;
	struct timeval          cfe_last_event; /* To user space */
	struct timeval          cfe_last_action; /* From user space */

	struct cfe_buf {
		/*
		 * cfe_pending_q holds data that has been delivered to
		 * the filter and for which we are waiting for an action
		 */
		struct cfil_queue       cfe_pending_q;
		/*
		 * This queue is for data that has not be delivered to
		 * the content filter (new data, pass peek or flow control)
		 */
		struct cfil_queue       cfe_ctl_q;

		uint64_t                cfe_pass_offset;
		uint64_t                cfe_peek_offset;
		uint64_t                cfe_peeked;
	} cfe_snd, cfe_rcv;
};

#define CFEF_CFIL_ATTACHED              0x0001  /* was attached to filter */
#define CFEF_SENT_SOCK_ATTACHED         0x0002  /* sock attach event was sent */
#define CFEF_DATA_START                 0x0004  /* can send data event */
#define CFEF_FLOW_CONTROLLED            0x0008  /* wait for flow control lift */
#define CFEF_SENT_DISCONNECT_IN         0x0010  /* event was sent */
#define CFEF_SENT_DISCONNECT_OUT        0x0020  /* event was sent */
#define CFEF_SENT_SOCK_CLOSED           0x0040  /* closed event was sent */
#define CFEF_CFIL_DETACHED              0x0080  /* filter was detached */


#define CFI_ADD_TIME_LOG(cfil, t1, t0, op)                                                                                      \
	        struct timeval _tdiff;                                                                                          \
	        if ((cfil)->cfi_op_list_ctr < CFI_MAX_TIME_LOG_ENTRY) {                                                         \
	                timersub(t1, t0, &_tdiff);                                                                              \
	                (cfil)->cfi_op_time[(cfil)->cfi_op_list_ctr] = (uint32_t)(_tdiff.tv_sec * 1000 + _tdiff.tv_usec / 1000);\
	                (cfil)->cfi_op_list[(cfil)->cfi_op_list_ctr] = (unsigned char)op;                                       \
	                (cfil)->cfi_op_list_ctr ++;                                                                             \
	        }

struct cfil_hash_entry;

/*
 * struct cfil_info
 *
 * There is a struct cfil_info per socket
 */
struct cfil_info {
	TAILQ_ENTRY(cfil_info)  cfi_link;
	struct socket           *cfi_so;
	uint64_t                cfi_flags;
	uint64_t                cfi_sock_id;
	struct timeval64        cfi_first_event;
	uint32_t                cfi_op_list_ctr;
	uint32_t                cfi_op_time[CFI_MAX_TIME_LOG_ENTRY];    /* time interval in microseconds since first event */
	unsigned char           cfi_op_list[CFI_MAX_TIME_LOG_ENTRY];

	struct cfi_buf {
		/*
		 * cfi_pending_first and cfi_pending_last describe the total
		 * amount of data outstanding for all the filters on
		 * this socket and data in the flow queue
		 * cfi_pending_mbcnt counts in sballoc() "chars of mbufs used"
		 */
		uint64_t                cfi_pending_first;
		uint64_t                cfi_pending_last;
		uint32_t                cfi_pending_mbcnt;
		uint32_t                cfi_pending_mbnum;
		uint32_t                cfi_tail_drop_cnt;
		/*
		 * cfi_pass_offset is the minimum of all the filters
		 */
		uint64_t                cfi_pass_offset;
		/*
		 * cfi_inject_q holds data that needs to be re-injected
		 * into the socket after filtering and that can
		 * be queued because of flow control
		 */
		struct cfil_queue       cfi_inject_q;
	} cfi_snd, cfi_rcv;

	struct cfil_entry       cfi_entries[MAX_CONTENT_FILTER];
	struct cfil_hash_entry *cfi_hash_entry;
} __attribute__((aligned(8)));

#define CFIF_DROP               0x0001  /* drop action applied */
#define CFIF_CLOSE_WAIT         0x0002  /* waiting for filter to close */
#define CFIF_SOCK_CLOSED        0x0004  /* socket is closed */
#define CFIF_RETRY_INJECT_IN    0x0010  /* inject in failed */
#define CFIF_RETRY_INJECT_OUT   0x0020  /* inject out failed */
#define CFIF_SHUT_WR            0x0040  /* shutdown write */
#define CFIF_SHUT_RD            0x0080  /* shutdown read */

#define CFI_MASK_GENCNT         0xFFFFFFFF00000000      /* upper 32 bits */
#define CFI_SHIFT_GENCNT        32
#define CFI_MASK_FLOWHASH       0x00000000FFFFFFFF      /* lower 32 bits */
#define CFI_SHIFT_FLOWHASH      0

TAILQ_HEAD(cfil_sock_head, cfil_info) cfil_sock_head;

#define CFIL_QUEUE_VERIFY(x) if (cfil_debug) cfil_queue_verify(x)
#define CFIL_INFO_VERIFY(x) if (cfil_debug) cfil_info_verify(x)

/*
 * UDP Socket Support
 */
LIST_HEAD(cfilhashhead, cfil_hash_entry);
#define CFILHASHSIZE 16
#define CFIL_HASH(laddr, faddr, lport, fport) ((faddr) ^ ((laddr) >> 16) ^ (fport) ^ (lport))
#define IS_UDP(so) (so && so->so_proto->pr_type == SOCK_DGRAM && so->so_proto->pr_protocol == IPPROTO_UDP)
#define UNCONNECTED(inp) (inp && (((inp->inp_vflag & INP_IPV4) && (inp->inp_faddr.s_addr == INADDR_ANY)) || \
	                                                          ((inp->inp_vflag & INP_IPV6) && IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_faddr))))
#define IS_ENTRY_ATTACHED(cfil_info, kcunit) (cfil_info != NULL && (kcunit <= MAX_CONTENT_FILTER) && \
	                                                                                  cfil_info->cfi_entries[kcunit - 1].cfe_filter != NULL)
#define IS_DNS(local, remote) (check_port(local, 53) || check_port(remote, 53) || check_port(local, 5353) || check_port(remote, 5353))

/*
 * UDP Garbage Collection:
 */
static struct thread *cfil_udp_gc_thread;
#define UDP_FLOW_GC_IDLE_TO          30  // Flow Idle Timeout in seconds
#define UDP_FLOW_GC_ACTION_TO        10  // Flow Action Timeout (no action from user space) in seconds
#define UDP_FLOW_GC_MAX_COUNT        100 // Max UDP flows to be handled per run
#define UDP_FLOW_GC_RUN_INTERVAL_NSEC  (10 * NSEC_PER_SEC)  // GC wakes up every 10 seconds

/*
 * UDP flow queue thresholds
 */
#define UDP_FLOW_GC_MBUF_CNT_MAX  (2 << MBSHIFT) // Max mbuf byte count in flow queue (2MB)
#define UDP_FLOW_GC_MBUF_NUM_MAX  (UDP_FLOW_GC_MBUF_CNT_MAX >> MCLSHIFT) // Max mbuf count in flow queue (1K)
#define UDP_FLOW_GC_MBUF_SHIFT    5             // Shift to get 1/32 of platform limits
/*
 * UDP flow queue threshold globals:
 */
static unsigned int cfil_udp_gc_mbuf_num_max = UDP_FLOW_GC_MBUF_NUM_MAX;
static unsigned int cfil_udp_gc_mbuf_cnt_max = UDP_FLOW_GC_MBUF_CNT_MAX;

/*
 * struct cfil_hash_entry
 *
 * Hash entry for cfil_info
 */
struct cfil_hash_entry {
	LIST_ENTRY(cfil_hash_entry)    cfentry_link;
	struct cfil_info               *cfentry_cfil;
	u_short cfentry_fport;
	u_short cfentry_lport;
	sa_family_t                    cfentry_family;
	u_int32_t                      cfentry_flowhash;
	u_int32_t                      cfentry_lastused;
	union {
		/* foreign host table entry */
		struct in_addr_4in6 addr46;
		struct in6_addr addr6;
	} cfentry_faddr;
	union {
		/* local host table entry */
		struct in_addr_4in6 addr46;
		struct in6_addr addr6;
	} cfentry_laddr;
};

/*
 * struct cfil_db
 *
 * For each UDP socket, this is a hash table maintaining all cfil_info structs
 * keyed by the flow 4-tuples <lport,fport,laddr,faddr>.
 */
struct cfil_db {
	struct socket       *cfdb_so;
	uint32_t            cfdb_count;       /* Number of total content filters */
	struct cfilhashhead *cfdb_hashbase;
	u_long              cfdb_hashmask;
	struct cfil_hash_entry *cfdb_only_entry;  /* Optimization for connected UDP */
};

/*
 * CFIL specific mbuf tag:
 * Save state of socket at the point of data entry into cfil.
 * Use saved state for reinjection at protocol layer.
 */
struct cfil_tag {
	union sockaddr_in_4_6 cfil_faddr;
	uint32_t cfil_so_state_change_cnt;
	short cfil_so_options;
};

#define    CFIL_HASH_ENTRY_ZONE_NAME    "cfil_entry_hash"
#define    CFIL_HASH_ENTRY_ZONE_MAX     1024
static struct zone *cfil_hash_entry_zone = NULL;

#define    CFIL_DB_ZONE_NAME       "cfil_db"
#define    CFIL_DB_ZONE_MAX        1024
static struct zone *cfil_db_zone = NULL;

/*
 * Statistics
 */

struct cfil_stats cfil_stats;

/*
 * For troubleshooting
 */
int cfil_log_level = LOG_ERR;
int cfil_debug = 1;

// Debug controls added for selective debugging.
// Disabled for production.  If enabled,
// these will have performance impact
#define LIFECYCLE_DEBUG 0
#define VERDICT_DEBUG 0
#define DATA_DEBUG 0
#define SHOW_DEBUG 0
#define GC_DEBUG 0

/*
 * Sysctls for logs and statistics
 */
static int sysctl_cfil_filter_list(struct sysctl_oid *, void *, int,
    struct sysctl_req *);
static int sysctl_cfil_sock_list(struct sysctl_oid *, void *, int,
    struct sysctl_req *);

SYSCTL_NODE(_net, OID_AUTO, cfil, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "cfil");

SYSCTL_INT(_net_cfil, OID_AUTO, log, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_log_level, 0, "");

SYSCTL_INT(_net_cfil, OID_AUTO, debug, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_debug, 0, "");

SYSCTL_UINT(_net_cfil, OID_AUTO, sock_attached_count, CTLFLAG_RD | CTLFLAG_LOCKED,
    &cfil_sock_attached_count, 0, "");

SYSCTL_UINT(_net_cfil, OID_AUTO, active_count, CTLFLAG_RD | CTLFLAG_LOCKED,
    &cfil_active_count, 0, "");

SYSCTL_UINT(_net_cfil, OID_AUTO, close_wait_timeout, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_close_wait_timeout, 0, "");

static int cfil_sbtrim = 1;
SYSCTL_UINT(_net_cfil, OID_AUTO, sbtrim, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_sbtrim, 0, "");

SYSCTL_PROC(_net_cfil, OID_AUTO, filter_list, CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_cfil_filter_list, "S,cfil_filter_stat", "");

SYSCTL_PROC(_net_cfil, OID_AUTO, sock_list, CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_cfil_sock_list, "S,cfil_sock_stat", "");

SYSCTL_STRUCT(_net_cfil, OID_AUTO, stats, CTLFLAG_RD | CTLFLAG_LOCKED,
    &cfil_stats, cfil_stats, "");

/*
 * Forward declaration to appease the compiler
 */
static int cfil_action_data_pass(struct socket *, struct cfil_info *, uint32_t, int,
    uint64_t, uint64_t);
static int cfil_action_drop(struct socket *, struct cfil_info *, uint32_t);
static int cfil_action_bless_client(uint32_t, struct cfil_msg_hdr *);
static int cfil_dispatch_closed_event(struct socket *, struct cfil_info *, int);
static int cfil_data_common(struct socket *, struct cfil_info *, int, struct sockaddr *,
    struct mbuf *, struct mbuf *, uint32_t);
static int cfil_data_filter(struct socket *, struct cfil_info *, uint32_t, int,
    struct mbuf *, uint64_t);
static void fill_ip_sockaddr_4_6(union sockaddr_in_4_6 *,
    struct in_addr, u_int16_t);
static void fill_ip6_sockaddr_4_6(union sockaddr_in_4_6 *,
    struct in6_addr *, u_int16_t);
;
static int cfil_dispatch_attach_event(struct socket *, struct cfil_info *, uint32_t);
static void cfil_info_free(struct cfil_info *);
static struct cfil_info * cfil_info_alloc(struct socket *, struct cfil_hash_entry *);
static int cfil_info_attach_unit(struct socket *, uint32_t, struct cfil_info *);
static struct socket * cfil_socket_from_sock_id(cfil_sock_id_t, bool);
static struct socket * cfil_socket_from_client_uuid(uuid_t, bool *);
static int cfil_service_pending_queue(struct socket *, struct cfil_info *, uint32_t, int);
static int cfil_data_service_ctl_q(struct socket *, struct cfil_info *, uint32_t, int);
static void cfil_info_verify(struct cfil_info *);
static int cfil_update_data_offsets(struct socket *, struct cfil_info *, uint32_t, int,
    uint64_t, uint64_t);
static int cfil_acquire_sockbuf(struct socket *, struct cfil_info *, int);
static void cfil_release_sockbuf(struct socket *, int);
static int cfil_filters_attached(struct socket *);

static void cfil_rw_lock_exclusive(lck_rw_t *);
static void cfil_rw_unlock_exclusive(lck_rw_t *);
static void cfil_rw_lock_shared(lck_rw_t *);
static void cfil_rw_unlock_shared(lck_rw_t *);
static boolean_t cfil_rw_lock_shared_to_exclusive(lck_rw_t *);
static void cfil_rw_lock_exclusive_to_shared(lck_rw_t *);

static unsigned int cfil_data_length(struct mbuf *, int *, int *);
static errno_t cfil_db_init(struct socket *);
static void cfil_db_free(struct socket *so);
struct cfil_hash_entry *cfil_db_lookup_entry(struct cfil_db *, struct sockaddr *, struct sockaddr *);
struct cfil_hash_entry *cfil_db_lookup_entry_with_sockid(struct cfil_db *, u_int64_t);
struct cfil_hash_entry *cfil_db_add_entry(struct cfil_db *, struct sockaddr *, struct sockaddr *);
void cfil_db_delete_entry(struct cfil_db *, struct cfil_hash_entry *);
struct cfil_hash_entry *cfil_sock_udp_get_flow(struct socket *, uint32_t, bool, struct sockaddr *, struct sockaddr *);
struct cfil_info *cfil_db_get_cfil_info(struct cfil_db *, cfil_sock_id_t);
static errno_t cfil_sock_udp_handle_data(bool, struct socket *, struct sockaddr *, struct sockaddr *,
    struct mbuf *, struct mbuf *, uint32_t);
static int32_t cfil_sock_udp_data_pending(struct sockbuf *, bool);
static void cfil_sock_udp_is_closed(struct socket *);
static int cfil_sock_udp_notify_shutdown(struct socket *, int, int, int);
static int cfil_sock_udp_shutdown(struct socket *, int *);
static void cfil_sock_udp_close_wait(struct socket *);
static void cfil_sock_udp_buf_update(struct sockbuf *);
static int cfil_filters_udp_attached(struct socket *, bool);
static void cfil_get_flow_address_v6(struct cfil_hash_entry *, struct inpcb *,
    struct in6_addr **, struct in6_addr **,
    u_int16_t *, u_int16_t *);
static void cfil_get_flow_address(struct cfil_hash_entry *, struct inpcb *,
    struct in_addr *, struct in_addr *,
    u_int16_t *, u_int16_t *);
static void cfil_info_log(int, struct cfil_info *, const char *);
void cfil_filter_show(u_int32_t);
void cfil_info_show(void);
bool cfil_info_idle_timed_out(struct cfil_info *, int, u_int32_t);
bool cfil_info_action_timed_out(struct cfil_info *, int);
bool cfil_info_buffer_threshold_exceeded(struct cfil_info *);
struct m_tag *cfil_udp_save_socket_state(struct cfil_info *, struct mbuf *);
static void cfil_udp_gc_thread_func(void *, wait_result_t);
static void cfil_info_udp_expire(void *, wait_result_t);

bool check_port(struct sockaddr *, u_short);

/*
 * Content filter global read write lock
 */

static void
cfil_rw_lock_exclusive(lck_rw_t *lck)
{
	void *lr_saved;

	lr_saved = __builtin_return_address(0);

	lck_rw_lock_exclusive(lck);

	cfil_rw_lock_history[cfil_rw_nxt_lck] = lr_saved;
	cfil_rw_nxt_lck = (cfil_rw_nxt_lck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_unlock_exclusive(lck_rw_t *lck)
{
	void *lr_saved;

	lr_saved = __builtin_return_address(0);

	lck_rw_unlock_exclusive(lck);

	cfil_rw_unlock_history[cfil_rw_nxt_unlck] = lr_saved;
	cfil_rw_nxt_unlck = (cfil_rw_nxt_unlck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_lock_shared(lck_rw_t *lck)
{
	void *lr_saved;

	lr_saved = __builtin_return_address(0);

	lck_rw_lock_shared(lck);

	cfil_rw_lock_history[cfil_rw_nxt_lck] = lr_saved;
	cfil_rw_nxt_lck = (cfil_rw_nxt_lck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_unlock_shared(lck_rw_t *lck)
{
	void *lr_saved;

	lr_saved = __builtin_return_address(0);

	lck_rw_unlock_shared(lck);

	cfil_rw_unlock_history[cfil_rw_nxt_unlck] = lr_saved;
	cfil_rw_nxt_unlck = (cfil_rw_nxt_unlck + 1) % CFIL_RW_LCK_MAX;
}

static boolean_t
cfil_rw_lock_shared_to_exclusive(lck_rw_t *lck)
{
	void *lr_saved;
	boolean_t upgraded;

	lr_saved = __builtin_return_address(0);

	upgraded = lck_rw_lock_shared_to_exclusive(lck);
	if (upgraded) {
		cfil_rw_unlock_history[cfil_rw_nxt_unlck] = lr_saved;
		cfil_rw_nxt_unlck = (cfil_rw_nxt_unlck + 1) % CFIL_RW_LCK_MAX;
	}
	return upgraded;
}

static void
cfil_rw_lock_exclusive_to_shared(lck_rw_t *lck)
{
	void *lr_saved;

	lr_saved = __builtin_return_address(0);

	lck_rw_lock_exclusive_to_shared(lck);

	cfil_rw_lock_history[cfil_rw_nxt_lck] = lr_saved;
	cfil_rw_nxt_lck = (cfil_rw_nxt_lck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_lock_assert_held(lck_rw_t *lck, int exclusive)
{
#if !MACH_ASSERT
#pragma unused(lck, exclusive)
#endif
	LCK_RW_ASSERT(lck,
	    exclusive ? LCK_RW_ASSERT_EXCLUSIVE : LCK_RW_ASSERT_HELD);
}

/*
 * Return the number of bytes in the mbuf chain using the same
 * method as m_length() or sballoc()
 *
 * Returns data len - starting from PKT start
 * - retmbcnt - optional param to get total mbuf bytes in chain
 * - retmbnum - optional param to get number of mbufs in chain
 */
static unsigned int
cfil_data_length(struct mbuf *m, int *retmbcnt, int *retmbnum)
{
	struct mbuf *m0;
	unsigned int pktlen = 0;
	int mbcnt;
	int mbnum;

	// Locate the start of data
	for (m0 = m; m0 != NULL; m0 = m0->m_next) {
		if (m0->m_flags & M_PKTHDR) {
			break;
		}
	}
	if (m0 == NULL) {
		CFIL_LOG(LOG_ERR, "cfil_data_length: no M_PKTHDR");
		return 0;
	}
	m = m0;

	if (retmbcnt == NULL && retmbnum == NULL) {
		return m_length(m);
	}

	pktlen = 0;
	mbcnt = 0;
	mbnum = 0;
	for (m0 = m; m0 != NULL; m0 = m0->m_next) {
		pktlen += m0->m_len;
		mbnum++;
		mbcnt += MSIZE;
		if (m0->m_flags & M_EXT) {
			mbcnt += m0->m_ext.ext_size;
		}
	}
	if (retmbcnt) {
		*retmbcnt = mbcnt;
	}
	if (retmbnum) {
		*retmbnum = mbnum;
	}
	return pktlen;
}

static struct mbuf *
cfil_data_start(struct mbuf *m)
{
	struct mbuf *m0;

	// Locate the start of data
	for (m0 = m; m0 != NULL; m0 = m0->m_next) {
		if (m0->m_flags & M_PKTHDR) {
			break;
		}
	}
	return m0;
}

/*
 * Common mbuf queue utilities
 */

static inline void
cfil_queue_init(struct cfil_queue *cfq)
{
	cfq->q_start = 0;
	cfq->q_end = 0;
	MBUFQ_INIT(&cfq->q_mq);
}

static inline uint64_t
cfil_queue_drain(struct cfil_queue *cfq)
{
	uint64_t drained = cfq->q_start - cfq->q_end;
	cfq->q_start = 0;
	cfq->q_end = 0;
	MBUFQ_DRAIN(&cfq->q_mq);

	return drained;
}

/* Return 1 when empty, 0 otherwise */
static inline int
cfil_queue_empty(struct cfil_queue *cfq)
{
	return MBUFQ_EMPTY(&cfq->q_mq);
}

static inline uint64_t
cfil_queue_offset_first(struct cfil_queue *cfq)
{
	return cfq->q_start;
}

static inline uint64_t
cfil_queue_offset_last(struct cfil_queue *cfq)
{
	return cfq->q_end;
}

static inline uint64_t
cfil_queue_len(struct cfil_queue *cfq)
{
	return cfq->q_end - cfq->q_start;
}

/*
 * Routines to verify some fundamental assumptions
 */

static void
cfil_queue_verify(struct cfil_queue *cfq)
{
	mbuf_t chain;
	mbuf_t m;
	mbuf_t n;
	uint64_t queuesize = 0;

	/* Verify offset are ordered */
	VERIFY(cfq->q_start <= cfq->q_end);

	/*
	 * When queue is empty, the offsets are equal otherwise the offsets
	 * are different
	 */
	VERIFY((MBUFQ_EMPTY(&cfq->q_mq) && cfq->q_start == cfq->q_end) ||
	    (!MBUFQ_EMPTY(&cfq->q_mq) &&
	    cfq->q_start != cfq->q_end));

	MBUFQ_FOREACH(chain, &cfq->q_mq) {
		size_t chainsize = 0;
		m = chain;
		unsigned int mlen = cfil_data_length(m, NULL, NULL);
		// skip the addr and control stuff if present
		m = cfil_data_start(m);

		if (m == NULL ||
		    m == (void *)M_TAG_FREE_PATTERN ||
		    m->m_next == (void *)M_TAG_FREE_PATTERN ||
		    m->m_nextpkt == (void *)M_TAG_FREE_PATTERN) {
			panic("%s - mq %p is free at %p", __func__,
			    &cfq->q_mq, m);
		}
		for (n = m; n != NULL; n = n->m_next) {
			if (n->m_type != MT_DATA &&
			    n->m_type != MT_HEADER &&
			    n->m_type != MT_OOBDATA) {
				panic("%s - %p unsupported type %u", __func__,
				    n, n->m_type);
			}
			chainsize += n->m_len;
		}
		if (mlen != chainsize) {
			panic("%s - %p m_length() %u != chainsize %lu",
			    __func__, m, mlen, chainsize);
		}
		queuesize += chainsize;
	}
	if (queuesize != cfq->q_end - cfq->q_start) {
		panic("%s - %p queuesize %llu != offsetdiffs %llu", __func__,
		    m, queuesize, cfq->q_end - cfq->q_start);
	}
}

static void
cfil_queue_enqueue(struct cfil_queue *cfq, mbuf_t m, size_t len)
{
	CFIL_QUEUE_VERIFY(cfq);

	MBUFQ_ENQUEUE(&cfq->q_mq, m);
	cfq->q_end += len;

	CFIL_QUEUE_VERIFY(cfq);
}

static void
cfil_queue_remove(struct cfil_queue *cfq, mbuf_t m, size_t len)
{
	CFIL_QUEUE_VERIFY(cfq);

	VERIFY(cfil_data_length(m, NULL, NULL) == len);

	MBUFQ_REMOVE(&cfq->q_mq, m);
	MBUFQ_NEXT(m) = NULL;
	cfq->q_start += len;

	CFIL_QUEUE_VERIFY(cfq);
}

static mbuf_t
cfil_queue_first(struct cfil_queue *cfq)
{
	return MBUFQ_FIRST(&cfq->q_mq);
}

static mbuf_t
cfil_queue_next(struct cfil_queue *cfq, mbuf_t m)
{
#pragma unused(cfq)
	return MBUFQ_NEXT(m);
}

static void
cfil_entry_buf_verify(struct cfe_buf *cfe_buf)
{
	CFIL_QUEUE_VERIFY(&cfe_buf->cfe_ctl_q);
	CFIL_QUEUE_VERIFY(&cfe_buf->cfe_pending_q);

	/* Verify the queues are ordered so that pending is before ctl */
	VERIFY(cfe_buf->cfe_ctl_q.q_start >= cfe_buf->cfe_pending_q.q_end);

	/* The peek offset cannot be less than the pass offset */
	VERIFY(cfe_buf->cfe_peek_offset >= cfe_buf->cfe_pass_offset);

	/* Make sure we've updated the offset we peeked at  */
	VERIFY(cfe_buf->cfe_ctl_q.q_start <= cfe_buf->cfe_peeked);
}

static void
cfil_entry_verify(struct cfil_entry *entry)
{
	cfil_entry_buf_verify(&entry->cfe_snd);
	cfil_entry_buf_verify(&entry->cfe_rcv);
}

static void
cfil_info_buf_verify(struct cfi_buf *cfi_buf)
{
	CFIL_QUEUE_VERIFY(&cfi_buf->cfi_inject_q);

	VERIFY(cfi_buf->cfi_pending_first <= cfi_buf->cfi_pending_last);
	VERIFY(cfi_buf->cfi_pending_mbcnt >= 0);
}

static void
cfil_info_verify(struct cfil_info *cfil_info)
{
	int i;

	if (cfil_info == NULL) {
		return;
	}

	cfil_info_buf_verify(&cfil_info->cfi_snd);
	cfil_info_buf_verify(&cfil_info->cfi_rcv);

	for (i = 0; i < MAX_CONTENT_FILTER; i++) {
		cfil_entry_verify(&cfil_info->cfi_entries[i]);
	}
}

static void
verify_content_filter(struct content_filter *cfc)
{
	struct cfil_entry *entry;
	uint32_t count = 0;

	VERIFY(cfc->cf_sock_count >= 0);

	TAILQ_FOREACH(entry, &cfc->cf_sock_entries, cfe_link) {
		count++;
		VERIFY(cfc == entry->cfe_filter);
	}
	VERIFY(count == cfc->cf_sock_count);
}

/*
 * Kernel control socket callbacks
 */
static errno_t
cfil_ctl_connect(kern_ctl_ref kctlref, struct sockaddr_ctl *sac,
    void **unitinfo)
{
	errno_t error = 0;
	struct content_filter *cfc = NULL;

	CFIL_LOG(LOG_NOTICE, "");

	cfc = zalloc(content_filter_zone);
	if (cfc == NULL) {
		CFIL_LOG(LOG_ERR, "zalloc failed");
		error = ENOMEM;
		goto done;
	}
	bzero(cfc, sizeof(struct content_filter));

	cfil_rw_lock_exclusive(&cfil_lck_rw);
	if (content_filters == NULL) {
		struct content_filter **tmp;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);

		MALLOC(tmp,
		    struct content_filter **,
		    MAX_CONTENT_FILTER * sizeof(struct content_filter *),
		    M_TEMP,
		    M_WAITOK | M_ZERO);

		cfil_rw_lock_exclusive(&cfil_lck_rw);

		if (tmp == NULL && content_filters == NULL) {
			error = ENOMEM;
			cfil_rw_unlock_exclusive(&cfil_lck_rw);
			goto done;
		}
		/* Another thread may have won the race */
		if (content_filters != NULL) {
			FREE(tmp, M_TEMP);
		} else {
			content_filters = tmp;
		}
	}

	if (sac->sc_unit == 0 || sac->sc_unit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "bad sc_unit %u", sac->sc_unit);
		error = EINVAL;
	} else if (content_filters[sac->sc_unit - 1] != NULL) {
		CFIL_LOG(LOG_ERR, "sc_unit %u in use", sac->sc_unit);
		error = EADDRINUSE;
	} else {
		/*
		 * kernel control socket kcunit numbers start at 1
		 */
		content_filters[sac->sc_unit - 1] = cfc;

		cfc->cf_kcref = kctlref;
		cfc->cf_kcunit = sac->sc_unit;
		TAILQ_INIT(&cfc->cf_sock_entries);

		*unitinfo = cfc;
		cfil_active_count++;
	}
	cfil_rw_unlock_exclusive(&cfil_lck_rw);
done:
	if (error != 0 && cfc != NULL) {
		zfree(content_filter_zone, cfc);
	}

	if (error == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_connect_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_connect_fail);
	}

	CFIL_LOG(LOG_INFO, "return %d cfil_active_count %u kcunit %u",
	    error, cfil_active_count, sac->sc_unit);

	return error;
}

static errno_t
cfil_ctl_disconnect(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo)
{
#pragma unused(kctlref)
	errno_t error = 0;
	struct content_filter *cfc;
	struct cfil_entry *entry;
	uint64_t sock_flow_id = 0;

	CFIL_LOG(LOG_NOTICE, "");

	if (content_filters == NULL) {
		CFIL_LOG(LOG_ERR, "no content filter");
		error = EINVAL;
		goto done;
	}
	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}

	cfc = (struct content_filter *)unitinfo;
	if (cfc == NULL) {
		goto done;
	}

	cfil_rw_lock_exclusive(&cfil_lck_rw);
	if (content_filters[kcunit - 1] != cfc || cfc->cf_kcunit != kcunit) {
		CFIL_LOG(LOG_ERR, "bad unit info %u)",
		    kcunit);
		cfil_rw_unlock_exclusive(&cfil_lck_rw);
		goto done;
	}
	cfc->cf_flags |= CFF_DETACHING;
	/*
	 * Remove all sockets from the filter
	 */
	while ((entry = TAILQ_FIRST(&cfc->cf_sock_entries)) != NULL) {
		cfil_rw_lock_assert_held(&cfil_lck_rw, 1);

		verify_content_filter(cfc);
		/*
		 * Accept all outstanding data by pushing to next filter
		 * or back to socket
		 *
		 * TBD: Actually we should make sure all data has been pushed
		 * back to socket
		 */
		if (entry->cfe_cfil_info && entry->cfe_cfil_info->cfi_so) {
			struct cfil_info *cfil_info = entry->cfe_cfil_info;
			struct socket *so = cfil_info->cfi_so;
			sock_flow_id = cfil_info->cfi_sock_id;

			/* Need to let data flow immediately */
			entry->cfe_flags |= CFEF_SENT_SOCK_ATTACHED |
			    CFEF_DATA_START;

			/*
			 * Respect locking hierarchy
			 */
			cfil_rw_unlock_exclusive(&cfil_lck_rw);

			socket_lock(so, 1);

			/*
			 * When cfe_filter is NULL the filter is detached
			 * and the entry has been removed from cf_sock_entries
			 */
			if ((so->so_cfil == NULL && so->so_cfil_db == NULL) || entry->cfe_filter == NULL) {
				cfil_rw_lock_exclusive(&cfil_lck_rw);
				goto release;
			}

			(void) cfil_action_data_pass(so, cfil_info, kcunit, 1,
			    CFM_MAX_OFFSET,
			    CFM_MAX_OFFSET);

			(void) cfil_action_data_pass(so, cfil_info, kcunit, 0,
			    CFM_MAX_OFFSET,
			    CFM_MAX_OFFSET);

			cfil_rw_lock_exclusive(&cfil_lck_rw);

			/*
			 * Check again to make sure if the cfil_info is still valid
			 * as the socket may have been unlocked when when calling
			 * cfil_acquire_sockbuf()
			 */
			if (entry->cfe_filter == NULL ||
			    (so->so_cfil == NULL && cfil_db_get_cfil_info(so->so_cfil_db, sock_flow_id) == NULL)) {
				goto release;
			}

			/* The filter is now detached */
			entry->cfe_flags |= CFEF_CFIL_DETACHED;
#if LIFECYCLE_DEBUG
			cfil_info_log(LOG_DEBUG, cfil_info, "CFIL: LIFECYCLE: - FILTER DISCONNECTED");
#endif
			CFIL_LOG(LOG_NOTICE, "so %llx detached %u",
			    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);
			if ((cfil_info->cfi_flags & CFIF_CLOSE_WAIT) &&
			    cfil_filters_attached(so) == 0) {
				CFIL_LOG(LOG_NOTICE, "so %llx waking",
				    (uint64_t)VM_KERNEL_ADDRPERM(so));
				wakeup((caddr_t)cfil_info);
			}

			/*
			 * Remove the filter entry from the content filter
			 * but leave the rest of the state intact as the queues
			 * may not be empty yet
			 */
			entry->cfe_filter = NULL;
			entry->cfe_necp_control_unit = 0;

			TAILQ_REMOVE(&cfc->cf_sock_entries, entry, cfe_link);
			cfc->cf_sock_count--;
release:
			socket_unlock(so, 1);
		}
	}
	verify_content_filter(cfc);

	VERIFY(cfc->cf_sock_count == 0);

	/*
	 * Make filter inactive
	 */
	content_filters[kcunit - 1] = NULL;
	cfil_active_count--;
	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	zfree(content_filter_zone, cfc);
done:
	if (error == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_disconnect_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_disconnect_fail);
	}

	CFIL_LOG(LOG_INFO, "return %d cfil_active_count %u kcunit %u",
	    error, cfil_active_count, kcunit);

	return error;
}

/*
 * cfil_acquire_sockbuf()
 *
 * Prevent any other thread from acquiring the sockbuf
 * We use sb_cfil_thread as a semaphore to prevent other threads from
 * messing with the sockbuf -- see sblock()
 * Note: We do not set SB_LOCK here because the thread may check or modify
 * SB_LOCK several times until it calls cfil_release_sockbuf() -- currently
 * sblock(), sbunlock() or sodefunct()
 */
static int
cfil_acquire_sockbuf(struct socket *so, struct cfil_info *cfil_info, int outgoing)
{
	thread_t tp = current_thread();
	struct sockbuf *sb = outgoing ? &so->so_snd : &so->so_rcv;
	lck_mtx_t *mutex_held;
	int error = 0;

	/*
	 * Wait until no thread is holding the sockbuf and other content
	 * filter threads have released the sockbuf
	 */
	while ((sb->sb_flags & SB_LOCK) ||
	    (sb->sb_cfil_thread != NULL && sb->sb_cfil_thread != tp)) {
		if (so->so_proto->pr_getlock != NULL) {
			mutex_held = (*so->so_proto->pr_getlock)(so, PR_F_WILLUNLOCK);
		} else {
			mutex_held = so->so_proto->pr_domain->dom_mtx;
		}

		LCK_MTX_ASSERT(mutex_held, LCK_MTX_ASSERT_OWNED);

		sb->sb_wantlock++;
		VERIFY(sb->sb_wantlock != 0);

		msleep(&sb->sb_flags, mutex_held, PSOCK, "cfil_acquire_sockbuf",
		    NULL);

		VERIFY(sb->sb_wantlock != 0);
		sb->sb_wantlock--;
	}
	/*
	 * Use reference count for repetitive calls on same thread
	 */
	if (sb->sb_cfil_refs == 0) {
		VERIFY(sb->sb_cfil_thread == NULL);
		VERIFY((sb->sb_flags & SB_LOCK) == 0);

		sb->sb_cfil_thread = tp;
		sb->sb_flags |= SB_LOCK;
	}
	sb->sb_cfil_refs++;

	/* We acquire the socket buffer when we need to cleanup */
	if (cfil_info == NULL) {
		CFIL_LOG(LOG_ERR, "so %llx cfil detached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = 0;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EPIPE;
	}

	return error;
}

static void
cfil_release_sockbuf(struct socket *so, int outgoing)
{
	struct sockbuf *sb = outgoing ? &so->so_snd : &so->so_rcv;
	thread_t tp = current_thread();

	socket_lock_assert_owned(so);

	if (sb->sb_cfil_thread != NULL && sb->sb_cfil_thread != tp) {
		panic("%s sb_cfil_thread %p not current %p", __func__,
		    sb->sb_cfil_thread, tp);
	}
	/*
	 * Don't panic if we are defunct because SB_LOCK has
	 * been cleared by sodefunct()
	 */
	if (!(so->so_flags & SOF_DEFUNCT) && !(sb->sb_flags & SB_LOCK)) {
		panic("%s SB_LOCK not set on %p", __func__,
		    sb);
	}
	/*
	 * We can unlock when the thread unwinds to the last reference
	 */
	sb->sb_cfil_refs--;
	if (sb->sb_cfil_refs == 0) {
		sb->sb_cfil_thread = NULL;
		sb->sb_flags &= ~SB_LOCK;

		if (sb->sb_wantlock > 0) {
			wakeup(&sb->sb_flags);
		}
	}
}

cfil_sock_id_t
cfil_sock_id_from_socket(struct socket *so)
{
	if ((so->so_flags & SOF_CONTENT_FILTER) && so->so_cfil) {
		return so->so_cfil->cfi_sock_id;
	} else {
		return CFIL_SOCK_ID_NONE;
	}
}

static bool
cfil_socket_safe_lock(struct inpcb *inp)
{
	if (in_pcb_checkstate(inp, WNT_ACQUIRE, 0) != WNT_STOPUSING) {
		socket_lock(inp->inp_socket, 1);
		if (in_pcb_checkstate(inp, WNT_RELEASE, 1) != WNT_STOPUSING) {
			return true;
		}
		socket_unlock(inp->inp_socket, 1);
	}
	return false;
}

static struct socket *
cfil_socket_from_sock_id(cfil_sock_id_t cfil_sock_id, bool udp_only)
{
	struct socket *so = NULL;
	u_int64_t gencnt = cfil_sock_id >> 32;
	u_int32_t flowhash = (u_int32_t)(cfil_sock_id & 0x0ffffffff);
	struct inpcb *inp = NULL;
	struct inpcbinfo *pcbinfo = NULL;

#if VERDICT_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: VERDICT: search for socket: id %llu gencnt %llx flowhash %x", cfil_sock_id, gencnt, flowhash);
#endif

	if (udp_only) {
		goto find_udp;
	}

	pcbinfo = &tcbinfo;
	lck_rw_lock_shared(pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    inp->inp_flowhash == flowhash &&
		    (inp->inp_socket->so_gencnt & 0x0ffffffff) == gencnt &&
		    inp->inp_socket->so_cfil != NULL) {
			if (cfil_socket_safe_lock(inp)) {
				so = inp->inp_socket;
			}
			break;
		}
	}
	lck_rw_done(pcbinfo->ipi_lock);
	if (so != NULL) {
		goto done;
	}

find_udp:

	pcbinfo = &udbinfo;
	lck_rw_lock_shared(pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    inp->inp_socket->so_cfil_db != NULL &&
		    (inp->inp_socket->so_gencnt & 0x0ffffffff) == gencnt) {
			if (cfil_socket_safe_lock(inp)) {
				so = inp->inp_socket;
			}
			break;
		}
	}
	lck_rw_done(pcbinfo->ipi_lock);

done:
	if (so == NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_id_not_found);
		CFIL_LOG(LOG_DEBUG,
		    "no socket for sock_id %llx gencnt %llx flowhash %x",
		    cfil_sock_id, gencnt, flowhash);
	}

	return so;
}

static struct socket *
cfil_socket_from_client_uuid(uuid_t necp_client_uuid, bool *cfil_attached)
{
	struct socket *so = NULL;
	struct inpcb *inp = NULL;
	struct inpcbinfo *pcbinfo = &tcbinfo;

	lck_rw_lock_shared(pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    uuid_compare(inp->necp_client_uuid, necp_client_uuid) == 0) {
			*cfil_attached = (inp->inp_socket->so_cfil != NULL);
			if (cfil_socket_safe_lock(inp)) {
				so = inp->inp_socket;
			}
			break;
		}
	}
	lck_rw_done(pcbinfo->ipi_lock);
	if (so != NULL) {
		goto done;
	}

	pcbinfo = &udbinfo;
	lck_rw_lock_shared(pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    uuid_compare(inp->necp_client_uuid, necp_client_uuid) == 0) {
			*cfil_attached = (inp->inp_socket->so_cfil_db != NULL);
			if (cfil_socket_safe_lock(inp)) {
				so = inp->inp_socket;
			}
			break;
		}
	}
	lck_rw_done(pcbinfo->ipi_lock);

done:
	return so;
}

static errno_t
cfil_ctl_send(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo, mbuf_t m,
    int flags)
{
#pragma unused(kctlref, flags)
	errno_t error = 0;
	struct cfil_msg_hdr *msghdr;
	struct content_filter *cfc = (struct content_filter *)unitinfo;
	struct socket *so;
	struct cfil_msg_action *action_msg;
	struct cfil_entry *entry;
	struct cfil_info *cfil_info = NULL;

	CFIL_LOG(LOG_INFO, "");

	if (content_filters == NULL) {
		CFIL_LOG(LOG_ERR, "no content filter");
		error = EINVAL;
		goto done;
	}
	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}

	if (m_length(m) < sizeof(struct cfil_msg_hdr)) {
		CFIL_LOG(LOG_ERR, "too short %u", m_length(m));
		error = EINVAL;
		goto done;
	}
	msghdr = (struct cfil_msg_hdr *)mbuf_data(m);
	if (msghdr->cfm_version != CFM_VERSION_CURRENT) {
		CFIL_LOG(LOG_ERR, "bad version %u", msghdr->cfm_version);
		error = EINVAL;
		goto done;
	}
	if (msghdr->cfm_type != CFM_TYPE_ACTION) {
		CFIL_LOG(LOG_ERR, "bad type %u", msghdr->cfm_type);
		error = EINVAL;
		goto done;
	}
	/* Validate action operation */
	switch (msghdr->cfm_op) {
	case CFM_OP_DATA_UPDATE:
		OSIncrementAtomic(
			&cfil_stats.cfs_ctl_action_data_update);
		break;
	case CFM_OP_DROP:
		OSIncrementAtomic(&cfil_stats.cfs_ctl_action_drop);
		break;
	case CFM_OP_BLESS_CLIENT:
		if (msghdr->cfm_len != sizeof(struct cfil_msg_bless_client)) {
			OSIncrementAtomic(&cfil_stats.cfs_ctl_action_bad_len);
			error = EINVAL;
			CFIL_LOG(LOG_ERR, "bad len: %u for op %u",
			    msghdr->cfm_len,
			    msghdr->cfm_op);
			goto done;
		}
		error = cfil_action_bless_client(kcunit, msghdr);
		goto done;
	default:
		OSIncrementAtomic(&cfil_stats.cfs_ctl_action_bad_op);
		CFIL_LOG(LOG_ERR, "bad op %u", msghdr->cfm_op);
		error = EINVAL;
		goto done;
	}
	if (msghdr->cfm_len != sizeof(struct cfil_msg_action)) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_action_bad_len);
		error = EINVAL;
		CFIL_LOG(LOG_ERR, "bad len: %u for op %u",
		    msghdr->cfm_len,
		    msghdr->cfm_op);
		goto done;
	}
	cfil_rw_lock_shared(&cfil_lck_rw);
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		error = EINVAL;
		cfil_rw_unlock_shared(&cfil_lck_rw);
		goto done;
	}
	cfil_rw_unlock_shared(&cfil_lck_rw);

	// Search for socket (TCP+UDP and lock so)
	so = cfil_socket_from_sock_id(msghdr->cfm_sock_id, false);
	if (so == NULL) {
		CFIL_LOG(LOG_NOTICE, "bad sock_id %llx",
		    msghdr->cfm_sock_id);
		error = EINVAL;
		goto done;
	}

	cfil_info = so->so_cfil_db != NULL ?
	    cfil_db_get_cfil_info(so->so_cfil_db, msghdr->cfm_sock_id) : so->so_cfil;

	if (cfil_info == NULL) {
		CFIL_LOG(LOG_NOTICE, "so %llx <id %llu> not attached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), msghdr->cfm_sock_id);
		error = EINVAL;
		goto unlock;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_NOTICE, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EINVAL;
		goto unlock;
	}
	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (entry->cfe_filter == NULL) {
		CFIL_LOG(LOG_NOTICE, "so %llx no filter",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EINVAL;
		goto unlock;
	}

	if (entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) {
		entry->cfe_flags |= CFEF_DATA_START;
	} else {
		CFIL_LOG(LOG_ERR,
		    "so %llx attached not sent for %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);
		error = EINVAL;
		goto unlock;
	}

	microuptime(&entry->cfe_last_action);
	CFI_ADD_TIME_LOG(cfil_info, &entry->cfe_last_action, &cfil_info->cfi_first_event, msghdr->cfm_op);

	action_msg = (struct cfil_msg_action *)msghdr;

	switch (msghdr->cfm_op) {
	case CFM_OP_DATA_UPDATE:
#if VERDICT_DEBUG
		CFIL_LOG(LOG_ERR, "CFIL: VERDICT RECEIVED: <so %llx sockID %llu> <IN peek:%llu pass:%llu, OUT peek:%llu pass:%llu>",
		    (uint64_t)VM_KERNEL_ADDRPERM(so),
		    cfil_info->cfi_sock_id,
		    action_msg->cfa_in_peek_offset, action_msg->cfa_in_pass_offset,
		    action_msg->cfa_out_peek_offset, action_msg->cfa_out_pass_offset);
#endif
		if (action_msg->cfa_out_peek_offset != 0 ||
		    action_msg->cfa_out_pass_offset != 0) {
			error = cfil_action_data_pass(so, cfil_info, kcunit, 1,
			    action_msg->cfa_out_pass_offset,
			    action_msg->cfa_out_peek_offset);
		}
		if (error == EJUSTRETURN) {
			error = 0;
		}
		if (error != 0) {
			break;
		}
		if (action_msg->cfa_in_peek_offset != 0 ||
		    action_msg->cfa_in_pass_offset != 0) {
			error = cfil_action_data_pass(so, cfil_info, kcunit, 0,
			    action_msg->cfa_in_pass_offset,
			    action_msg->cfa_in_peek_offset);
		}
		if (error == EJUSTRETURN) {
			error = 0;
		}
		break;

	case CFM_OP_DROP:
		error = cfil_action_drop(so, cfil_info, kcunit);
		break;

	default:
		error = EINVAL;
		break;
	}
unlock:
	socket_unlock(so, 1);
done:
	mbuf_freem(m);

	if (error == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_send_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_send_bad);
	}

	return error;
}

static errno_t
cfil_ctl_getopt(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo,
    int opt, void *data, size_t *len)
{
#pragma unused(kctlref, opt)
	struct cfil_info *cfil_info = NULL;
	errno_t error = 0;
	struct content_filter *cfc = (struct content_filter *)unitinfo;

	CFIL_LOG(LOG_NOTICE, "");

	cfil_rw_lock_shared(&cfil_lck_rw);

	if (content_filters == NULL) {
		CFIL_LOG(LOG_ERR, "no content filter");
		error = EINVAL;
		goto done;
	}
	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		error = EINVAL;
		goto done;
	}
	switch (opt) {
	case CFIL_OPT_NECP_CONTROL_UNIT:
		if (*len < sizeof(uint32_t)) {
			CFIL_LOG(LOG_ERR, "len too small %lu", *len);
			error = EINVAL;
			goto done;
		}
		if (data != NULL) {
			*(uint32_t *)data = cfc->cf_necp_control_unit;
		}
		break;
	case CFIL_OPT_GET_SOCKET_INFO:
		if (*len != sizeof(struct cfil_opt_sock_info)) {
			CFIL_LOG(LOG_ERR, "len does not match %lu", *len);
			error = EINVAL;
			goto done;
		}
		if (data == NULL) {
			CFIL_LOG(LOG_ERR, "data not passed");
			error = EINVAL;
			goto done;
		}

		struct cfil_opt_sock_info *sock_info =
		    (struct cfil_opt_sock_info *) data;

		// Unlock here so that we never hold both cfil_lck_rw and the
		// socket_lock at the same time. Otherwise, this can deadlock
		// because soclose() takes the socket_lock and then exclusive
		// cfil_lck_rw and we require the opposite order.

		// WARNING: Be sure to never use anything protected
		//     by cfil_lck_rw beyond this point.
		// WARNING: Be sure to avoid fallthrough and
		//     goto return_already_unlocked from this branch.
		cfil_rw_unlock_shared(&cfil_lck_rw);

		// Search (TCP+UDP) and lock socket
		struct socket *sock =
		    cfil_socket_from_sock_id(sock_info->cfs_sock_id, false);
		if (sock == NULL) {
#if LIFECYCLE_DEBUG
			CFIL_LOG(LOG_ERR, "CFIL: GET_SOCKET_INFO failed: bad sock_id %llu",
			    sock_info->cfs_sock_id);
#endif
			error = ENOENT;
			goto return_already_unlocked;
		}

		cfil_info = (sock->so_cfil_db != NULL) ?
		    cfil_db_get_cfil_info(sock->so_cfil_db, sock_info->cfs_sock_id) : sock->so_cfil;

		if (cfil_info == NULL) {
#if LIFECYCLE_DEBUG
			CFIL_LOG(LOG_ERR, "CFIL: GET_SOCKET_INFO failed: so %llx not attached, cannot fetch info",
			    (uint64_t)VM_KERNEL_ADDRPERM(sock));
#endif
			error = EINVAL;
			socket_unlock(sock, 1);
			goto return_already_unlocked;
		}

		// Fill out family, type, and protocol
		sock_info->cfs_sock_family = sock->so_proto->pr_domain->dom_family;
		sock_info->cfs_sock_type = sock->so_proto->pr_type;
		sock_info->cfs_sock_protocol = sock->so_proto->pr_protocol;

		// Source and destination addresses
		struct inpcb *inp = sotoinpcb(sock);
		if (inp->inp_vflag & INP_IPV6) {
			struct in6_addr *laddr = NULL, *faddr = NULL;
			u_int16_t lport = 0, fport = 0;

			cfil_get_flow_address_v6(cfil_info->cfi_hash_entry, inp,
			    &laddr, &faddr, &lport, &fport);
			fill_ip6_sockaddr_4_6(&sock_info->cfs_local, laddr, lport);
			fill_ip6_sockaddr_4_6(&sock_info->cfs_remote, faddr, fport);
		} else if (inp->inp_vflag & INP_IPV4) {
			struct in_addr laddr = {0}, faddr = {0};
			u_int16_t lport = 0, fport = 0;

			cfil_get_flow_address(cfil_info->cfi_hash_entry, inp,
			    &laddr, &faddr, &lport, &fport);
			fill_ip_sockaddr_4_6(&sock_info->cfs_local, laddr, lport);
			fill_ip_sockaddr_4_6(&sock_info->cfs_remote, faddr, fport);
		}

		// Set the pid info
		sock_info->cfs_pid = sock->last_pid;
		memcpy(sock_info->cfs_uuid, sock->last_uuid, sizeof(uuid_t));

		if (sock->so_flags & SOF_DELEGATED) {
			sock_info->cfs_e_pid = sock->e_pid;
			memcpy(sock_info->cfs_e_uuid, sock->e_uuid, sizeof(uuid_t));
		} else {
			sock_info->cfs_e_pid = sock->last_pid;
			memcpy(sock_info->cfs_e_uuid, sock->last_uuid, sizeof(uuid_t));
		}

		socket_unlock(sock, 1);

		goto return_already_unlocked;
	default:
		error = ENOPROTOOPT;
		break;
	}
done:
	cfil_rw_unlock_shared(&cfil_lck_rw);

	return error;

return_already_unlocked:

	return error;
}

static errno_t
cfil_ctl_setopt(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo,
    int opt, void *data, size_t len)
{
#pragma unused(kctlref, opt)
	errno_t error = 0;
	struct content_filter *cfc = (struct content_filter *)unitinfo;

	CFIL_LOG(LOG_NOTICE, "");

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	if (content_filters == NULL) {
		CFIL_LOG(LOG_ERR, "no content filter");
		error = EINVAL;
		goto done;
	}
	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		error = EINVAL;
		goto done;
	}
	switch (opt) {
	case CFIL_OPT_NECP_CONTROL_UNIT:
		if (len < sizeof(uint32_t)) {
			CFIL_LOG(LOG_ERR, "CFIL_OPT_NECP_CONTROL_UNIT "
			    "len too small %lu", len);
			error = EINVAL;
			goto done;
		}
		if (cfc->cf_necp_control_unit != 0) {
			CFIL_LOG(LOG_ERR, "CFIL_OPT_NECP_CONTROL_UNIT "
			    "already set %u",
			    cfc->cf_necp_control_unit);
			error = EINVAL;
			goto done;
		}
		cfc->cf_necp_control_unit = *(uint32_t *)data;
		break;
	default:
		error = ENOPROTOOPT;
		break;
	}
done:
	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	return error;
}


static void
cfil_ctl_rcvd(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo, int flags)
{
#pragma unused(kctlref, flags)
	struct content_filter *cfc = (struct content_filter *)unitinfo;
	struct socket *so = NULL;
	int error;
	struct cfil_entry *entry;
	struct cfil_info *cfil_info = NULL;

	CFIL_LOG(LOG_INFO, "");

	if (content_filters == NULL) {
		CFIL_LOG(LOG_ERR, "no content filter");
		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_bad);
		return;
	}
	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_bad);
		return;
	}
	cfil_rw_lock_shared(&cfil_lck_rw);
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_bad);
		goto done;
	}
	/* Let's assume the flow control is lifted */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags &= ~CFF_FLOW_CONTROLLED;

		cfil_rw_lock_exclusive_to_shared(&cfil_lck_rw);
		LCK_RW_ASSERT(&cfil_lck_rw, LCK_RW_ASSERT_SHARED);
	}
	/*
	 * Flow control will be raised again as soon as an entry cannot enqueue
	 * to the kernel control socket
	 */
	while ((cfc->cf_flags & CFF_FLOW_CONTROLLED) == 0) {
		verify_content_filter(cfc);

		cfil_rw_lock_assert_held(&cfil_lck_rw, 0);

		/* Find an entry that is flow controlled */
		TAILQ_FOREACH(entry, &cfc->cf_sock_entries, cfe_link) {
			if (entry->cfe_cfil_info == NULL ||
			    entry->cfe_cfil_info->cfi_so == NULL) {
				continue;
			}
			if ((entry->cfe_flags & CFEF_FLOW_CONTROLLED) == 0) {
				continue;
			}
		}
		if (entry == NULL) {
			break;
		}

		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_flow_lift);

		cfil_info = entry->cfe_cfil_info;
		so = cfil_info->cfi_so;

		cfil_rw_unlock_shared(&cfil_lck_rw);
		socket_lock(so, 1);

		do {
			error = cfil_acquire_sockbuf(so, cfil_info, 1);
			if (error == 0) {
				error = cfil_data_service_ctl_q(so, cfil_info, kcunit, 1);
			}
			cfil_release_sockbuf(so, 1);
			if (error != 0) {
				break;
			}

			error = cfil_acquire_sockbuf(so, cfil_info, 0);
			if (error == 0) {
				error = cfil_data_service_ctl_q(so, cfil_info, kcunit, 0);
			}
			cfil_release_sockbuf(so, 0);
		} while (0);

		socket_lock_assert_owned(so);
		socket_unlock(so, 1);

		cfil_rw_lock_shared(&cfil_lck_rw);
	}
done:
	cfil_rw_unlock_shared(&cfil_lck_rw);
}

void
cfil_init(void)
{
	struct kern_ctl_reg kern_ctl;
	errno_t error = 0;
	vm_size_t content_filter_size = 0;      /* size of content_filter */
	vm_size_t cfil_info_size = 0;   /* size of cfil_info */
	vm_size_t cfil_hash_entry_size = 0; /* size of cfil_hash_entry */
	vm_size_t cfil_db_size = 0; /* size of cfil_db */
	unsigned int mbuf_limit = 0;

	CFIL_LOG(LOG_NOTICE, "");

	/*
	 * Compile time verifications
	 */
	_CASSERT(CFIL_MAX_FILTER_COUNT == MAX_CONTENT_FILTER);
	_CASSERT(sizeof(struct cfil_filter_stat) % sizeof(uint32_t) == 0);
	_CASSERT(sizeof(struct cfil_entry_stat) % sizeof(uint32_t) == 0);
	_CASSERT(sizeof(struct cfil_sock_stat) % sizeof(uint32_t) == 0);

	/*
	 * Runtime time verifications
	 */
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_in_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_out_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_in_peeked,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_out_peeked,
	    sizeof(uint32_t)));

	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_pending_q_in_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_pending_q_out_enqueued,
	    sizeof(uint32_t)));

	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_in_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_out_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_in_passed,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_out_passed,
	    sizeof(uint32_t)));

	/*
	 * Zone for content filters kernel control sockets
	 */
	content_filter_size = sizeof(struct content_filter);
	content_filter_zone = zinit(content_filter_size,
	    CONTENT_FILTER_ZONE_MAX * content_filter_size,
	    0,
	    CONTENT_FILTER_ZONE_NAME);
	if (content_filter_zone == NULL) {
		panic("%s: zinit(%s) failed", __func__,
		    CONTENT_FILTER_ZONE_NAME);
		/* NOTREACHED */
	}
	zone_change(content_filter_zone, Z_CALLERACCT, FALSE);
	zone_change(content_filter_zone, Z_EXPAND, TRUE);

	/*
	 * Zone for per socket content filters
	 */
	cfil_info_size = sizeof(struct cfil_info);
	cfil_info_zone = zinit(cfil_info_size,
	    CFIL_INFO_ZONE_MAX * cfil_info_size,
	    0,
	    CFIL_INFO_ZONE_NAME);
	if (cfil_info_zone == NULL) {
		panic("%s: zinit(%s) failed", __func__, CFIL_INFO_ZONE_NAME);
		/* NOTREACHED */
	}
	zone_change(cfil_info_zone, Z_CALLERACCT, FALSE);
	zone_change(cfil_info_zone, Z_EXPAND, TRUE);

	/*
	 * Zone for content filters cfil hash entries and db
	 */
	cfil_hash_entry_size = sizeof(struct cfil_hash_entry);
	cfil_hash_entry_zone = zinit(cfil_hash_entry_size,
	    CFIL_HASH_ENTRY_ZONE_MAX * cfil_hash_entry_size,
	    0,
	    CFIL_HASH_ENTRY_ZONE_NAME);
	if (cfil_hash_entry_zone == NULL) {
		panic("%s: zinit(%s) failed", __func__, CFIL_HASH_ENTRY_ZONE_NAME);
		/* NOTREACHED */
	}
	zone_change(cfil_hash_entry_zone, Z_CALLERACCT, FALSE);
	zone_change(cfil_hash_entry_zone, Z_EXPAND, TRUE);

	cfil_db_size = sizeof(struct cfil_db);
	cfil_db_zone = zinit(cfil_db_size,
	    CFIL_DB_ZONE_MAX * cfil_db_size,
	    0,
	    CFIL_DB_ZONE_NAME);
	if (cfil_db_zone == NULL) {
		panic("%s: zinit(%s) failed", __func__, CFIL_DB_ZONE_NAME);
		/* NOTREACHED */
	}
	zone_change(cfil_db_zone, Z_CALLERACCT, FALSE);
	zone_change(cfil_db_zone, Z_EXPAND, TRUE);

	/*
	 * Allocate locks
	 */
	cfil_lck_grp_attr = lck_grp_attr_alloc_init();
	if (cfil_lck_grp_attr == NULL) {
		panic("%s: lck_grp_attr_alloc_init failed", __func__);
		/* NOTREACHED */
	}
	cfil_lck_grp = lck_grp_alloc_init("content filter",
	    cfil_lck_grp_attr);
	if (cfil_lck_grp == NULL) {
		panic("%s: lck_grp_alloc_init failed", __func__);
		/* NOTREACHED */
	}
	cfil_lck_attr = lck_attr_alloc_init();
	if (cfil_lck_attr == NULL) {
		panic("%s: lck_attr_alloc_init failed", __func__);
		/* NOTREACHED */
	}
	lck_rw_init(&cfil_lck_rw, cfil_lck_grp, cfil_lck_attr);

	TAILQ_INIT(&cfil_sock_head);

	/*
	 * Register kernel control
	 */
	bzero(&kern_ctl, sizeof(kern_ctl));
	strlcpy(kern_ctl.ctl_name, CONTENT_FILTER_CONTROL_NAME,
	    sizeof(kern_ctl.ctl_name));
	kern_ctl.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
	kern_ctl.ctl_sendsize = 512 * 1024; /* enough? */
	kern_ctl.ctl_recvsize = 512 * 1024; /* enough? */
	kern_ctl.ctl_connect = cfil_ctl_connect;
	kern_ctl.ctl_disconnect = cfil_ctl_disconnect;
	kern_ctl.ctl_send = cfil_ctl_send;
	kern_ctl.ctl_getopt = cfil_ctl_getopt;
	kern_ctl.ctl_setopt = cfil_ctl_setopt;
	kern_ctl.ctl_rcvd = cfil_ctl_rcvd;
	error = ctl_register(&kern_ctl, &cfil_kctlref);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_register failed: %d", error);
		return;
	}

	// Spawn thread for gargage collection
	if (kernel_thread_start(cfil_udp_gc_thread_func, NULL,
	    &cfil_udp_gc_thread) != KERN_SUCCESS) {
		panic_plain("%s: Can't create UDP GC thread", __func__);
		/* NOTREACHED */
	}
	/* this must not fail */
	VERIFY(cfil_udp_gc_thread != NULL);

	// Set UDP per-flow mbuf thresholds to 1/32 of platform max
	mbuf_limit = MAX(UDP_FLOW_GC_MBUF_CNT_MAX, (nmbclusters << MCLSHIFT) >> UDP_FLOW_GC_MBUF_SHIFT);
	cfil_udp_gc_mbuf_num_max = (mbuf_limit >> MCLSHIFT);
	cfil_udp_gc_mbuf_cnt_max = mbuf_limit;
}

struct cfil_info *
cfil_info_alloc(struct socket *so, struct cfil_hash_entry *hash_entry)
{
	int kcunit;
	struct cfil_info *cfil_info = NULL;
	struct inpcb *inp = sotoinpcb(so);

	CFIL_LOG(LOG_INFO, "");

	socket_lock_assert_owned(so);

	cfil_info = zalloc(cfil_info_zone);
	if (cfil_info == NULL) {
		goto done;
	}
	bzero(cfil_info, sizeof(struct cfil_info));

	cfil_queue_init(&cfil_info->cfi_snd.cfi_inject_q);
	cfil_queue_init(&cfil_info->cfi_rcv.cfi_inject_q);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		struct cfil_entry *entry;

		entry = &cfil_info->cfi_entries[kcunit - 1];
		entry->cfe_cfil_info = cfil_info;

		/* Initialize the filter entry */
		entry->cfe_filter = NULL;
		entry->cfe_flags = 0;
		entry->cfe_necp_control_unit = 0;
		entry->cfe_snd.cfe_pass_offset = 0;
		entry->cfe_snd.cfe_peek_offset = 0;
		entry->cfe_snd.cfe_peeked = 0;
		entry->cfe_rcv.cfe_pass_offset = 0;
		entry->cfe_rcv.cfe_peek_offset = 0;
		entry->cfe_rcv.cfe_peeked = 0;
		/*
		 * Timestamp the last action to avoid pre-maturely
		 * triggering garbage collection
		 */
		microuptime(&entry->cfe_last_action);

		cfil_queue_init(&entry->cfe_snd.cfe_pending_q);
		cfil_queue_init(&entry->cfe_rcv.cfe_pending_q);
		cfil_queue_init(&entry->cfe_snd.cfe_ctl_q);
		cfil_queue_init(&entry->cfe_rcv.cfe_ctl_q);
	}

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	/*
	 * Create a cfi_sock_id that's not the socket pointer!
	 */

	if (hash_entry == NULL) {
		// This is the TCP case, cfil_info is tracked per socket
		if (inp->inp_flowhash == 0) {
			inp->inp_flowhash = inp_calc_flowhash(inp);
		}

		so->so_cfil = cfil_info;
		cfil_info->cfi_so = so;
		cfil_info->cfi_sock_id =
		    ((so->so_gencnt << 32) | inp->inp_flowhash);
	} else {
		// This is the UDP case, cfil_info is tracked in per-socket hash
		cfil_info->cfi_so = so;
		hash_entry->cfentry_cfil = cfil_info;
		cfil_info->cfi_hash_entry = hash_entry;
		cfil_info->cfi_sock_id = ((so->so_gencnt << 32) | (hash_entry->cfentry_flowhash & 0xffffffff));
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP inp_flowhash %x so_gencnt %llx entry flowhash %x sockID %llx",
		    inp->inp_flowhash, so->so_gencnt, hash_entry->cfentry_flowhash, cfil_info->cfi_sock_id);

		// Wake up gc thread if this is first flow added
		if (cfil_sock_udp_attached_count == 0) {
			thread_wakeup((caddr_t)&cfil_sock_udp_attached_count);
		}

		cfil_sock_udp_attached_count++;
	}

	TAILQ_INSERT_TAIL(&cfil_sock_head, cfil_info, cfi_link);

	cfil_sock_attached_count++;

	cfil_rw_unlock_exclusive(&cfil_lck_rw);

done:
	if (cfil_info != NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_cfi_alloc_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_cfi_alloc_fail);
	}

	return cfil_info;
}

int
cfil_info_attach_unit(struct socket *so, uint32_t filter_control_unit, struct cfil_info *cfil_info)
{
	int kcunit;
	int attached = 0;

	CFIL_LOG(LOG_INFO, "");

	socket_lock_assert_owned(so);

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	for (kcunit = 1;
	    content_filters != NULL && kcunit <= MAX_CONTENT_FILTER;
	    kcunit++) {
		struct content_filter *cfc = content_filters[kcunit - 1];
		struct cfil_entry *entry;

		if (cfc == NULL) {
			continue;
		}
		if (cfc->cf_necp_control_unit != filter_control_unit) {
			continue;
		}

		entry = &cfil_info->cfi_entries[kcunit - 1];

		entry->cfe_filter = cfc;
		entry->cfe_necp_control_unit = filter_control_unit;
		TAILQ_INSERT_TAIL(&cfc->cf_sock_entries, entry, cfe_link);
		cfc->cf_sock_count++;
		verify_content_filter(cfc);
		attached = 1;
		entry->cfe_flags |= CFEF_CFIL_ATTACHED;
		break;
	}

	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	return attached;
}

static void
cfil_info_free(struct cfil_info *cfil_info)
{
	int kcunit;
	uint64_t in_drain = 0;
	uint64_t out_drained = 0;

	if (cfil_info == NULL) {
		return;
	}

	CFIL_LOG(LOG_INFO, "");

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	for (kcunit = 1;
	    content_filters != NULL && kcunit <= MAX_CONTENT_FILTER;
	    kcunit++) {
		struct cfil_entry *entry;
		struct content_filter *cfc;

		entry = &cfil_info->cfi_entries[kcunit - 1];

		/* Don't be silly and try to detach twice */
		if (entry->cfe_filter == NULL) {
			continue;
		}

		cfc = content_filters[kcunit - 1];

		VERIFY(cfc == entry->cfe_filter);

		entry->cfe_filter = NULL;
		entry->cfe_necp_control_unit = 0;
		TAILQ_REMOVE(&cfc->cf_sock_entries, entry, cfe_link);
		cfc->cf_sock_count--;

		verify_content_filter(cfc);
	}
	if (cfil_info->cfi_hash_entry != NULL) {
		cfil_sock_udp_attached_count--;
	}
	cfil_sock_attached_count--;
	TAILQ_REMOVE(&cfil_sock_head, cfil_info, cfi_link);

	out_drained += cfil_queue_drain(&cfil_info->cfi_snd.cfi_inject_q);
	in_drain += cfil_queue_drain(&cfil_info->cfi_rcv.cfi_inject_q);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		struct cfil_entry *entry;

		entry = &cfil_info->cfi_entries[kcunit - 1];
		out_drained += cfil_queue_drain(&entry->cfe_snd.cfe_pending_q);
		in_drain += cfil_queue_drain(&entry->cfe_rcv.cfe_pending_q);
		out_drained += cfil_queue_drain(&entry->cfe_snd.cfe_ctl_q);
		in_drain += cfil_queue_drain(&entry->cfe_rcv.cfe_ctl_q);
	}
	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	if (out_drained) {
		OSIncrementAtomic(&cfil_stats.cfs_flush_out_free);
	}
	if (in_drain) {
		OSIncrementAtomic(&cfil_stats.cfs_flush_in_free);
	}

	zfree(cfil_info_zone, cfil_info);
}

/*
 * Entry point from Sockets layer
 * The socket is locked.
 */
errno_t
cfil_sock_attach(struct socket *so)
{
	errno_t error = 0;
	uint32_t filter_control_unit;

	socket_lock_assert_owned(so);

	/* Limit ourselves to TCP that are not MPTCP subflows */
	if ((so->so_proto->pr_domain->dom_family != PF_INET &&
	    so->so_proto->pr_domain->dom_family != PF_INET6) ||
	    so->so_proto->pr_type != SOCK_STREAM ||
	    so->so_proto->pr_protocol != IPPROTO_TCP ||
	    (so->so_flags & SOF_MP_SUBFLOW) != 0 ||
	    (so->so_flags1 & SOF1_CONTENT_FILTER_SKIP) != 0) {
		goto done;
	}

	filter_control_unit = necp_socket_get_content_filter_control_unit(so);
	if (filter_control_unit == 0) {
		goto done;
	}

	if ((filter_control_unit & NECP_MASK_USERSPACE_ONLY) != 0) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_userspace_only);
		goto done;
	}
	if (cfil_active_count == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_in_vain);
		goto done;
	}
	if (so->so_cfil != NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_already);
		CFIL_LOG(LOG_ERR, "already attached");
	} else {
		cfil_info_alloc(so, NULL);
		if (so->so_cfil == NULL) {
			error = ENOMEM;
			OSIncrementAtomic(&cfil_stats.cfs_sock_attach_no_mem);
			goto done;
		}
	}
	if (cfil_info_attach_unit(so, filter_control_unit, so->so_cfil) == 0) {
		CFIL_LOG(LOG_ERR, "cfil_info_attach_unit(%u) failed",
		    filter_control_unit);
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_failed);
		goto done;
	}
	CFIL_LOG(LOG_INFO, "so %llx filter_control_unit %u sockID %llx",
	    (uint64_t)VM_KERNEL_ADDRPERM(so),
	    filter_control_unit, so->so_cfil->cfi_sock_id);

	so->so_flags |= SOF_CONTENT_FILTER;
	OSIncrementAtomic(&cfil_stats.cfs_sock_attached);

	/* Hold a reference on the socket */
	so->so_usecount++;

	error = cfil_dispatch_attach_event(so, so->so_cfil, filter_control_unit);
	/* We can recover from flow control or out of memory errors */
	if (error == ENOBUFS || error == ENOMEM) {
		error = 0;
	} else if (error != 0) {
		goto done;
	}

	CFIL_INFO_VERIFY(so->so_cfil);
done:
	return error;
}

/*
 * Entry point from Sockets layer
 * The socket is locked.
 */
errno_t
cfil_sock_detach(struct socket *so)
{
	if (IS_UDP(so)) {
		cfil_db_free(so);
		return 0;
	}

	if (so->so_cfil) {
		if (so->so_flags & SOF_CONTENT_FILTER) {
			so->so_flags &= ~SOF_CONTENT_FILTER;
			VERIFY(so->so_usecount > 0);
			so->so_usecount--;
		}
		cfil_info_free(so->so_cfil);
		so->so_cfil = NULL;
		OSIncrementAtomic(&cfil_stats.cfs_sock_detached);
	}
	return 0;
}

static int
cfil_dispatch_attach_event(struct socket *so, struct cfil_info *cfil_info, uint32_t filter_control_unit)
{
	errno_t error = 0;
	struct cfil_entry *entry = NULL;
	struct cfil_msg_sock_attached msg_attached;
	uint32_t kcunit;
	struct content_filter *cfc = NULL;

	socket_lock_assert_owned(so);

	cfil_rw_lock_shared(&cfil_lck_rw);

	if (so->so_proto == NULL || so->so_proto->pr_domain == NULL) {
		error = EINVAL;
		goto done;
	}
	/*
	 * Find the matching filter unit
	 */
	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		cfc = content_filters[kcunit - 1];

		if (cfc == NULL) {
			continue;
		}
		if (cfc->cf_necp_control_unit != filter_control_unit) {
			continue;
		}
		entry = &cfil_info->cfi_entries[kcunit - 1];
		if (entry->cfe_filter == NULL) {
			continue;
		}

		VERIFY(cfc == entry->cfe_filter);

		break;
	}

	if (entry == NULL || entry->cfe_filter == NULL) {
		goto done;
	}

	if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED)) {
		goto done;
	}

	CFIL_LOG(LOG_INFO, "so %llx filter_control_unit %u kcunit %u",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), filter_control_unit, kcunit);

	/* Would be wasteful to try when flow controlled */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}

	bzero(&msg_attached, sizeof(struct cfil_msg_sock_attached));
	msg_attached.cfs_msghdr.cfm_len = sizeof(struct cfil_msg_sock_attached);
	msg_attached.cfs_msghdr.cfm_version = CFM_VERSION_CURRENT;
	msg_attached.cfs_msghdr.cfm_type = CFM_TYPE_EVENT;
	msg_attached.cfs_msghdr.cfm_op = CFM_OP_SOCKET_ATTACHED;
	msg_attached.cfs_msghdr.cfm_sock_id = entry->cfe_cfil_info->cfi_sock_id;

	msg_attached.cfs_sock_family = so->so_proto->pr_domain->dom_family;
	msg_attached.cfs_sock_type = so->so_proto->pr_type;
	msg_attached.cfs_sock_protocol = so->so_proto->pr_protocol;
	msg_attached.cfs_pid = so->last_pid;
	memcpy(msg_attached.cfs_uuid, so->last_uuid, sizeof(uuid_t));
	if (so->so_flags & SOF_DELEGATED) {
		msg_attached.cfs_e_pid = so->e_pid;
		memcpy(msg_attached.cfs_e_uuid, so->e_uuid, sizeof(uuid_t));
	} else {
		msg_attached.cfs_e_pid = so->last_pid;
		memcpy(msg_attached.cfs_e_uuid, so->last_uuid, sizeof(uuid_t));
	}

#if LIFECYCLE_DEBUG
	CFIL_LOG(LOG_DEBUG, "CFIL: LIFECYCLE: SENDING ATTACH UP <sockID %llu> ",
	    entry->cfe_cfil_info->cfi_sock_id);
#endif

	error = ctl_enqueuedata(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    &msg_attached,
	    sizeof(struct cfil_msg_sock_attached),
	    CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuedata() failed: %d", error);
		goto done;
	}
	microuptime(&entry->cfe_last_event);
	cfil_info->cfi_first_event.tv_sec = entry->cfe_last_event.tv_sec;
	cfil_info->cfi_first_event.tv_usec = entry->cfe_last_event.tv_usec;

	entry->cfe_flags |= CFEF_SENT_SOCK_ATTACHED;
	OSIncrementAtomic(&cfil_stats.cfs_attach_event_ok);
done:

	/* We can recover from flow control */
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(&cfil_stats.cfs_attach_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_attach_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}
	return error;
}

static int
cfil_dispatch_disconnect_event(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing)
{
	errno_t error = 0;
	struct mbuf *msg = NULL;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	struct cfil_msg_hdr msg_disconnected;
	struct content_filter *cfc;

	socket_lock_assert_owned(so);

	cfil_rw_lock_shared(&cfil_lck_rw);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	cfc = entry->cfe_filter;
	if (cfc == NULL) {
		goto done;
	}

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	/*
	 * Send the disconnection event once
	 */
	if ((outgoing && (entry->cfe_flags & CFEF_SENT_DISCONNECT_OUT)) ||
	    (!outgoing && (entry->cfe_flags & CFEF_SENT_DISCONNECT_IN))) {
		CFIL_LOG(LOG_INFO, "so %llx disconnect already sent",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		goto done;
	}

	/*
	 * We're not disconnected as long as some data is waiting
	 * to be delivered to the filter
	 */
	if (outgoing && cfil_queue_empty(&entrybuf->cfe_ctl_q) == 0) {
		CFIL_LOG(LOG_INFO, "so %llx control queue not empty",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EBUSY;
		goto done;
	}
	/* Would be wasteful to try when flow controlled */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}

#if LIFECYCLE_DEBUG
	cfil_info_log(LOG_ERR, cfil_info, outgoing ?
	    "CFIL: LIFECYCLE: OUT - SENDING DISCONNECT UP":
	    "CFIL: LIFECYCLE: IN - SENDING DISCONNECT UP");
#endif

	bzero(&msg_disconnected, sizeof(struct cfil_msg_hdr));
	msg_disconnected.cfm_len = sizeof(struct cfil_msg_hdr);
	msg_disconnected.cfm_version = CFM_VERSION_CURRENT;
	msg_disconnected.cfm_type = CFM_TYPE_EVENT;
	msg_disconnected.cfm_op = outgoing ? CFM_OP_DISCONNECT_OUT :
	    CFM_OP_DISCONNECT_IN;
	msg_disconnected.cfm_sock_id = entry->cfe_cfil_info->cfi_sock_id;
	error = ctl_enqueuedata(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    &msg_disconnected,
	    sizeof(struct cfil_msg_hdr),
	    CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuembuf() failed: %d", error);
		mbuf_freem(msg);
		goto done;
	}
	microuptime(&entry->cfe_last_event);
	CFI_ADD_TIME_LOG(cfil_info, &entry->cfe_last_event, &cfil_info->cfi_first_event, msg_disconnected.cfm_op);

	/* Remember we have sent the disconnection message */
	if (outgoing) {
		entry->cfe_flags |= CFEF_SENT_DISCONNECT_OUT;
		OSIncrementAtomic(&cfil_stats.cfs_disconnect_out_event_ok);
	} else {
		entry->cfe_flags |= CFEF_SENT_DISCONNECT_IN;
		OSIncrementAtomic(&cfil_stats.cfs_disconnect_in_event_ok);
	}
done:
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(
			&cfil_stats.cfs_disconnect_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(
				&cfil_stats.cfs_disconnect_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}
	return error;
}

int
cfil_dispatch_closed_event(struct socket *so, struct cfil_info *cfil_info, int kcunit)
{
	struct cfil_entry *entry;
	struct cfil_msg_sock_closed msg_closed;
	errno_t error = 0;
	struct content_filter *cfc;

	socket_lock_assert_owned(so);

	cfil_rw_lock_shared(&cfil_lck_rw);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	cfc = entry->cfe_filter;
	if (cfc == NULL) {
		goto done;
	}

	CFIL_LOG(LOG_INFO, "so %llx kcunit %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);

	/* Would be wasteful to try when flow controlled */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}
	/*
	 * Send a single closed message per filter
	 */
	if ((entry->cfe_flags & CFEF_SENT_SOCK_CLOSED) != 0) {
		goto done;
	}
	if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
		goto done;
	}

	microuptime(&entry->cfe_last_event);
	CFI_ADD_TIME_LOG(cfil_info, &entry->cfe_last_event, &cfil_info->cfi_first_event, CFM_OP_SOCKET_CLOSED);

	bzero(&msg_closed, sizeof(struct cfil_msg_sock_closed));
	msg_closed.cfc_msghdr.cfm_len = sizeof(struct cfil_msg_sock_closed);
	msg_closed.cfc_msghdr.cfm_version = CFM_VERSION_CURRENT;
	msg_closed.cfc_msghdr.cfm_type = CFM_TYPE_EVENT;
	msg_closed.cfc_msghdr.cfm_op = CFM_OP_SOCKET_CLOSED;
	msg_closed.cfc_msghdr.cfm_sock_id = entry->cfe_cfil_info->cfi_sock_id;
	msg_closed.cfc_first_event.tv_sec = cfil_info->cfi_first_event.tv_sec;
	msg_closed.cfc_first_event.tv_usec = cfil_info->cfi_first_event.tv_usec;
	memcpy(msg_closed.cfc_op_time, cfil_info->cfi_op_time, sizeof(uint32_t) * CFI_MAX_TIME_LOG_ENTRY);
	memcpy(msg_closed.cfc_op_list, cfil_info->cfi_op_list, sizeof(unsigned char) * CFI_MAX_TIME_LOG_ENTRY);
	msg_closed.cfc_op_list_ctr = cfil_info->cfi_op_list_ctr;

#if LIFECYCLE_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: LIFECYCLE: SENDING CLOSED UP: <sock id %llu> op ctr %d, start time %llu.%llu", msg_closed.cfc_msghdr.cfm_sock_id, cfil_info->cfi_op_list_ctr, cfil_info->cfi_first_event.tv_sec, cfil_info->cfi_first_event.tv_usec);
#endif
	/* for debugging
	 *  if (msg_closed.cfc_op_list_ctr > CFI_MAX_TIME_LOG_ENTRY) {
	 *       msg_closed.cfc_op_list_ctr  = CFI_MAX_TIME_LOG_ENTRY;       // just in case
	 *  }
	 *  for (unsigned int i = 0; i < msg_closed.cfc_op_list_ctr ; i++) {
	 *       CFIL_LOG(LOG_ERR, "MD: socket %llu event %2u, time + %u msec", msg_closed.cfc_msghdr.cfm_sock_id, (unsigned short)msg_closed.cfc_op_list[i], msg_closed.cfc_op_time[i]);
	 *  }
	 */

	error = ctl_enqueuedata(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    &msg_closed,
	    sizeof(struct cfil_msg_sock_closed),
	    CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuedata() failed: %d",
		    error);
		goto done;
	}

	entry->cfe_flags |= CFEF_SENT_SOCK_CLOSED;
	OSIncrementAtomic(&cfil_stats.cfs_closed_event_ok);
done:
	/* We can recover from flow control */
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(&cfil_stats.cfs_closed_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_closed_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}

	return error;
}

static void
fill_ip6_sockaddr_4_6(union sockaddr_in_4_6 *sin46,
    struct in6_addr *ip6, u_int16_t port)
{
	struct sockaddr_in6 *sin6 = &sin46->sin6;

	sin6->sin6_family = AF_INET6;
	sin6->sin6_len = sizeof(*sin6);
	sin6->sin6_port = port;
	sin6->sin6_addr = *ip6;
	if (IN6_IS_SCOPE_EMBED(&sin6->sin6_addr)) {
		sin6->sin6_scope_id = ntohs(sin6->sin6_addr.s6_addr16[1]);
		sin6->sin6_addr.s6_addr16[1] = 0;
	}
}

static void
fill_ip_sockaddr_4_6(union sockaddr_in_4_6 *sin46,
    struct in_addr ip, u_int16_t port)
{
	struct sockaddr_in *sin = &sin46->sin;

	sin->sin_family = AF_INET;
	sin->sin_len = sizeof(*sin);
	sin->sin_port = port;
	sin->sin_addr.s_addr = ip.s_addr;
}

static void
cfil_get_flow_address_v6(struct cfil_hash_entry *entry, struct inpcb *inp,
    struct in6_addr **laddr, struct in6_addr **faddr,
    u_int16_t *lport, u_int16_t *fport)
{
	if (entry != NULL) {
		*laddr = &entry->cfentry_laddr.addr6;
		*faddr = &entry->cfentry_faddr.addr6;
		*lport = entry->cfentry_lport;
		*fport = entry->cfentry_fport;
	} else {
		*laddr = &inp->in6p_laddr;
		*faddr = &inp->in6p_faddr;
		*lport = inp->inp_lport;
		*fport = inp->inp_fport;
	}
}

static void
cfil_get_flow_address(struct cfil_hash_entry *entry, struct inpcb *inp,
    struct in_addr *laddr, struct in_addr *faddr,
    u_int16_t *lport, u_int16_t *fport)
{
	if (entry != NULL) {
		*laddr = entry->cfentry_laddr.addr46.ia46_addr4;
		*faddr = entry->cfentry_faddr.addr46.ia46_addr4;
		*lport = entry->cfentry_lport;
		*fport = entry->cfentry_fport;
	} else {
		*laddr = inp->inp_laddr;
		*faddr = inp->inp_faddr;
		*lport = inp->inp_lport;
		*fport = inp->inp_fport;
	}
}

static int
cfil_dispatch_data_event(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    struct mbuf *data, unsigned int copyoffset, unsigned int copylen)
{
	errno_t error = 0;
	struct mbuf *copy = NULL;
	struct mbuf *msg = NULL;
	unsigned int one = 1;
	struct cfil_msg_data_event *data_req;
	size_t hdrsize;
	struct inpcb *inp = (struct inpcb *)so->so_pcb;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	struct content_filter *cfc;
	struct timeval tv;

	cfil_rw_lock_shared(&cfil_lck_rw);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	cfc = entry->cfe_filter;
	if (cfc == NULL) {
		goto done;
	}

	data = cfil_data_start(data);
	if (data == NULL || (data->m_flags & M_PKTHDR) == 0) {
		CFIL_LOG(LOG_ERR, "NOT PKTHDR");
		goto done;
	}

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	/* Would be wasteful to try */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}

	/* Make a copy of the data to pass to kernel control socket */
	copy = m_copym_mode(data, copyoffset, copylen, M_DONTWAIT,
	    M_COPYM_NOOP_HDR);
	if (copy == NULL) {
		CFIL_LOG(LOG_ERR, "m_copym_mode() failed");
		error = ENOMEM;
		goto done;
	}

	/* We need an mbuf packet for the message header */
	hdrsize = sizeof(struct cfil_msg_data_event);
	error = mbuf_allocpacket(MBUF_DONTWAIT, hdrsize, &one, &msg);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "mbuf_allocpacket() failed");
		m_freem(copy);
		/*
		 * ENOBUFS is to indicate flow control
		 */
		error = ENOMEM;
		goto done;
	}
	mbuf_setlen(msg, hdrsize);
	mbuf_pkthdr_setlen(msg, hdrsize + copylen);
	msg->m_next = copy;
	data_req = (struct cfil_msg_data_event *)mbuf_data(msg);
	bzero(data_req, hdrsize);
	data_req->cfd_msghdr.cfm_len = hdrsize + copylen;
	data_req->cfd_msghdr.cfm_version = 1;
	data_req->cfd_msghdr.cfm_type = CFM_TYPE_EVENT;
	data_req->cfd_msghdr.cfm_op =
	    outgoing ? CFM_OP_DATA_OUT : CFM_OP_DATA_IN;
	data_req->cfd_msghdr.cfm_sock_id =
	    entry->cfe_cfil_info->cfi_sock_id;
	data_req->cfd_start_offset = entrybuf->cfe_peeked;
	data_req->cfd_end_offset = entrybuf->cfe_peeked + copylen;

	/*
	 * TBD:
	 * For non connected sockets need to copy addresses from passed
	 * parameters
	 */
	if (inp->inp_vflag & INP_IPV6) {
		struct in6_addr *laddr = NULL, *faddr = NULL;
		u_int16_t lport = 0, fport = 0;

		cfil_get_flow_address_v6(cfil_info->cfi_hash_entry, inp,
		    &laddr, &faddr, &lport, &fport);
		if (outgoing) {
			fill_ip6_sockaddr_4_6(&data_req->cfc_src, laddr, lport);
			fill_ip6_sockaddr_4_6(&data_req->cfc_dst, faddr, fport);
		} else {
			fill_ip6_sockaddr_4_6(&data_req->cfc_src, faddr, fport);
			fill_ip6_sockaddr_4_6(&data_req->cfc_dst, laddr, lport);
		}
	} else if (inp->inp_vflag & INP_IPV4) {
		struct in_addr laddr = {0}, faddr = {0};
		u_int16_t lport = 0, fport = 0;

		cfil_get_flow_address(cfil_info->cfi_hash_entry, inp,
		    &laddr, &faddr, &lport, &fport);

		if (outgoing) {
			fill_ip_sockaddr_4_6(&data_req->cfc_src, laddr, lport);
			fill_ip_sockaddr_4_6(&data_req->cfc_dst, faddr, fport);
		} else {
			fill_ip_sockaddr_4_6(&data_req->cfc_src, faddr, fport);
			fill_ip_sockaddr_4_6(&data_req->cfc_dst, laddr, lport);
		}
	}

	microuptime(&tv);
	CFI_ADD_TIME_LOG(cfil_info, &tv, &cfil_info->cfi_first_event, data_req->cfd_msghdr.cfm_op);

	/* Pass the message to the content filter */
	error = ctl_enqueuembuf(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    msg, CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuembuf() failed: %d", error);
		mbuf_freem(msg);
		goto done;
	}
	entry->cfe_flags &= ~CFEF_FLOW_CONTROLLED;
	OSIncrementAtomic(&cfil_stats.cfs_data_event_ok);

#if VERDICT_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: VERDICT ACTION: so %llx sockID %llu outgoing %d: mbuf %llx copyoffset %u copylen %u",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), cfil_info->cfi_sock_id, outgoing, (uint64_t)VM_KERNEL_ADDRPERM(data), copyoffset, copylen);
#endif

done:
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(
			&cfil_stats.cfs_data_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_data_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}
	return error;
}

/*
 * Process the queue of data waiting to be delivered to content filter
 */
static int
cfil_data_service_ctl_q(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing)
{
	errno_t error = 0;
	struct mbuf *data, *tmp = NULL;
	unsigned int datalen = 0, copylen = 0, copyoffset = 0;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	uint64_t currentoffset = 0;

	if (cfil_info == NULL) {
		return 0;
	}

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	/* Send attached message if not yet done */
	if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
		error = cfil_dispatch_attach_event(so, cfil_info, kcunit);
		if (error != 0) {
			/* We can recover from flow control */
			if (error == ENOBUFS || error == ENOMEM) {
				error = 0;
			}
			goto done;
		}
	} else if ((entry->cfe_flags & CFEF_DATA_START) == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_q_not_started);
		goto done;
	}

#if DATA_DEBUG
	CFIL_LOG(LOG_DEBUG, "CFIL: SERVICE CTL-Q: pass_offset %llu peeked %llu peek_offset %llu",
	    entrybuf->cfe_pass_offset,
	    entrybuf->cfe_peeked,
	    entrybuf->cfe_peek_offset);
#endif

	/* Move all data that can pass */
	while ((data = cfil_queue_first(&entrybuf->cfe_ctl_q)) != NULL &&
	    entrybuf->cfe_ctl_q.q_start < entrybuf->cfe_pass_offset) {
		datalen = cfil_data_length(data, NULL, NULL);
		tmp = data;

		if (entrybuf->cfe_ctl_q.q_start + datalen <=
		    entrybuf->cfe_pass_offset) {
			/*
			 * The first mbuf can fully pass
			 */
			copylen = datalen;
		} else {
			/*
			 * The first mbuf can partially pass
			 */
			copylen = entrybuf->cfe_pass_offset -
			    entrybuf->cfe_ctl_q.q_start;
		}
		VERIFY(copylen <= datalen);

#if DATA_DEBUG
		CFIL_LOG(LOG_DEBUG,
		    "CFIL: SERVICE CTL-Q PASSING: %llx first %llu peeked %llu pass %llu peek %llu"
		    "datalen %u copylen %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
		    entrybuf->cfe_ctl_q.q_start,
		    entrybuf->cfe_peeked,
		    entrybuf->cfe_pass_offset,
		    entrybuf->cfe_peek_offset,
		    datalen, copylen);
#endif

		/*
		 * Data that passes has been peeked at explicitly or
		 * implicitly
		 */
		if (entrybuf->cfe_ctl_q.q_start + copylen >
		    entrybuf->cfe_peeked) {
			entrybuf->cfe_peeked =
			    entrybuf->cfe_ctl_q.q_start + copylen;
		}
		/*
		 * Stop on partial pass
		 */
		if (copylen < datalen) {
			break;
		}

		/* All good, move full data from ctl queue to pending queue */
		cfil_queue_remove(&entrybuf->cfe_ctl_q, data, datalen);

		cfil_queue_enqueue(&entrybuf->cfe_pending_q, data, datalen);
		if (outgoing) {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_pending_q_out_enqueued);
		} else {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_pending_q_in_enqueued);
		}
	}
	CFIL_INFO_VERIFY(cfil_info);
	if (tmp != NULL) {
		CFIL_LOG(LOG_DEBUG,
		    "%llx first %llu peeked %llu pass %llu peek %llu"
		    "datalen %u copylen %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
		    entrybuf->cfe_ctl_q.q_start,
		    entrybuf->cfe_peeked,
		    entrybuf->cfe_pass_offset,
		    entrybuf->cfe_peek_offset,
		    datalen, copylen);
	}
	tmp = NULL;

	/* Now deal with remaining data the filter wants to peek at */
	for (data = cfil_queue_first(&entrybuf->cfe_ctl_q),
	    currentoffset = entrybuf->cfe_ctl_q.q_start;
	    data != NULL && currentoffset < entrybuf->cfe_peek_offset;
	    data = cfil_queue_next(&entrybuf->cfe_ctl_q, data),
	    currentoffset += datalen) {
		datalen = cfil_data_length(data, NULL, NULL);
		tmp = data;

		/* We've already peeked at this mbuf */
		if (currentoffset + datalen <= entrybuf->cfe_peeked) {
			continue;
		}
		/*
		 * The data in the first mbuf may have been
		 * partially peeked at
		 */
		copyoffset = entrybuf->cfe_peeked - currentoffset;
		VERIFY(copyoffset < datalen);
		copylen = datalen - copyoffset;
		VERIFY(copylen <= datalen);
		/*
		 * Do not copy more than needed
		 */
		if (currentoffset + copyoffset + copylen >
		    entrybuf->cfe_peek_offset) {
			copylen = entrybuf->cfe_peek_offset -
			    (currentoffset + copyoffset);
		}

#if DATA_DEBUG
		CFIL_LOG(LOG_DEBUG,
		    "CFIL: SERVICE CTL-Q PEEKING: %llx current %llu peeked %llu pass %llu peek %llu "
		    "datalen %u copylen %u copyoffset %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
		    currentoffset,
		    entrybuf->cfe_peeked,
		    entrybuf->cfe_pass_offset,
		    entrybuf->cfe_peek_offset,
		    datalen, copylen, copyoffset);
#endif

		/*
		 * Stop if there is nothing more to peek at
		 */
		if (copylen == 0) {
			break;
		}
		/*
		 * Let the filter get a peek at this span of data
		 */
		error = cfil_dispatch_data_event(so, cfil_info, kcunit,
		    outgoing, data, copyoffset, copylen);
		if (error != 0) {
			/* On error, leave data in ctl_q */
			break;
		}
		entrybuf->cfe_peeked += copylen;
		if (outgoing) {
			OSAddAtomic64(copylen,
			    &cfil_stats.cfs_ctl_q_out_peeked);
		} else {
			OSAddAtomic64(copylen,
			    &cfil_stats.cfs_ctl_q_in_peeked);
		}

		/* Stop when data could not be fully peeked at */
		if (copylen + copyoffset < datalen) {
			break;
		}
	}
	CFIL_INFO_VERIFY(cfil_info);
	if (tmp != NULL) {
		CFIL_LOG(LOG_DEBUG,
		    "%llx first %llu peeked %llu pass %llu peek %llu"
		    "datalen %u copylen %u copyoffset %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
		    currentoffset,
		    entrybuf->cfe_peeked,
		    entrybuf->cfe_pass_offset,
		    entrybuf->cfe_peek_offset,
		    datalen, copylen, copyoffset);
	}

	/*
	 * Process data that has passed the filter
	 */
	error = cfil_service_pending_queue(so, cfil_info, kcunit, outgoing);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "cfil_service_pending_queue() error %d",
		    error);
		goto done;
	}

	/*
	 * Dispatch disconnect events that could not be sent
	 */
	if (cfil_info == NULL) {
		goto done;
	} else if (outgoing) {
		if ((cfil_info->cfi_flags & CFIF_SHUT_WR) &&
		    !(entry->cfe_flags & CFEF_SENT_DISCONNECT_OUT)) {
			cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 1);
		}
	} else {
		if ((cfil_info->cfi_flags & CFIF_SHUT_RD) &&
		    !(entry->cfe_flags & CFEF_SENT_DISCONNECT_IN)) {
			cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 0);
		}
	}

done:
	CFIL_LOG(LOG_DEBUG,
	    "first %llu peeked %llu pass %llu peek %llu",
	    entrybuf->cfe_ctl_q.q_start,
	    entrybuf->cfe_peeked,
	    entrybuf->cfe_pass_offset,
	    entrybuf->cfe_peek_offset);

	CFIL_INFO_VERIFY(cfil_info);
	return error;
}

/*
 * cfil_data_filter()
 *
 * Process data for a content filter installed on a socket
 */
int
cfil_data_filter(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    struct mbuf *data, uint64_t datalen)
{
	errno_t error = 0;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	/* Are we attached to the filter? */
	if (entry->cfe_filter == NULL) {
		error = 0;
		goto done;
	}

	/* Dispatch to filters */
	cfil_queue_enqueue(&entrybuf->cfe_ctl_q, data, datalen);
	if (outgoing) {
		OSAddAtomic64(datalen,
		    &cfil_stats.cfs_ctl_q_out_enqueued);
	} else {
		OSAddAtomic64(datalen,
		    &cfil_stats.cfs_ctl_q_in_enqueued);
	}

	error = cfil_data_service_ctl_q(so, cfil_info, kcunit, outgoing);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "cfil_data_service_ctl_q() error %d",
		    error);
	}
	/*
	 * We have to return EJUSTRETURN in all cases to avoid double free
	 * by socket layer
	 */
	error = EJUSTRETURN;
done:
	CFIL_INFO_VERIFY(cfil_info);

	CFIL_LOG(LOG_INFO, "return %d", error);
	return error;
}

/*
 * cfil_service_inject_queue() re-inject data that passed the
 * content filters
 */
static int
cfil_service_inject_queue(struct socket *so, struct cfil_info *cfil_info, int outgoing)
{
	mbuf_t data;
	unsigned int datalen;
	int mbcnt = 0;
	int mbnum = 0;
	errno_t error = 0;
	struct cfi_buf *cfi_buf;
	struct cfil_queue *inject_q;
	int need_rwakeup = 0;
	int count = 0;

	if (cfil_info == NULL) {
		return 0;
	}

	socket_lock_assert_owned(so);

	if (outgoing) {
		cfi_buf = &cfil_info->cfi_snd;
		cfil_info->cfi_flags &= ~CFIF_RETRY_INJECT_OUT;
	} else {
		cfi_buf = &cfil_info->cfi_rcv;
		cfil_info->cfi_flags &= ~CFIF_RETRY_INJECT_IN;
	}
	inject_q = &cfi_buf->cfi_inject_q;

	if (cfil_queue_empty(inject_q)) {
		return 0;
	}

#if DATA_DEBUG | VERDICT_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: SERVICE INJECT-Q: <so %llx> outgoing %d queue len %llu",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing, cfil_queue_len(inject_q));
#endif

	while ((data = cfil_queue_first(inject_q)) != NULL) {
		datalen = cfil_data_length(data, &mbcnt, &mbnum);

#if DATA_DEBUG
		CFIL_LOG(LOG_DEBUG, "CFIL: SERVICE INJECT-Q: <%s>: <so %llx> data %llx datalen %u (mbcnt %u)",
		    remote_addr_ptr ? "UNCONNECTED" : "CONNECTED",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), (uint64_t)VM_KERNEL_ADDRPERM(data), datalen, mbcnt);
#endif

		/* Remove data from queue and adjust stats */
		cfil_queue_remove(inject_q, data, datalen);
		cfi_buf->cfi_pending_first += datalen;
		cfi_buf->cfi_pending_mbcnt -= mbcnt;
		cfi_buf->cfi_pending_mbnum -= mbnum;
		cfil_info_buf_verify(cfi_buf);

		if (outgoing) {
			error = sosend_reinject(so, NULL, data, NULL, 0);
			if (error != 0) {
#if DATA_DEBUG
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: Error: sosend_reinject() failed");
				CFIL_LOG(LOG_ERR, "### sosend() failed %d", error);
#endif
				break;
			}
			// At least one injection succeeded, need to wake up pending threads.
			need_rwakeup = 1;
		} else {
			data->m_flags |= M_SKIPCFIL;

			/*
			 * NOTE: We currently only support TCP and UDP.
			 * For RAWIP, MPTCP and message TCP we'll
			 * need to call the appropriate sbappendxxx()
			 * of fix sock_inject_data_in()
			 */
			if (IS_UDP(so) == TRUE) {
				if (sbappendchain(&so->so_rcv, data, 0)) {
					need_rwakeup = 1;
				}
			} else {
				if (sbappendstream(&so->so_rcv, data)) {
					need_rwakeup = 1;
				}
			}
		}

		if (outgoing) {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_inject_q_out_passed);
		} else {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_inject_q_in_passed);
		}

		count++;
	}

#if DATA_DEBUG | VERDICT_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: SERVICE INJECT-Q: <so %llx> injected %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), count);
#endif

	/* A single wakeup is for several packets is more efficient */
	if (need_rwakeup) {
		if (outgoing == TRUE) {
			sowwakeup(so);
		} else {
			sorwakeup(so);
		}
	}

	if (error != 0 && cfil_info) {
		if (error == ENOBUFS) {
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_nobufs);
		}
		if (error == ENOMEM) {
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_nomem);
		}

		if (outgoing) {
			cfil_info->cfi_flags |= CFIF_RETRY_INJECT_OUT;
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_out_fail);
		} else {
			cfil_info->cfi_flags |= CFIF_RETRY_INJECT_IN;
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_in_fail);
		}
	}

	/*
	 * Notify
	 */
	if (cfil_info && (cfil_info->cfi_flags & CFIF_SHUT_WR)) {
		cfil_sock_notify_shutdown(so, SHUT_WR);
		if (cfil_sock_data_pending(&so->so_snd) == 0) {
			soshutdownlock_final(so, SHUT_WR);
		}
	}
	if (cfil_info && (cfil_info->cfi_flags & CFIF_CLOSE_WAIT)) {
		if (cfil_filters_attached(so) == 0) {
			CFIL_LOG(LOG_INFO, "so %llx waking",
			    (uint64_t)VM_KERNEL_ADDRPERM(so));
			wakeup((caddr_t)cfil_info);
		}
	}

	CFIL_INFO_VERIFY(cfil_info);

	return error;
}

static int
cfil_service_pending_queue(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing)
{
	uint64_t passlen, curlen;
	mbuf_t data;
	unsigned int datalen;
	errno_t error = 0;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	struct cfil_queue *pending_q;

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	pending_q = &entrybuf->cfe_pending_q;

	passlen = entrybuf->cfe_pass_offset - pending_q->q_start;

	/*
	 * Locate the chunks of data that we can pass to the next filter
	 * A data chunk must be on mbuf boundaries
	 */
	curlen = 0;
	while ((data = cfil_queue_first(pending_q)) != NULL) {
		datalen = cfil_data_length(data, NULL, NULL);

#if DATA_DEBUG
		CFIL_LOG(LOG_DEBUG,
		    "CFIL: SERVICE PENDING-Q: data %llx datalen %u passlen %llu curlen %llu",
		    (uint64_t)VM_KERNEL_ADDRPERM(data), datalen,
		    passlen, curlen);
#endif

		if (curlen + datalen > passlen) {
			break;
		}

		cfil_queue_remove(pending_q, data, datalen);

		curlen += datalen;

		for (kcunit += 1;
		    kcunit <= MAX_CONTENT_FILTER;
		    kcunit++) {
			error = cfil_data_filter(so, cfil_info, kcunit, outgoing,
			    data, datalen);
			/* 0 means passed so we can continue */
			if (error != 0) {
				break;
			}
		}
		/* When data has passed all filters, re-inject */
		if (error == 0) {
			if (outgoing) {
				cfil_queue_enqueue(
					&cfil_info->cfi_snd.cfi_inject_q,
					data, datalen);
				OSAddAtomic64(datalen,
				    &cfil_stats.cfs_inject_q_out_enqueued);
			} else {
				cfil_queue_enqueue(
					&cfil_info->cfi_rcv.cfi_inject_q,
					data, datalen);
				OSAddAtomic64(datalen,
				    &cfil_stats.cfs_inject_q_in_enqueued);
			}
		}
	}

	CFIL_INFO_VERIFY(cfil_info);

	return error;
}

int
cfil_update_data_offsets(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    uint64_t pass_offset, uint64_t peek_offset)
{
	errno_t error = 0;
	struct cfil_entry *entry = NULL;
	struct cfe_buf *entrybuf;
	int updated = 0;

	CFIL_LOG(LOG_INFO, "pass %llu peek %llu", pass_offset, peek_offset);

	socket_lock_assert_owned(so);

	if (cfil_info == NULL) {
		CFIL_LOG(LOG_ERR, "so %llx cfil detached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = 0;
		goto done;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EPIPE;
		goto done;
	}

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	/* Record updated offsets for this content filter */
	if (pass_offset > entrybuf->cfe_pass_offset) {
		entrybuf->cfe_pass_offset = pass_offset;

		if (entrybuf->cfe_peek_offset < entrybuf->cfe_pass_offset) {
			entrybuf->cfe_peek_offset = entrybuf->cfe_pass_offset;
		}
		updated = 1;
	} else {
		CFIL_LOG(LOG_INFO, "pass_offset %llu <= cfe_pass_offset %llu",
		    pass_offset, entrybuf->cfe_pass_offset);
	}
	/* Filter does not want or need to see data that's allowed to pass */
	if (peek_offset > entrybuf->cfe_pass_offset &&
	    peek_offset > entrybuf->cfe_peek_offset) {
		entrybuf->cfe_peek_offset = peek_offset;
		updated = 1;
	}
	/* Nothing to do */
	if (updated == 0) {
		goto done;
	}

	/* Move data held in control queue to pending queue if needed */
	error = cfil_data_service_ctl_q(so, cfil_info, kcunit, outgoing);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "cfil_data_service_ctl_q() error %d",
		    error);
		goto done;
	}
	error = EJUSTRETURN;

done:
	/*
	 * The filter is effectively detached when pass all from both sides
	 * or when the socket is closed and no more data is waiting
	 * to be delivered to the filter
	 */
	if (entry != NULL &&
	    ((entry->cfe_snd.cfe_pass_offset == CFM_MAX_OFFSET &&
	    entry->cfe_rcv.cfe_pass_offset == CFM_MAX_OFFSET) ||
	    ((cfil_info->cfi_flags & CFIF_CLOSE_WAIT) &&
	    cfil_queue_empty(&entry->cfe_snd.cfe_ctl_q) &&
	    cfil_queue_empty(&entry->cfe_rcv.cfe_ctl_q)))) {
		entry->cfe_flags |= CFEF_CFIL_DETACHED;
#if LIFECYCLE_DEBUG
		cfil_info_log(LOG_ERR, cfil_info, outgoing ?
		    "CFIL: LIFECYCLE: OUT - PASSED ALL - DETACH":
		    "CFIL: LIFECYCLE: IN - PASSED ALL - DETACH");
#endif
		CFIL_LOG(LOG_INFO, "so %llx detached %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);
		if ((cfil_info->cfi_flags & CFIF_CLOSE_WAIT) &&
		    cfil_filters_attached(so) == 0) {
#if LIFECYCLE_DEBUG
			cfil_info_log(LOG_ERR, cfil_info, "CFIL: LIFECYCLE: WAKING");
#endif
			CFIL_LOG(LOG_INFO, "so %llx waking",
			    (uint64_t)VM_KERNEL_ADDRPERM(so));
			wakeup((caddr_t)cfil_info);
		}
	}
	CFIL_INFO_VERIFY(cfil_info);
	CFIL_LOG(LOG_INFO, "return %d", error);
	return error;
}

/*
 * Update pass offset for socket when no data is pending
 */
static int
cfil_set_socket_pass_offset(struct socket *so, struct cfil_info *cfil_info, int outgoing)
{
	struct cfi_buf *cfi_buf;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	uint32_t kcunit;
	uint64_t pass_offset = 0;

	if (cfil_info == NULL) {
		return 0;
	}

	CFIL_LOG(LOG_INFO, "so %llx outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing);

	socket_lock_assert_owned(so);

	if (outgoing) {
		cfi_buf = &cfil_info->cfi_snd;
	} else {
		cfi_buf = &cfil_info->cfi_rcv;
	}

	CFIL_LOG(LOG_DEBUG, "CFIL: <so %llx, sockID %llu> outgoing %d cfi_pending_first %llu cfi_pending_last %llu",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), cfil_info->cfi_sock_id, outgoing,
	    cfi_buf->cfi_pending_first, cfi_buf->cfi_pending_last);

	if (cfi_buf->cfi_pending_last - cfi_buf->cfi_pending_first == 0) {
		for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			/* Are we attached to a filter? */
			if (entry->cfe_filter == NULL) {
				continue;
			}

			if (outgoing) {
				entrybuf = &entry->cfe_snd;
			} else {
				entrybuf = &entry->cfe_rcv;
			}

			if (pass_offset == 0 ||
			    entrybuf->cfe_pass_offset < pass_offset) {
				pass_offset = entrybuf->cfe_pass_offset;
			}
		}
		cfi_buf->cfi_pass_offset = pass_offset;
	}

	CFIL_LOG(LOG_DEBUG, "CFIL: <so %llx, sockID %llu>, cfi_pass_offset %llu",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), cfil_info->cfi_sock_id, cfi_buf->cfi_pass_offset);

	return 0;
}

int
cfil_action_data_pass(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    uint64_t pass_offset, uint64_t peek_offset)
{
	errno_t error = 0;

	CFIL_LOG(LOG_INFO, "");

	socket_lock_assert_owned(so);

	error = cfil_acquire_sockbuf(so, cfil_info, outgoing);
	if (error != 0) {
		CFIL_LOG(LOG_INFO, "so %llx %s dropped",
		    (uint64_t)VM_KERNEL_ADDRPERM(so),
		    outgoing ? "out" : "in");
		goto release;
	}

	error = cfil_update_data_offsets(so, cfil_info, kcunit, outgoing,
	    pass_offset, peek_offset);

	cfil_service_inject_queue(so, cfil_info, outgoing);

	cfil_set_socket_pass_offset(so, cfil_info, outgoing);
release:
	CFIL_INFO_VERIFY(cfil_info);
	cfil_release_sockbuf(so, outgoing);

	return error;
}


static void
cfil_flush_queues(struct socket *so, struct cfil_info *cfil_info)
{
	struct cfil_entry *entry;
	int kcunit;
	uint64_t drained;

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || cfil_info == NULL) {
		goto done;
	}

	socket_lock_assert_owned(so);

	/*
	 * Flush the output queues and ignore errors as long as
	 * we are attached
	 */
	(void) cfil_acquire_sockbuf(so, cfil_info, 1);
	if (cfil_info != NULL) {
		drained = 0;
		for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			drained += cfil_queue_drain(&entry->cfe_snd.cfe_ctl_q);
			drained += cfil_queue_drain(&entry->cfe_snd.cfe_pending_q);
		}
		drained += cfil_queue_drain(&cfil_info->cfi_snd.cfi_inject_q);

		if (drained) {
			if (cfil_info->cfi_flags & CFIF_DROP) {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_out_drop);
			} else {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_out_close);
			}
		}
	}
	cfil_release_sockbuf(so, 1);

	/*
	 * Flush the input queues
	 */
	(void) cfil_acquire_sockbuf(so, cfil_info, 0);
	if (cfil_info != NULL) {
		drained = 0;
		for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			drained += cfil_queue_drain(
				&entry->cfe_rcv.cfe_ctl_q);
			drained += cfil_queue_drain(
				&entry->cfe_rcv.cfe_pending_q);
		}
		drained += cfil_queue_drain(&cfil_info->cfi_rcv.cfi_inject_q);

		if (drained) {
			if (cfil_info->cfi_flags & CFIF_DROP) {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_in_drop);
			} else {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_in_close);
			}
		}
	}
	cfil_release_sockbuf(so, 0);
done:
	CFIL_INFO_VERIFY(cfil_info);
}

int
cfil_action_drop(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit)
{
	errno_t error = 0;
	struct cfil_entry *entry;
	struct proc *p;

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || cfil_info == NULL) {
		goto done;
	}

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];

	/* Are we attached to the filter? */
	if (entry->cfe_filter == NULL) {
		goto done;
	}

	cfil_info->cfi_flags |= CFIF_DROP;

	p = current_proc();

	/*
	 * Force the socket to be marked defunct
	 * (forcing fixed along with rdar://19391339)
	 */
	if (so->so_cfil_db == NULL) {
		error = sosetdefunct(p, so,
		    SHUTDOWN_SOCKET_LEVEL_CONTENT_FILTER | SHUTDOWN_SOCKET_LEVEL_DISCONNECT_ALL,
		    FALSE);

		/* Flush the socket buffer and disconnect */
		if (error == 0) {
			error = sodefunct(p, so,
			    SHUTDOWN_SOCKET_LEVEL_CONTENT_FILTER | SHUTDOWN_SOCKET_LEVEL_DISCONNECT_ALL);
		}
	}

	/* The filter is done, mark as detached */
	entry->cfe_flags |= CFEF_CFIL_DETACHED;
#if LIFECYCLE_DEBUG
	cfil_info_log(LOG_ERR, cfil_info, "CFIL: LIFECYCLE: DROP - DETACH");
#endif
	CFIL_LOG(LOG_INFO, "so %llx detached %u",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);

	/* Pending data needs to go */
	cfil_flush_queues(so, cfil_info);

	if (cfil_info && (cfil_info->cfi_flags & CFIF_CLOSE_WAIT)) {
		if (cfil_filters_attached(so) == 0) {
			CFIL_LOG(LOG_INFO, "so %llx waking",
			    (uint64_t)VM_KERNEL_ADDRPERM(so));
			wakeup((caddr_t)cfil_info);
		}
	}
done:
	return error;
}

int
cfil_action_bless_client(uint32_t kcunit, struct cfil_msg_hdr *msghdr)
{
	errno_t error = 0;
	struct cfil_info *cfil_info = NULL;

	bool cfil_attached = false;
	struct cfil_msg_bless_client *blessmsg = (struct cfil_msg_bless_client *)msghdr;

	// Search and lock socket
	struct socket *so = cfil_socket_from_client_uuid(blessmsg->cfb_client_uuid, &cfil_attached);
	if (so == NULL) {
		error = ENOENT;
	} else {
		// The client gets a pass automatically
		cfil_info = (so->so_cfil_db != NULL) ?
		    cfil_db_get_cfil_info(so->so_cfil_db, msghdr->cfm_sock_id) : so->so_cfil;

		if (cfil_attached) {
#if VERDICT_DEBUG
			if (cfil_info != NULL) {
				CFIL_LOG(LOG_ERR, "CFIL: VERDICT RECEIVED: BLESS %s <so %llx sockID %llu>",
				    cfil_info->cfi_hash_entry ? "UDP" : "TCP",
				    (uint64_t)VM_KERNEL_ADDRPERM(so),
				    cfil_info->cfi_sock_id);
			}
#endif
			(void)cfil_action_data_pass(so, cfil_info, kcunit, 1, CFM_MAX_OFFSET, CFM_MAX_OFFSET);
			(void)cfil_action_data_pass(so, cfil_info, kcunit, 0, CFM_MAX_OFFSET, CFM_MAX_OFFSET);
		} else {
			so->so_flags1 |= SOF1_CONTENT_FILTER_SKIP;
		}
		socket_unlock(so, 1);
	}

	return error;
}

static int
cfil_update_entry_offsets(struct socket *so, struct cfil_info *cfil_info, int outgoing, unsigned int datalen)
{
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	uint32_t kcunit;

	CFIL_LOG(LOG_INFO, "so %llx outgoing %d datalen %u",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing, datalen);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		entry = &cfil_info->cfi_entries[kcunit - 1];

		/* Are we attached to the filter? */
		if (entry->cfe_filter == NULL) {
			continue;
		}

		if (outgoing) {
			entrybuf = &entry->cfe_snd;
		} else {
			entrybuf = &entry->cfe_rcv;
		}

		entrybuf->cfe_ctl_q.q_start += datalen;
		entrybuf->cfe_pass_offset = entrybuf->cfe_ctl_q.q_start;
		entrybuf->cfe_peeked = entrybuf->cfe_ctl_q.q_start;
		if (entrybuf->cfe_peek_offset < entrybuf->cfe_pass_offset) {
			entrybuf->cfe_peek_offset = entrybuf->cfe_pass_offset;
		}

		entrybuf->cfe_ctl_q.q_end += datalen;

		entrybuf->cfe_pending_q.q_start += datalen;
		entrybuf->cfe_pending_q.q_end += datalen;
	}
	CFIL_INFO_VERIFY(cfil_info);
	return 0;
}

int
cfil_data_common(struct socket *so, struct cfil_info *cfil_info, int outgoing, struct sockaddr *to,
    struct mbuf *data, struct mbuf *control, uint32_t flags)
{
#pragma unused(to, control, flags)
	errno_t error = 0;
	unsigned int datalen;
	int mbcnt = 0;
	int mbnum = 0;
	int kcunit;
	struct cfi_buf *cfi_buf;
	struct mbuf *chain = NULL;

	if (cfil_info == NULL) {
		CFIL_LOG(LOG_ERR, "so %llx cfil detached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = 0;
		goto done;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EPIPE;
		goto done;
	}

	datalen = cfil_data_length(data, &mbcnt, &mbnum);

	if (outgoing) {
		cfi_buf = &cfil_info->cfi_snd;
	} else {
		cfi_buf = &cfil_info->cfi_rcv;
	}

	cfi_buf->cfi_pending_last += datalen;
	cfi_buf->cfi_pending_mbcnt += mbcnt;
	cfi_buf->cfi_pending_mbnum += mbnum;

	if (IS_UDP(so)) {
		if (cfi_buf->cfi_pending_mbnum > cfil_udp_gc_mbuf_num_max ||
		    cfi_buf->cfi_pending_mbcnt > cfil_udp_gc_mbuf_cnt_max) {
			cfi_buf->cfi_tail_drop_cnt++;
			cfi_buf->cfi_pending_mbcnt -= mbcnt;
			cfi_buf->cfi_pending_mbnum -= mbnum;
			return EPIPE;
		}
	}

	cfil_info_buf_verify(cfi_buf);

#if DATA_DEBUG
	CFIL_LOG(LOG_DEBUG, "CFIL: QUEUEING DATA: <so %llx> %s: data %llx len %u flags 0x%x nextpkt %llx - cfi_pending_last %llu cfi_pending_mbcnt %u   cfi_pass_offset %llu",
	    (uint64_t)VM_KERNEL_ADDRPERM(so),
	    outgoing ? "OUT" : "IN",
	    (uint64_t)VM_KERNEL_ADDRPERM(data), datalen, data->m_flags,
	    (uint64_t)VM_KERNEL_ADDRPERM(data->m_nextpkt),
	    cfi_buf->cfi_pending_last,
	    cfi_buf->cfi_pending_mbcnt,
	    cfi_buf->cfi_pass_offset);
#endif

	/* Fast path when below pass offset */
	if (cfi_buf->cfi_pending_last <= cfi_buf->cfi_pass_offset) {
		cfil_update_entry_offsets(so, cfil_info, outgoing, datalen);
#if DATA_DEBUG
		CFIL_LOG(LOG_DEBUG, "CFIL: QUEUEING DATA: FAST PATH");
#endif
	} else {
		for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			// Is cfil attached to this filter?
			if (IS_ENTRY_ATTACHED(cfil_info, kcunit)) {
				if (IS_UDP(so)) {
					/* UDP only:
					 * Chain addr (incoming only TDB), control (optional) and data into one chain.
					 * This full chain will be reinjected into socket after recieving verdict.
					 */
					(void) cfil_udp_save_socket_state(cfil_info, data);
					chain = sbconcat_mbufs(NULL, outgoing ? NULL : to, data, control);
					if (chain == NULL) {
						return ENOBUFS;
					}
					data = chain;
				}
				error = cfil_data_filter(so, cfil_info, kcunit, outgoing, data,
				    datalen);
			}
			/* 0 means passed so continue with next filter */
			if (error != 0) {
				break;
			}
		}
	}

	/* Move cursor if no filter claimed the data */
	if (error == 0) {
		cfi_buf->cfi_pending_first += datalen;
		cfi_buf->cfi_pending_mbcnt -= mbcnt;
		cfi_buf->cfi_pending_mbnum -= mbnum;
		cfil_info_buf_verify(cfi_buf);
	}
done:
	CFIL_INFO_VERIFY(cfil_info);

	return error;
}

/*
 * Callback from socket layer sosendxxx()
 */
int
cfil_sock_data_out(struct socket *so, struct sockaddr  *to,
    struct mbuf *data, struct mbuf *control, uint32_t flags)
{
	int error = 0;

	if (IS_UDP(so)) {
		return cfil_sock_udp_handle_data(TRUE, so, NULL, to, data, control, flags);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return 0;
	}

	socket_lock_assert_owned(so);

	if (so->so_cfil->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		return EPIPE;
	}
	if (control != NULL) {
		CFIL_LOG(LOG_ERR, "so %llx control",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_out_control);
	}
	if ((flags & MSG_OOB)) {
		CFIL_LOG(LOG_ERR, "so %llx MSG_OOB",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_out_oob);
	}
	if ((so->so_snd.sb_flags & SB_LOCK) == 0) {
		panic("so %p SB_LOCK not set", so);
	}

	if (so->so_snd.sb_cfil_thread != NULL) {
		panic("%s sb_cfil_thread %p not NULL", __func__,
		    so->so_snd.sb_cfil_thread);
	}

	error = cfil_data_common(so, so->so_cfil, 1, to, data, control, flags);

	return error;
}

/*
 * Callback from socket layer sbappendxxx()
 */
int
cfil_sock_data_in(struct socket *so, struct sockaddr *from,
    struct mbuf *data, struct mbuf *control, uint32_t flags)
{
	int error = 0;

	if (IS_UDP(so)) {
		return cfil_sock_udp_handle_data(FALSE, so, NULL, from, data, control, flags);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return 0;
	}

	socket_lock_assert_owned(so);

	if (so->so_cfil->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		return EPIPE;
	}
	if (control != NULL) {
		CFIL_LOG(LOG_ERR, "so %llx control",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_in_control);
	}
	if (data->m_type == MT_OOBDATA) {
		CFIL_LOG(LOG_ERR, "so %llx MSG_OOB",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_in_oob);
	}
	error = cfil_data_common(so, so->so_cfil, 0, from, data, control, flags);

	return error;
}

/*
 * Callback from socket layer soshutdownxxx()
 *
 * We may delay the shutdown write if there's outgoing data in process.
 *
 * There is no point in delaying the shutdown read because the process
 * indicated that it does not want to read anymore data.
 */
int
cfil_sock_shutdown(struct socket *so, int *how)
{
	int error = 0;

	if (IS_UDP(so)) {
		return cfil_sock_udp_shutdown(so, how);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		goto done;
	}

	socket_lock_assert_owned(so);

	CFIL_LOG(LOG_INFO, "so %llx how %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), *how);

	/*
	 * Check the state of the socket before the content filter
	 */
	if (*how != SHUT_WR && (so->so_state & SS_CANTRCVMORE) != 0) {
		/* read already shut down */
		error = ENOTCONN;
		goto done;
	}
	if (*how != SHUT_RD && (so->so_state & SS_CANTSENDMORE) != 0) {
		/* write already shut down */
		error = ENOTCONN;
		goto done;
	}

	if ((so->so_cfil->cfi_flags & CFIF_DROP) != 0) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		goto done;
	}

	/*
	 * shutdown read: SHUT_RD or SHUT_RDWR
	 */
	if (*how != SHUT_WR) {
		if (so->so_cfil->cfi_flags & CFIF_SHUT_RD) {
			error = ENOTCONN;
			goto done;
		}
		so->so_cfil->cfi_flags |= CFIF_SHUT_RD;
		cfil_sock_notify_shutdown(so, SHUT_RD);
	}
	/*
	 * shutdown write: SHUT_WR or SHUT_RDWR
	 */
	if (*how != SHUT_RD) {
		if (so->so_cfil->cfi_flags & CFIF_SHUT_WR) {
			error = ENOTCONN;
			goto done;
		}
		so->so_cfil->cfi_flags |= CFIF_SHUT_WR;
		cfil_sock_notify_shutdown(so, SHUT_WR);
		/*
		 * When outgoing data is pending, we delay the shutdown at the
		 * protocol level until the content filters give the final
		 * verdict on the pending data.
		 */
		if (cfil_sock_data_pending(&so->so_snd) != 0) {
			/*
			 * When shutting down the read and write sides at once
			 * we can proceed to the final shutdown of the read
			 * side. Otherwise, we just return.
			 */
			if (*how == SHUT_WR) {
				error = EJUSTRETURN;
			} else if (*how == SHUT_RDWR) {
				*how = SHUT_RD;
			}
		}
	}
done:
	return error;
}

/*
 * This is called when the socket is closed and there is no more
 * opportunity for filtering
 */
void
cfil_sock_is_closed(struct socket *so)
{
	errno_t error = 0;
	int kcunit;

	if (IS_UDP(so)) {
		cfil_sock_udp_is_closed(so);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	CFIL_LOG(LOG_INFO, "so %llx", (uint64_t)VM_KERNEL_ADDRPERM(so));

	socket_lock_assert_owned(so);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		/* Let the filters know of the closing */
		error = cfil_dispatch_closed_event(so, so->so_cfil, kcunit);
	}

	/* Last chance to push passed data out */
	error = cfil_acquire_sockbuf(so, so->so_cfil, 1);
	if (error == 0) {
		cfil_service_inject_queue(so, so->so_cfil, 1);
	}
	cfil_release_sockbuf(so, 1);

	so->so_cfil->cfi_flags |= CFIF_SOCK_CLOSED;

	/* Pending data needs to go */
	cfil_flush_queues(so, so->so_cfil);

	CFIL_INFO_VERIFY(so->so_cfil);
}

/*
 * This is called when the socket is disconnected so let the filters
 * know about the disconnection and that no more data will come
 *
 * The how parameter has the same values as soshutown()
 */
void
cfil_sock_notify_shutdown(struct socket *so, int how)
{
	errno_t error = 0;
	int kcunit;

	if (IS_UDP(so)) {
		cfil_sock_udp_notify_shutdown(so, how, 0, 0);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	CFIL_LOG(LOG_INFO, "so %llx how %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), how);

	socket_lock_assert_owned(so);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		/* Disconnect incoming side */
		if (how != SHUT_WR) {
			error = cfil_dispatch_disconnect_event(so, so->so_cfil, kcunit, 0);
		}
		/* Disconnect outgoing side */
		if (how != SHUT_RD) {
			error = cfil_dispatch_disconnect_event(so, so->so_cfil, kcunit, 1);
		}
	}
}

static int
cfil_filters_attached(struct socket *so)
{
	struct cfil_entry *entry;
	uint32_t kcunit;
	int attached = 0;

	if (IS_UDP(so)) {
		return cfil_filters_udp_attached(so, FALSE);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return 0;
	}

	socket_lock_assert_owned(so);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		entry = &so->so_cfil->cfi_entries[kcunit - 1];

		/* Are we attached to the filter? */
		if (entry->cfe_filter == NULL) {
			continue;
		}
		if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
			continue;
		}
		if ((entry->cfe_flags & CFEF_CFIL_DETACHED) != 0) {
			continue;
		}
		attached = 1;
		break;
	}

	return attached;
}

/*
 * This is called when the socket is closed and we are waiting for
 * the filters to gives the final pass or drop
 */
void
cfil_sock_close_wait(struct socket *so)
{
	lck_mtx_t *mutex_held;
	struct timespec ts;
	int error;

	if (IS_UDP(so)) {
		cfil_sock_udp_close_wait(so);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	CFIL_LOG(LOG_INFO, "so %llx", (uint64_t)VM_KERNEL_ADDRPERM(so));

	if (so->so_proto->pr_getlock != NULL) {
		mutex_held = (*so->so_proto->pr_getlock)(so, PR_F_WILLUNLOCK);
	} else {
		mutex_held = so->so_proto->pr_domain->dom_mtx;
	}
	LCK_MTX_ASSERT(mutex_held, LCK_MTX_ASSERT_OWNED);

	while (cfil_filters_attached(so)) {
		/*
		 * Notify the filters we are going away so they can detach
		 */
		cfil_sock_notify_shutdown(so, SHUT_RDWR);

		/*
		 * Make sure we need to wait after the filter are notified
		 * of the disconnection
		 */
		if (cfil_filters_attached(so) == 0) {
			break;
		}

		CFIL_LOG(LOG_INFO, "so %llx waiting",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));

		ts.tv_sec = cfil_close_wait_timeout / 1000;
		ts.tv_nsec = (cfil_close_wait_timeout % 1000) *
		    NSEC_PER_USEC * 1000;

		OSIncrementAtomic(&cfil_stats.cfs_close_wait);
		so->so_cfil->cfi_flags |= CFIF_CLOSE_WAIT;
		error = msleep((caddr_t)so->so_cfil, mutex_held,
		    PSOCK | PCATCH, "cfil_sock_close_wait", &ts);
		so->so_cfil->cfi_flags &= ~CFIF_CLOSE_WAIT;

		CFIL_LOG(LOG_NOTICE, "so %llx timed out %d",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), (error != 0));

		/*
		 * Force close in case of timeout
		 */
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_close_wait_timeout);
			break;
		}
	}
}

/*
 * Returns the size of the data held by the content filter by using
 */
int32_t
cfil_sock_data_pending(struct sockbuf *sb)
{
	struct socket *so = sb->sb_so;
	uint64_t pending = 0;

	if (IS_UDP(so)) {
		return cfil_sock_udp_data_pending(sb, FALSE);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil != NULL) {
		struct cfi_buf *cfi_buf;

		socket_lock_assert_owned(so);

		if ((sb->sb_flags & SB_RECV) == 0) {
			cfi_buf = &so->so_cfil->cfi_snd;
		} else {
			cfi_buf = &so->so_cfil->cfi_rcv;
		}

		pending = cfi_buf->cfi_pending_last -
		    cfi_buf->cfi_pending_first;

		/*
		 * If we are limited by the "chars of mbufs used" roughly
		 * adjust so we won't overcommit
		 */
		if (pending > (uint64_t)cfi_buf->cfi_pending_mbcnt) {
			pending = cfi_buf->cfi_pending_mbcnt;
		}
	}

	VERIFY(pending < INT32_MAX);

	return (int32_t)(pending);
}

/*
 * Return the socket buffer space used by data being held by content filters
 * so processes won't clog the socket buffer
 */
int32_t
cfil_sock_data_space(struct sockbuf *sb)
{
	struct socket *so = sb->sb_so;
	uint64_t pending = 0;

	if (IS_UDP(so)) {
		return cfil_sock_udp_data_pending(sb, TRUE);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil != NULL &&
	    so->so_snd.sb_cfil_thread != current_thread()) {
		struct cfi_buf *cfi_buf;

		socket_lock_assert_owned(so);

		if ((sb->sb_flags & SB_RECV) == 0) {
			cfi_buf = &so->so_cfil->cfi_snd;
		} else {
			cfi_buf = &so->so_cfil->cfi_rcv;
		}

		pending = cfi_buf->cfi_pending_last -
		    cfi_buf->cfi_pending_first;

		/*
		 * If we are limited by the "chars of mbufs used" roughly
		 * adjust so we won't overcommit
		 */
		if ((uint64_t)cfi_buf->cfi_pending_mbcnt > pending) {
			pending = cfi_buf->cfi_pending_mbcnt;
		}
	}

	VERIFY(pending < INT32_MAX);

	return (int32_t)(pending);
}

/*
 * A callback from the socket and protocol layer when data becomes
 * available in the socket buffer to give a chance for the content filter
 * to re-inject data that was held back
 */
void
cfil_sock_buf_update(struct sockbuf *sb)
{
	int outgoing;
	int error;
	struct socket *so = sb->sb_so;

	if (IS_UDP(so)) {
		cfil_sock_udp_buf_update(sb);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	if (!cfil_sbtrim) {
		return;
	}

	socket_lock_assert_owned(so);

	if ((sb->sb_flags & SB_RECV) == 0) {
		if ((so->so_cfil->cfi_flags & CFIF_RETRY_INJECT_OUT) == 0) {
			return;
		}
		outgoing = 1;
		OSIncrementAtomic(&cfil_stats.cfs_inject_q_out_retry);
	} else {
		if ((so->so_cfil->cfi_flags & CFIF_RETRY_INJECT_IN) == 0) {
			return;
		}
		outgoing = 0;
		OSIncrementAtomic(&cfil_stats.cfs_inject_q_in_retry);
	}

	CFIL_LOG(LOG_NOTICE, "so %llx outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing);

	error = cfil_acquire_sockbuf(so, so->so_cfil, outgoing);
	if (error == 0) {
		cfil_service_inject_queue(so, so->so_cfil, outgoing);
	}
	cfil_release_sockbuf(so, outgoing);
}

int
sysctl_cfil_filter_list(struct sysctl_oid *oidp, void *arg1, int arg2,
    struct sysctl_req *req)
{
#pragma unused(oidp, arg1, arg2)
	int error = 0;
	size_t len = 0;
	u_int32_t i;

	/* Read only  */
	if (req->newptr != USER_ADDR_NULL) {
		return EPERM;
	}

	cfil_rw_lock_shared(&cfil_lck_rw);

	for (i = 0; content_filters != NULL && i < MAX_CONTENT_FILTER; i++) {
		struct cfil_filter_stat filter_stat;
		struct content_filter *cfc = content_filters[i];

		if (cfc == NULL) {
			continue;
		}

		/* If just asking for the size */
		if (req->oldptr == USER_ADDR_NULL) {
			len += sizeof(struct cfil_filter_stat);
			continue;
		}

		bzero(&filter_stat, sizeof(struct cfil_filter_stat));
		filter_stat.cfs_len = sizeof(struct cfil_filter_stat);
		filter_stat.cfs_filter_id = cfc->cf_kcunit;
		filter_stat.cfs_flags = cfc->cf_flags;
		filter_stat.cfs_sock_count = cfc->cf_sock_count;
		filter_stat.cfs_necp_control_unit = cfc->cf_necp_control_unit;

		error = SYSCTL_OUT(req, &filter_stat,
		    sizeof(struct cfil_filter_stat));
		if (error != 0) {
			break;
		}
	}
	/* If just asking for the size */
	if (req->oldptr == USER_ADDR_NULL) {
		req->oldidx = len;
	}

	cfil_rw_unlock_shared(&cfil_lck_rw);

#if SHOW_DEBUG
	if (req->oldptr != USER_ADDR_NULL) {
		for (i = 1; content_filters != NULL && i <= MAX_CONTENT_FILTER; i++) {
			cfil_filter_show(i);
		}
	}
#endif

	return error;
}

static int
sysctl_cfil_sock_list(struct sysctl_oid *oidp, void *arg1, int arg2,
    struct sysctl_req *req)
{
#pragma unused(oidp, arg1, arg2)
	int error = 0;
	u_int32_t i;
	struct cfil_info *cfi;

	/* Read only  */
	if (req->newptr != USER_ADDR_NULL) {
		return EPERM;
	}

	cfil_rw_lock_shared(&cfil_lck_rw);

	/*
	 * If just asking for the size,
	 */
	if (req->oldptr == USER_ADDR_NULL) {
		req->oldidx = cfil_sock_attached_count *
		    sizeof(struct cfil_sock_stat);
		/* Bump the length in case new sockets gets attached */
		req->oldidx += req->oldidx >> 3;
		goto done;
	}

	TAILQ_FOREACH(cfi, &cfil_sock_head, cfi_link) {
		struct cfil_entry *entry;
		struct cfil_sock_stat stat;
		struct socket *so = cfi->cfi_so;

		bzero(&stat, sizeof(struct cfil_sock_stat));
		stat.cfs_len = sizeof(struct cfil_sock_stat);
		stat.cfs_sock_id = cfi->cfi_sock_id;
		stat.cfs_flags = cfi->cfi_flags;

		if (so != NULL) {
			stat.cfs_pid = so->last_pid;
			memcpy(stat.cfs_uuid, so->last_uuid,
			    sizeof(uuid_t));
			if (so->so_flags & SOF_DELEGATED) {
				stat.cfs_e_pid = so->e_pid;
				memcpy(stat.cfs_e_uuid, so->e_uuid,
				    sizeof(uuid_t));
			} else {
				stat.cfs_e_pid = so->last_pid;
				memcpy(stat.cfs_e_uuid, so->last_uuid,
				    sizeof(uuid_t));
			}

			stat.cfs_sock_family = so->so_proto->pr_domain->dom_family;
			stat.cfs_sock_type = so->so_proto->pr_type;
			stat.cfs_sock_protocol = so->so_proto->pr_protocol;
		}

		stat.cfs_snd.cbs_pending_first =
		    cfi->cfi_snd.cfi_pending_first;
		stat.cfs_snd.cbs_pending_last =
		    cfi->cfi_snd.cfi_pending_last;
		stat.cfs_snd.cbs_inject_q_len =
		    cfil_queue_len(&cfi->cfi_snd.cfi_inject_q);
		stat.cfs_snd.cbs_pass_offset =
		    cfi->cfi_snd.cfi_pass_offset;

		stat.cfs_rcv.cbs_pending_first =
		    cfi->cfi_rcv.cfi_pending_first;
		stat.cfs_rcv.cbs_pending_last =
		    cfi->cfi_rcv.cfi_pending_last;
		stat.cfs_rcv.cbs_inject_q_len =
		    cfil_queue_len(&cfi->cfi_rcv.cfi_inject_q);
		stat.cfs_rcv.cbs_pass_offset =
		    cfi->cfi_rcv.cfi_pass_offset;

		for (i = 0; i < MAX_CONTENT_FILTER; i++) {
			struct cfil_entry_stat *estat;
			struct cfe_buf *ebuf;
			struct cfe_buf_stat *sbuf;

			entry = &cfi->cfi_entries[i];

			estat = &stat.ces_entries[i];

			estat->ces_len = sizeof(struct cfil_entry_stat);
			estat->ces_filter_id = entry->cfe_filter ?
			    entry->cfe_filter->cf_kcunit : 0;
			estat->ces_flags = entry->cfe_flags;
			estat->ces_necp_control_unit =
			    entry->cfe_necp_control_unit;

			estat->ces_last_event.tv_sec =
			    (int64_t)entry->cfe_last_event.tv_sec;
			estat->ces_last_event.tv_usec =
			    (int64_t)entry->cfe_last_event.tv_usec;

			estat->ces_last_action.tv_sec =
			    (int64_t)entry->cfe_last_action.tv_sec;
			estat->ces_last_action.tv_usec =
			    (int64_t)entry->cfe_last_action.tv_usec;

			ebuf = &entry->cfe_snd;
			sbuf = &estat->ces_snd;
			sbuf->cbs_pending_first =
			    cfil_queue_offset_first(&ebuf->cfe_pending_q);
			sbuf->cbs_pending_last =
			    cfil_queue_offset_last(&ebuf->cfe_pending_q);
			sbuf->cbs_ctl_first =
			    cfil_queue_offset_first(&ebuf->cfe_ctl_q);
			sbuf->cbs_ctl_last =
			    cfil_queue_offset_last(&ebuf->cfe_ctl_q);
			sbuf->cbs_pass_offset =  ebuf->cfe_pass_offset;
			sbuf->cbs_peek_offset =  ebuf->cfe_peek_offset;
			sbuf->cbs_peeked =  ebuf->cfe_peeked;

			ebuf = &entry->cfe_rcv;
			sbuf = &estat->ces_rcv;
			sbuf->cbs_pending_first =
			    cfil_queue_offset_first(&ebuf->cfe_pending_q);
			sbuf->cbs_pending_last =
			    cfil_queue_offset_last(&ebuf->cfe_pending_q);
			sbuf->cbs_ctl_first =
			    cfil_queue_offset_first(&ebuf->cfe_ctl_q);
			sbuf->cbs_ctl_last =
			    cfil_queue_offset_last(&ebuf->cfe_ctl_q);
			sbuf->cbs_pass_offset =  ebuf->cfe_pass_offset;
			sbuf->cbs_peek_offset =  ebuf->cfe_peek_offset;
			sbuf->cbs_peeked =  ebuf->cfe_peeked;
		}
		error = SYSCTL_OUT(req, &stat,
		    sizeof(struct cfil_sock_stat));
		if (error != 0) {
			break;
		}
	}
done:
	cfil_rw_unlock_shared(&cfil_lck_rw);

#if SHOW_DEBUG
	if (req->oldptr != USER_ADDR_NULL) {
		cfil_info_show();
	}
#endif

	return error;
}

/*
 * UDP Socket Support
 */
static void
cfil_hash_entry_log(int level, struct socket *so, struct cfil_hash_entry *entry, uint64_t sockId, const char* msg)
{
	char local[MAX_IPv6_STR_LEN + 6];
	char remote[MAX_IPv6_STR_LEN + 6];
	const void  *addr;

	// No sock or not UDP, no-op
	if (so == NULL || entry == NULL) {
		return;
	}

	local[0] = remote[0] = 0x0;

	switch (entry->cfentry_family) {
	case AF_INET6:
		addr = &entry->cfentry_laddr.addr6;
		inet_ntop(AF_INET6, addr, local, sizeof(local));
		addr = &entry->cfentry_faddr.addr6;
		inet_ntop(AF_INET6, addr, remote, sizeof(local));
		break;
	case AF_INET:
		addr = &entry->cfentry_laddr.addr46.ia46_addr4.s_addr;
		inet_ntop(AF_INET, addr, local, sizeof(local));
		addr = &entry->cfentry_faddr.addr46.ia46_addr4.s_addr;
		inet_ntop(AF_INET, addr, remote, sizeof(local));
		break;
	default:
		return;
	}

	CFIL_LOG(level, "<%s>: <UDP so %llx, entry %p, sockID %llu> lport %d fport %d laddr %s faddr %s",
	    msg,
	    (uint64_t)VM_KERNEL_ADDRPERM(so), entry, sockId,
	    ntohs(entry->cfentry_lport), ntohs(entry->cfentry_fport), local, remote);
}

static void
cfil_inp_log(int level, struct socket *so, const char* msg)
{
	struct inpcb *inp = NULL;
	char local[MAX_IPv6_STR_LEN + 6];
	char remote[MAX_IPv6_STR_LEN + 6];
	const void  *addr;

	if (so == NULL) {
		return;
	}

	inp = sotoinpcb(so);
	if (inp == NULL) {
		return;
	}

	local[0] = remote[0] = 0x0;

#if INET6
	if (inp->inp_vflag & INP_IPV6) {
		addr = &inp->in6p_laddr.s6_addr32;
		inet_ntop(AF_INET6, addr, local, sizeof(local));
		addr = &inp->in6p_faddr.s6_addr32;
		inet_ntop(AF_INET6, addr, remote, sizeof(local));
	} else
#endif /* INET6 */
	{
		addr = &inp->inp_laddr.s_addr;
		inet_ntop(AF_INET, addr, local, sizeof(local));
		addr = &inp->inp_faddr.s_addr;
		inet_ntop(AF_INET, addr, remote, sizeof(local));
	}

	if (so->so_cfil != NULL) {
		CFIL_LOG(level, "<%s>: <%s so %llx - flags 0x%x 0x%x, sockID %llu> lport %d fport %d laddr %s faddr %s",
		    msg, IS_UDP(so) ? "UDP" : "TCP",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), inp->inp_flags, inp->inp_socket->so_flags, so->so_cfil->cfi_sock_id,
		    ntohs(inp->inp_lport), ntohs(inp->inp_fport), local, remote);
	} else {
		CFIL_LOG(level, "<%s>: <%s so %llx - flags 0x%x 0x%x> lport %d fport %d laddr %s faddr %s",
		    msg, IS_UDP(so) ? "UDP" : "TCP",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), inp->inp_flags, inp->inp_socket->so_flags,
		    ntohs(inp->inp_lport), ntohs(inp->inp_fport), local, remote);
	}
}

static void
cfil_info_log(int level, struct cfil_info *cfil_info, const char* msg)
{
	if (cfil_info == NULL) {
		return;
	}

	if (cfil_info->cfi_hash_entry != NULL) {
		cfil_hash_entry_log(level, cfil_info->cfi_so, cfil_info->cfi_hash_entry, cfil_info->cfi_sock_id, msg);
	} else {
		cfil_inp_log(level, cfil_info->cfi_so, msg);
	}
}

errno_t
cfil_db_init(struct socket *so)
{
	errno_t error = 0;
	struct cfil_db *db = NULL;

	CFIL_LOG(LOG_INFO, "");

	db = zalloc(cfil_db_zone);
	if (db == NULL) {
		error = ENOMEM;
		goto done;
	}
	bzero(db, sizeof(struct cfil_db));
	db->cfdb_so = so;
	db->cfdb_hashbase = hashinit(CFILHASHSIZE, M_CFIL, &db->cfdb_hashmask);
	if (db->cfdb_hashbase == NULL) {
		zfree(cfil_db_zone, db);
		db = NULL;
		error = ENOMEM;
		goto done;
	}

	so->so_cfil_db = db;

done:
	return error;
}

void
cfil_db_free(struct socket *so)
{
	struct cfil_hash_entry *entry = NULL;
	struct cfil_hash_entry *temp_entry = NULL;
	struct cfilhashhead *cfilhash = NULL;
	struct cfil_db *db = NULL;

	CFIL_LOG(LOG_INFO, "");

	if (so == NULL || so->so_cfil_db == NULL) {
		return;
	}
	db = so->so_cfil_db;

#if LIFECYCLE_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: LIFECYCLE: <so %llx, db %p> freeing db (count == %d)",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), db, db->cfdb_count);
#endif

	for (int i = 0; i < CFILHASHSIZE; i++) {
		cfilhash = &db->cfdb_hashbase[i];
		LIST_FOREACH_SAFE(entry, cfilhash, cfentry_link, temp_entry) {
			if (entry->cfentry_cfil != NULL) {
#if LIFECYCLE_DEBUG
				cfil_info_log(LOG_ERR, entry->cfentry_cfil, "CFIL: LIFECYCLE: DB FREE CLEAN UP");
#endif
				cfil_info_free(entry->cfentry_cfil);
				OSIncrementAtomic(&cfil_stats.cfs_sock_detached);
				entry->cfentry_cfil = NULL;
			}

			cfil_db_delete_entry(db, entry);
			if (so->so_flags & SOF_CONTENT_FILTER) {
				if (db->cfdb_count == 0) {
					so->so_flags &= ~SOF_CONTENT_FILTER;
				}
				VERIFY(so->so_usecount > 0);
				so->so_usecount--;
			}
		}
	}

	// Make sure all entries are cleaned up!
	VERIFY(db->cfdb_count == 0);
#if LIFECYCLE_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: LIFECYCLE: so usecount %d", so->so_usecount);
#endif

	FREE(db->cfdb_hashbase, M_CFIL);
	zfree(cfil_db_zone, db);
	so->so_cfil_db = NULL;
}

static bool
fill_cfil_hash_entry_from_address(struct cfil_hash_entry *entry, bool isLocal, struct sockaddr *addr)
{
	struct sockaddr_in *sin = NULL;
	struct sockaddr_in6 *sin6 = NULL;

	if (entry == NULL || addr == NULL) {
		return FALSE;
	}

	switch (addr->sa_family) {
	case AF_INET:
		sin = satosin(addr);
		if (sin->sin_len != sizeof(*sin)) {
			return FALSE;
		}
		if (isLocal == TRUE) {
			entry->cfentry_lport = sin->sin_port;
			entry->cfentry_laddr.addr46.ia46_addr4.s_addr = sin->sin_addr.s_addr;
		} else {
			entry->cfentry_fport = sin->sin_port;
			entry->cfentry_faddr.addr46.ia46_addr4.s_addr = sin->sin_addr.s_addr;
		}
		entry->cfentry_family = AF_INET;
		return TRUE;
	case AF_INET6:
		sin6 = satosin6(addr);
		if (sin6->sin6_len != sizeof(*sin6)) {
			return FALSE;
		}
		if (isLocal == TRUE) {
			entry->cfentry_lport = sin6->sin6_port;
			entry->cfentry_laddr.addr6 = sin6->sin6_addr;
		} else {
			entry->cfentry_fport = sin6->sin6_port;
			entry->cfentry_faddr.addr6 = sin6->sin6_addr;
		}
		entry->cfentry_family = AF_INET6;
		return TRUE;
	default:
		return FALSE;
	}
}

static bool
fill_cfil_hash_entry_from_inp(struct cfil_hash_entry *entry, bool isLocal, struct inpcb *inp)
{
	if (entry == NULL || inp == NULL) {
		return FALSE;
	}

	if (inp->inp_vflag & INP_IPV4) {
		if (isLocal == TRUE) {
			entry->cfentry_lport = inp->inp_lport;
			entry->cfentry_laddr.addr46.ia46_addr4.s_addr = inp->inp_laddr.s_addr;
		} else {
			entry->cfentry_fport = inp->inp_fport;
			entry->cfentry_faddr.addr46.ia46_addr4.s_addr = inp->inp_faddr.s_addr;
		}
		entry->cfentry_family = AF_INET;
		return TRUE;
	} else if (inp->inp_vflag & INP_IPV6) {
		if (isLocal == TRUE) {
			entry->cfentry_lport = inp->inp_lport;
			entry->cfentry_laddr.addr6 = inp->in6p_laddr;
		} else {
			entry->cfentry_fport = inp->inp_fport;
			entry->cfentry_faddr.addr6 = inp->in6p_faddr;
		}
		entry->cfentry_family = AF_INET6;
		return TRUE;
	}
	return FALSE;
}

bool
check_port(struct sockaddr *addr, u_short port)
{
	struct sockaddr_in *sin = NULL;
	struct sockaddr_in6 *sin6 = NULL;

	if (addr == NULL || port == 0) {
		return FALSE;
	}

	switch (addr->sa_family) {
	case AF_INET:
		sin = satosin(addr);
		if (sin->sin_len != sizeof(*sin)) {
			return FALSE;
		}
		if (port == ntohs(sin->sin_port)) {
			return TRUE;
		}
		break;
	case AF_INET6:
		sin6 = satosin6(addr);
		if (sin6->sin6_len != sizeof(*sin6)) {
			return FALSE;
		}
		if (port == ntohs(sin6->sin6_port)) {
			return TRUE;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

struct cfil_hash_entry *
cfil_db_lookup_entry_with_sockid(struct cfil_db *db, u_int64_t sock_id)
{
	struct cfilhashhead *cfilhash = NULL;
	u_int32_t flowhash = (u_int32_t)(sock_id & 0x0ffffffff);
	struct cfil_hash_entry *nextentry;

	if (db == NULL || db->cfdb_hashbase == NULL || sock_id == 0) {
		return NULL;
	}

	flowhash &= db->cfdb_hashmask;
	cfilhash = &db->cfdb_hashbase[flowhash];

	LIST_FOREACH(nextentry, cfilhash, cfentry_link) {
		if (nextentry->cfentry_cfil != NULL &&
		    nextentry->cfentry_cfil->cfi_sock_id == sock_id) {
			CFIL_LOG(LOG_DEBUG, "CFIL: UDP <so %llx> matched <id %llu, hash %u>",
			    (uint64_t)VM_KERNEL_ADDRPERM(db->cfdb_so), nextentry->cfentry_cfil->cfi_sock_id, flowhash);
			cfil_hash_entry_log(LOG_DEBUG, db->cfdb_so, nextentry, 0, "CFIL: UDP found entry");
			return nextentry;
		}
	}

	CFIL_LOG(LOG_DEBUG, "CFIL: UDP <so %llx> NOT matched <id %llu, hash %u>",
	    (uint64_t)VM_KERNEL_ADDRPERM(db->cfdb_so), sock_id, flowhash);
	return NULL;
}

struct cfil_hash_entry *
cfil_db_lookup_entry(struct cfil_db *db, struct sockaddr *local, struct sockaddr *remote)
{
	struct cfil_hash_entry matchentry;
	struct cfil_hash_entry *nextentry = NULL;
	struct inpcb *inp = sotoinpcb(db->cfdb_so);
	u_int32_t hashkey_faddr = 0, hashkey_laddr = 0;
	int inp_hash_element = 0;
	struct cfilhashhead *cfilhash = NULL;

	CFIL_LOG(LOG_INFO, "");

	if (inp == NULL) {
		goto done;
	}

	if (local != NULL) {
		fill_cfil_hash_entry_from_address(&matchentry, TRUE, local);
	} else {
		fill_cfil_hash_entry_from_inp(&matchentry, TRUE, inp);
	}
	if (remote != NULL) {
		fill_cfil_hash_entry_from_address(&matchentry, FALSE, remote);
	} else {
		fill_cfil_hash_entry_from_inp(&matchentry, FALSE, inp);
	}

#if INET6
	if (inp->inp_vflag & INP_IPV6) {
		hashkey_faddr = matchentry.cfentry_faddr.addr6.s6_addr32[3];
		hashkey_laddr = matchentry.cfentry_laddr.addr6.s6_addr32[3];
	} else
#endif /* INET6 */
	{
		hashkey_faddr = matchentry.cfentry_faddr.addr46.ia46_addr4.s_addr;
		hashkey_laddr = matchentry.cfentry_laddr.addr46.ia46_addr4.s_addr;
	}

	inp_hash_element = CFIL_HASH(hashkey_laddr, hashkey_faddr,
	    matchentry.cfentry_lport, matchentry.cfentry_fport);
	inp_hash_element &= db->cfdb_hashmask;

	cfilhash = &db->cfdb_hashbase[inp_hash_element];

	LIST_FOREACH(nextentry, cfilhash, cfentry_link) {
#if INET6
		if ((inp->inp_vflag & INP_IPV6) &&
		    nextentry->cfentry_lport == matchentry.cfentry_lport &&
		    nextentry->cfentry_fport == matchentry.cfentry_fport &&
		    IN6_ARE_ADDR_EQUAL(&nextentry->cfentry_laddr.addr6, &matchentry.cfentry_laddr.addr6) &&
		    IN6_ARE_ADDR_EQUAL(&nextentry->cfentry_faddr.addr6, &matchentry.cfentry_faddr.addr6)) {
#if DATA_DEBUG
			cfil_hash_entry_log(LOG_DEBUG, db->cfdb_so, &matchentry, 0, "CFIL LOOKUP ENTRY: UDP V6 found entry");
#endif
			return nextentry;
		} else
#endif /* INET6 */
		if (nextentry->cfentry_lport == matchentry.cfentry_lport &&
		    nextentry->cfentry_fport == matchentry.cfentry_fport &&
		    nextentry->cfentry_laddr.addr46.ia46_addr4.s_addr == matchentry.cfentry_laddr.addr46.ia46_addr4.s_addr &&
		    nextentry->cfentry_faddr.addr46.ia46_addr4.s_addr == matchentry.cfentry_faddr.addr46.ia46_addr4.s_addr) {
#if DATA_DEBUG
			cfil_hash_entry_log(LOG_DEBUG, db->cfdb_so, &matchentry, 0, "CFIL LOOKUP ENTRY: UDP V4 found entry");
#endif
			return nextentry;
		}
	}

done:
#if DATA_DEBUG
	cfil_hash_entry_log(LOG_DEBUG, db->cfdb_so, &matchentry, 0, "CFIL LOOKUP ENTRY: UDP no entry found");
#endif
	return NULL;
}

void
cfil_db_delete_entry(struct cfil_db *db, struct cfil_hash_entry *hash_entry)
{
	if (hash_entry == NULL) {
		return;
	}
	if (db == NULL || db->cfdb_count == 0) {
		return;
	}
	db->cfdb_count--;
	if (db->cfdb_only_entry == hash_entry) {
		db->cfdb_only_entry = NULL;
	}
	LIST_REMOVE(hash_entry, cfentry_link);
	zfree(cfil_hash_entry_zone, hash_entry);
}

struct cfil_hash_entry *
cfil_db_add_entry(struct cfil_db *db, struct sockaddr *local, struct sockaddr *remote)
{
	struct cfil_hash_entry *entry = NULL;
	struct inpcb *inp = sotoinpcb(db->cfdb_so);
	u_int32_t hashkey_faddr = 0, hashkey_laddr = 0;
	int inp_hash_element = 0;
	struct cfilhashhead *cfilhash = NULL;

	CFIL_LOG(LOG_INFO, "");

	if (inp == NULL) {
		goto done;
	}

	entry = zalloc(cfil_hash_entry_zone);
	if (entry == NULL) {
		goto done;
	}
	bzero(entry, sizeof(struct cfil_hash_entry));

	if (local != NULL) {
		fill_cfil_hash_entry_from_address(entry, TRUE, local);
	} else {
		fill_cfil_hash_entry_from_inp(entry, TRUE, inp);
	}
	if (remote != NULL) {
		fill_cfil_hash_entry_from_address(entry, FALSE, remote);
	} else {
		fill_cfil_hash_entry_from_inp(entry, FALSE, inp);
	}
	entry->cfentry_lastused = net_uptime();

#if INET6
	if (inp->inp_vflag & INP_IPV6) {
		hashkey_faddr = entry->cfentry_faddr.addr6.s6_addr32[3];
		hashkey_laddr = entry->cfentry_laddr.addr6.s6_addr32[3];
	} else
#endif /* INET6 */
	{
		hashkey_faddr = entry->cfentry_faddr.addr46.ia46_addr4.s_addr;
		hashkey_laddr = entry->cfentry_laddr.addr46.ia46_addr4.s_addr;
	}
	entry->cfentry_flowhash = CFIL_HASH(hashkey_laddr, hashkey_faddr,
	    entry->cfentry_lport, entry->cfentry_fport);
	inp_hash_element = entry->cfentry_flowhash & db->cfdb_hashmask;

	cfilhash = &db->cfdb_hashbase[inp_hash_element];

	LIST_INSERT_HEAD(cfilhash, entry, cfentry_link);
	db->cfdb_count++;
	db->cfdb_only_entry = entry;
	cfil_hash_entry_log(LOG_DEBUG, db->cfdb_so, entry, 0, "CFIL: cfil_db_add_entry: ADDED");

done:
	CFIL_LOG(LOG_DEBUG, "CFIL: UDP <so %llx> total count %d", (uint64_t)VM_KERNEL_ADDRPERM(db->cfdb_so), db->cfdb_count);
	return entry;
}

struct cfil_info *
cfil_db_get_cfil_info(struct cfil_db *db, cfil_sock_id_t id)
{
	struct cfil_hash_entry *hash_entry = NULL;

	CFIL_LOG(LOG_INFO, "");

	if (db == NULL || id == 0) {
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP <so %llx> NULL DB <id %llu>",
		    (uint64_t)VM_KERNEL_ADDRPERM(db->cfdb_so), id);
		return NULL;
	}

	// This is an optimization for connected UDP socket which only has one flow.
	// No need to do the hash lookup.
	if (db->cfdb_count == 1) {
		if (db->cfdb_only_entry && db->cfdb_only_entry->cfentry_cfil &&
		    db->cfdb_only_entry->cfentry_cfil->cfi_sock_id == id) {
			return db->cfdb_only_entry->cfentry_cfil;
		}
	}

	hash_entry = cfil_db_lookup_entry_with_sockid(db, id);
	return hash_entry != NULL ? hash_entry->cfentry_cfil : NULL;
}

struct cfil_hash_entry *
cfil_sock_udp_get_flow(struct socket *so, uint32_t filter_control_unit, bool outgoing, struct sockaddr *local, struct sockaddr *remote)
{
#pragma unused(so, filter_control_unit, outgoing, local, remote)
	struct cfil_hash_entry *hash_entry = NULL;

	errno_t error = 0;
	socket_lock_assert_owned(so);

	// If new socket, allocate cfil db
	if (so->so_cfil_db == NULL) {
		if (cfil_db_init(so) != 0) {
			return NULL;
		}
	}

	// See if flow already exists.
	hash_entry = cfil_db_lookup_entry(so->so_cfil_db, local, remote);
	if (hash_entry != NULL) {
		return hash_entry;
	}

	hash_entry = cfil_db_add_entry(so->so_cfil_db, local, remote);
	if (hash_entry == NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_no_mem);
		CFIL_LOG(LOG_ERR, "CFIL: UDP failed to add entry");
		return NULL;
	}

	if (cfil_info_alloc(so, hash_entry) == NULL ||
	    hash_entry->cfentry_cfil == NULL) {
		cfil_db_delete_entry(so->so_cfil_db, hash_entry);
		CFIL_LOG(LOG_ERR, "CFIL: UDP failed to alloc cfil_info");
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_no_mem);
		return NULL;
	}

#if LIFECYCLE_DEBUG
	cfil_info_log(LOG_ERR, hash_entry->cfentry_cfil, "CFIL: LIFECYCLE: ADDED");
#endif

	if (cfil_info_attach_unit(so, filter_control_unit, hash_entry->cfentry_cfil) == 0) {
		cfil_info_free(hash_entry->cfentry_cfil);
		cfil_db_delete_entry(so->so_cfil_db, hash_entry);
		CFIL_LOG(LOG_ERR, "CFIL: UDP cfil_info_attach_unit(%u) failed",
		    filter_control_unit);
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_failed);
		return NULL;
	}
	CFIL_LOG(LOG_DEBUG, "CFIL: UDP <so %llx> filter_control_unit %u sockID %llu attached",
	    (uint64_t)VM_KERNEL_ADDRPERM(so),
	    filter_control_unit, hash_entry->cfentry_cfil->cfi_sock_id);

	so->so_flags |= SOF_CONTENT_FILTER;
	OSIncrementAtomic(&cfil_stats.cfs_sock_attached);

	/* Hold a reference on the socket for each flow */
	so->so_usecount++;

	error = cfil_dispatch_attach_event(so, hash_entry->cfentry_cfil, filter_control_unit);
	/* We can recover from flow control or out of memory errors */
	if (error != 0 && error != ENOBUFS && error != ENOMEM) {
		return NULL;
	}

	CFIL_INFO_VERIFY(hash_entry->cfentry_cfil);
	return hash_entry;
}

errno_t
cfil_sock_udp_handle_data(bool outgoing, struct socket *so,
    struct sockaddr *local, struct sockaddr *remote,
    struct mbuf *data, struct mbuf *control, uint32_t flags)
{
#pragma unused(outgoing, so, local, remote, data, control, flags)
	errno_t error = 0;
	uint32_t filter_control_unit;
	struct cfil_hash_entry *hash_entry = NULL;
	struct cfil_info *cfil_info = NULL;

	socket_lock_assert_owned(so);

	if (cfil_active_count == 0) {
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP no active filter");
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_in_vain);
		return error;
	}

	filter_control_unit = necp_socket_get_content_filter_control_unit(so);
	if (filter_control_unit == 0) {
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP failed to get control unit");
		return error;
	}

	if ((filter_control_unit & NECP_MASK_USERSPACE_ONLY) != 0) {
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP user space only");
		OSIncrementAtomic(&cfil_stats.cfs_sock_userspace_only);
		return error;
	}

	hash_entry = cfil_sock_udp_get_flow(so, filter_control_unit, outgoing, local, remote);
	if (hash_entry == NULL || hash_entry->cfentry_cfil == NULL) {
		CFIL_LOG(LOG_ERR, "CFIL: Falied to create UDP flow");
		return EPIPE;
	}
	// Update last used timestamp, this is for flow Idle TO
	hash_entry->cfentry_lastused = net_uptime();
	cfil_info = hash_entry->cfentry_cfil;

	if (cfil_info->cfi_flags & CFIF_DROP) {
#if DATA_DEBUG
		cfil_hash_entry_log(LOG_DEBUG, so, hash_entry, 0, "CFIL: UDP DROP");
#endif
		return EPIPE;
	}
	if (control != NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_data_in_control);
	}
	if (data->m_type == MT_OOBDATA) {
		CFIL_LOG(LOG_ERR, "so %llx MSG_OOB",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_in_oob);
	}

	error = cfil_data_common(so, cfil_info, outgoing, remote, data, control, flags);

	return error;
}

/*
 * Go through all UDP flows for specified socket and returns TRUE if
 * any flow is still attached.  If need_wait is TRUE, wait on first
 * attached flow.
 */
static int
cfil_filters_udp_attached(struct socket *so, bool need_wait)
{
	struct timespec ts;
	lck_mtx_t *mutex_held;
	struct cfilhashhead *cfilhash = NULL;
	struct cfil_db *db = NULL;
	struct cfil_hash_entry *hash_entry = NULL;
	struct cfil_hash_entry *temp_hash_entry = NULL;
	struct cfil_info *cfil_info = NULL;
	struct cfil_entry *entry = NULL;
	errno_t error = 0;
	int kcunit;
	int attached = 0;
	uint64_t sock_flow_id = 0;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil_db != NULL) {
		if (so->so_proto->pr_getlock != NULL) {
			mutex_held = (*so->so_proto->pr_getlock)(so, PR_F_WILLUNLOCK);
		} else {
			mutex_held = so->so_proto->pr_domain->dom_mtx;
		}
		LCK_MTX_ASSERT(mutex_held, LCK_MTX_ASSERT_OWNED);

		db = so->so_cfil_db;

		for (int i = 0; i < CFILHASHSIZE; i++) {
			cfilhash = &db->cfdb_hashbase[i];

			LIST_FOREACH_SAFE(hash_entry, cfilhash, cfentry_link, temp_hash_entry) {
				if (hash_entry->cfentry_cfil != NULL) {
					cfil_info = hash_entry->cfentry_cfil;
					for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
						entry = &cfil_info->cfi_entries[kcunit - 1];

						/* Are we attached to the filter? */
						if (entry->cfe_filter == NULL) {
							continue;
						}

						if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
							continue;
						}
						if ((entry->cfe_flags & CFEF_CFIL_DETACHED) != 0) {
							continue;
						}

						attached = 1;

						if (need_wait == TRUE) {
#if LIFECYCLE_DEBUG
							cfil_info_log(LOG_ERR, cfil_info, "CFIL: LIFECYCLE: WAIT FOR FLOW TO FINISH");
#endif

							ts.tv_sec = cfil_close_wait_timeout / 1000;
							ts.tv_nsec = (cfil_close_wait_timeout % 1000) *
							    NSEC_PER_USEC * 1000;

							OSIncrementAtomic(&cfil_stats.cfs_close_wait);
							cfil_info->cfi_flags |= CFIF_CLOSE_WAIT;
							sock_flow_id = cfil_info->cfi_sock_id;

							error = msleep((caddr_t)cfil_info, mutex_held,
							    PSOCK | PCATCH, "cfil_filters_udp_attached", &ts);

							// Woke up from sleep, validate if cfil_info is still valid
							if (so->so_cfil_db == NULL ||
							    (cfil_info != cfil_db_get_cfil_info(so->so_cfil_db, sock_flow_id))) {
								// cfil_info is not valid, do not continue
								goto done;
							}

							cfil_info->cfi_flags &= ~CFIF_CLOSE_WAIT;

#if LIFECYCLE_DEBUG
							cfil_info_log(LOG_ERR, cfil_info, "CFIL: LIFECYCLE: WAIT FOR FLOW DONE");
#endif

							/*
							 * Force close in case of timeout
							 */
							if (error != 0) {
								OSIncrementAtomic(&cfil_stats.cfs_close_wait_timeout);
#if LIFECYCLE_DEBUG
								cfil_info_log(LOG_ERR, cfil_info, "CFIL: LIFECYCLE: WAIT FOR FLOW TIMED OUT, FORCE DETACH");
#endif
								entry->cfe_flags |= CFEF_CFIL_DETACHED;
							}
						}
						goto done;
					}
				}
			}
		}
	}

done:
	return attached;
}

int32_t
cfil_sock_udp_data_pending(struct sockbuf *sb, bool check_thread)
{
	struct socket *so = sb->sb_so;
	struct cfi_buf *cfi_buf;
	uint64_t pending = 0;
	uint64_t total_pending = 0;
	struct cfilhashhead *cfilhash = NULL;
	struct cfil_db *db = NULL;
	struct cfil_hash_entry *hash_entry = NULL;
	struct cfil_hash_entry *temp_hash_entry = NULL;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil_db != NULL &&
	    (check_thread == FALSE || so->so_snd.sb_cfil_thread != current_thread())) {
		db = so->so_cfil_db;

		for (int i = 0; i < CFILHASHSIZE; i++) {
			cfilhash = &db->cfdb_hashbase[i];

			LIST_FOREACH_SAFE(hash_entry, cfilhash, cfentry_link, temp_hash_entry) {
				if (hash_entry->cfentry_cfil != NULL) {
					if ((sb->sb_flags & SB_RECV) == 0) {
						cfi_buf = &hash_entry->cfentry_cfil->cfi_snd;
					} else {
						cfi_buf = &hash_entry->cfentry_cfil->cfi_rcv;
					}

					pending = cfi_buf->cfi_pending_last - cfi_buf->cfi_pending_first;
					/*
					 * If we are limited by the "chars of mbufs used" roughly
					 * adjust so we won't overcommit
					 */
					if ((uint64_t)cfi_buf->cfi_pending_mbcnt > pending) {
						pending = cfi_buf->cfi_pending_mbcnt;
					}

					total_pending += pending;
				}
			}
		}

		VERIFY(total_pending < INT32_MAX);
#if DATA_DEBUG
		CFIL_LOG(LOG_DEBUG, "CFIL: <so %llx> total pending %llu <check_thread %d>",
		    (uint64_t)VM_KERNEL_ADDRPERM(so),
		    total_pending, check_thread);
#endif
	}

	return (int32_t)(total_pending);
}

int
cfil_sock_udp_notify_shutdown(struct socket *so, int how, int drop_flag, int shut_flag)
{
	struct cfil_info *cfil_info = NULL;
	struct cfilhashhead *cfilhash = NULL;
	struct cfil_db *db = NULL;
	struct cfil_hash_entry *hash_entry = NULL;
	struct cfil_hash_entry *temp_hash_entry = NULL;
	errno_t error = 0;
	int done_count = 0;
	int kcunit;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil_db != NULL) {
		db = so->so_cfil_db;

		for (int i = 0; i < CFILHASHSIZE; i++) {
			cfilhash = &db->cfdb_hashbase[i];

			LIST_FOREACH_SAFE(hash_entry, cfilhash, cfentry_link, temp_hash_entry) {
				if (hash_entry->cfentry_cfil != NULL) {
					cfil_info = hash_entry->cfentry_cfil;

					// This flow is marked as DROP
					if (cfil_info->cfi_flags & drop_flag) {
						done_count++;
						continue;
					}

					// This flow has been shut already, skip
					if (cfil_info->cfi_flags & shut_flag) {
						continue;
					}
					// Mark flow as shut
					cfil_info->cfi_flags |= shut_flag;
					done_count++;

					for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
						/* Disconnect incoming side */
						if (how != SHUT_WR) {
							error = cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 0);
						}
						/* Disconnect outgoing side */
						if (how != SHUT_RD) {
							error = cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 1);
						}
					}
				}
			}
		}
	}

	if (done_count == 0) {
		error = ENOTCONN;
	}
	return error;
}

int
cfil_sock_udp_shutdown(struct socket *so, int *how)
{
	int error = 0;

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || (so->so_cfil_db == NULL)) {
		goto done;
	}

	socket_lock_assert_owned(so);

	CFIL_LOG(LOG_INFO, "so %llx how %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), *how);

	/*
	 * Check the state of the socket before the content filter
	 */
	if (*how != SHUT_WR && (so->so_state & SS_CANTRCVMORE) != 0) {
		/* read already shut down */
		error = ENOTCONN;
		goto done;
	}
	if (*how != SHUT_RD && (so->so_state & SS_CANTSENDMORE) != 0) {
		/* write already shut down */
		error = ENOTCONN;
		goto done;
	}

	/*
	 * shutdown read: SHUT_RD or SHUT_RDWR
	 */
	if (*how != SHUT_WR) {
		error = cfil_sock_udp_notify_shutdown(so, SHUT_RD, CFIF_DROP, CFIF_SHUT_RD);
		if (error != 0) {
			goto done;
		}
	}
	/*
	 * shutdown write: SHUT_WR or SHUT_RDWR
	 */
	if (*how != SHUT_RD) {
		error = cfil_sock_udp_notify_shutdown(so, SHUT_WR, CFIF_DROP, CFIF_SHUT_WR);
		if (error != 0) {
			goto done;
		}

		/*
		 * When outgoing data is pending, we delay the shutdown at the
		 * protocol level until the content filters give the final
		 * verdict on the pending data.
		 */
		if (cfil_sock_data_pending(&so->so_snd) != 0) {
			/*
			 * When shutting down the read and write sides at once
			 * we can proceed to the final shutdown of the read
			 * side. Otherwise, we just return.
			 */
			if (*how == SHUT_WR) {
				error = EJUSTRETURN;
			} else if (*how == SHUT_RDWR) {
				*how = SHUT_RD;
			}
		}
	}
done:
	return error;
}

void
cfil_sock_udp_close_wait(struct socket *so)
{
	socket_lock_assert_owned(so);

	while (cfil_filters_udp_attached(so, FALSE)) {
		/*
		 * Notify the filters we are going away so they can detach
		 */
		cfil_sock_udp_notify_shutdown(so, SHUT_RDWR, 0, 0);

		/*
		 * Make sure we need to wait after the filter are notified
		 * of the disconnection
		 */
		if (cfil_filters_udp_attached(so, TRUE) == 0) {
			break;
		}
	}
}

void
cfil_sock_udp_is_closed(struct socket *so)
{
	struct cfil_info *cfil_info = NULL;
	struct cfilhashhead *cfilhash = NULL;
	struct cfil_db *db = NULL;
	struct cfil_hash_entry *hash_entry = NULL;
	struct cfil_hash_entry *temp_hash_entry = NULL;
	errno_t error = 0;
	int kcunit;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil_db != NULL) {
		db = so->so_cfil_db;

		for (int i = 0; i < CFILHASHSIZE; i++) {
			cfilhash = &db->cfdb_hashbase[i];

			LIST_FOREACH_SAFE(hash_entry, cfilhash, cfentry_link, temp_hash_entry) {
				if (hash_entry->cfentry_cfil != NULL) {
					cfil_info = hash_entry->cfentry_cfil;

					for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
						/* Let the filters know of the closing */
						error = cfil_dispatch_closed_event(so, cfil_info, kcunit);
					}

					/* Last chance to push passed data out */
					error = cfil_acquire_sockbuf(so, cfil_info, 1);
					if (error == 0) {
						cfil_service_inject_queue(so, cfil_info, 1);
					}
					cfil_release_sockbuf(so, 1);

					cfil_info->cfi_flags |= CFIF_SOCK_CLOSED;

					/* Pending data needs to go */
					cfil_flush_queues(so, cfil_info);

					CFIL_INFO_VERIFY(cfil_info);
				}
			}
		}
	}
}

void
cfil_sock_udp_buf_update(struct sockbuf *sb)
{
	struct cfil_info *cfil_info = NULL;
	struct cfilhashhead *cfilhash = NULL;
	struct cfil_db *db = NULL;
	struct cfil_hash_entry *hash_entry = NULL;
	struct cfil_hash_entry *temp_hash_entry = NULL;
	errno_t error = 0;
	int outgoing;
	struct socket *so = sb->sb_so;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil_db != NULL) {
		if (!cfil_sbtrim) {
			return;
		}

		db = so->so_cfil_db;

		for (int i = 0; i < CFILHASHSIZE; i++) {
			cfilhash = &db->cfdb_hashbase[i];

			LIST_FOREACH_SAFE(hash_entry, cfilhash, cfentry_link, temp_hash_entry) {
				if (hash_entry->cfentry_cfil != NULL) {
					cfil_info = hash_entry->cfentry_cfil;

					if ((sb->sb_flags & SB_RECV) == 0) {
						if ((cfil_info->cfi_flags & CFIF_RETRY_INJECT_OUT) == 0) {
							return;
						}
						outgoing = 1;
						OSIncrementAtomic(&cfil_stats.cfs_inject_q_out_retry);
					} else {
						if ((cfil_info->cfi_flags & CFIF_RETRY_INJECT_IN) == 0) {
							return;
						}
						outgoing = 0;
						OSIncrementAtomic(&cfil_stats.cfs_inject_q_in_retry);
					}

					CFIL_LOG(LOG_NOTICE, "so %llx outgoing %d",
					    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing);

					error = cfil_acquire_sockbuf(so, cfil_info, outgoing);
					if (error == 0) {
						cfil_service_inject_queue(so, cfil_info, outgoing);
					}
					cfil_release_sockbuf(so, outgoing);
				}
			}
		}
	}
}

void
cfil_filter_show(u_int32_t kcunit)
{
	struct content_filter *cfc = NULL;
	struct cfil_entry *entry;
	int count = 0;

	if (content_filters == NULL) {
		return;
	}
	if (kcunit > MAX_CONTENT_FILTER) {
		return;
	}

	cfil_rw_lock_shared(&cfil_lck_rw);

	if (content_filters[kcunit - 1] == NULL) {
		cfil_rw_unlock_shared(&cfil_lck_rw);
		return;
	}
	cfc = content_filters[kcunit - 1];

	CFIL_LOG(LOG_ERR, "CFIL: FILTER SHOW: Filter <unit %d, entry count %d> flags <%lx>:",
	    kcunit, cfc->cf_sock_count, (unsigned long)cfc->cf_flags);
	if (cfc->cf_flags & CFF_DETACHING) {
		CFIL_LOG(LOG_ERR, "CFIL: FILTER SHOW: - DETACHING");
	}
	if (cfc->cf_flags & CFF_ACTIVE) {
		CFIL_LOG(LOG_ERR, "CFIL: FILTER SHOW: - ACTIVE");
	}
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		CFIL_LOG(LOG_ERR, "CFIL: FILTER SHOW: - FLOW CONTROLLED");
	}

	TAILQ_FOREACH(entry, &cfc->cf_sock_entries, cfe_link) {
		if (entry->cfe_cfil_info && entry->cfe_cfil_info->cfi_so) {
			struct cfil_info *cfil_info = entry->cfe_cfil_info;

			count++;

			if (entry->cfe_flags & CFEF_CFIL_DETACHED) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: FILTER SHOW: - DETACHED");
			} else {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: FILTER SHOW: - ATTACHED");
			}
		}
	}

	CFIL_LOG(LOG_ERR, "CFIL: FILTER SHOW: Filter - total entries shown: %d", count);

	cfil_rw_unlock_shared(&cfil_lck_rw);
}

void
cfil_info_show(void)
{
	struct cfil_info *cfil_info;
	int count = 0;

	cfil_rw_lock_shared(&cfil_lck_rw);

	CFIL_LOG(LOG_ERR, "CFIL: INFO SHOW: count %d", cfil_sock_attached_count);

	TAILQ_FOREACH(cfil_info, &cfil_sock_head, cfi_link) {
		count++;

		cfil_info_log(LOG_ERR, cfil_info, "CFIL: INFO SHOW");

		if (cfil_info->cfi_flags & CFIF_DROP) {
			CFIL_LOG(LOG_ERR, "CFIL: INFO FLAG - DROP");
		}
		if (cfil_info->cfi_flags & CFIF_CLOSE_WAIT) {
			CFIL_LOG(LOG_ERR, "CFIL: INFO FLAG - CLOSE_WAIT");
		}
		if (cfil_info->cfi_flags & CFIF_SOCK_CLOSED) {
			CFIL_LOG(LOG_ERR, "CFIL: INFO FLAG - SOCK_CLOSED");
		}
		if (cfil_info->cfi_flags & CFIF_RETRY_INJECT_IN) {
			CFIL_LOG(LOG_ERR, "CFIL: INFO FLAG - RETRY_INJECT_IN");
		}
		if (cfil_info->cfi_flags & CFIF_RETRY_INJECT_OUT) {
			CFIL_LOG(LOG_ERR, "CFIL: INFO FLAG - RETRY_INJECT_OUT");
		}
		if (cfil_info->cfi_flags & CFIF_SHUT_WR) {
			CFIL_LOG(LOG_ERR, "CFIL: INFO FLAG - SHUT_WR");
		}
		if (cfil_info->cfi_flags & CFIF_SHUT_RD) {
			CFIL_LOG(LOG_ERR, "CFIL: INFO FLAG - SHUT_RD");
		}
	}

	CFIL_LOG(LOG_ERR, "CFIL: INFO SHOW: total cfil_info shown: %d", count);

	cfil_rw_unlock_shared(&cfil_lck_rw);
}

bool
cfil_info_idle_timed_out(struct cfil_info *cfil_info, int timeout, u_int32_t current_time)
{
	if (cfil_info && cfil_info->cfi_hash_entry &&
	    (current_time - cfil_info->cfi_hash_entry->cfentry_lastused >= (u_int32_t)timeout)) {
#if GC_DEBUG
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: flow IDLE timeout expired");
#endif
		return true;
	}
	return false;
}

bool
cfil_info_action_timed_out(struct cfil_info *cfil_info, int timeout)
{
	struct cfil_entry *entry;
	struct timeval current_tv;
	struct timeval diff_time;

	if (cfil_info == NULL) {
		return false;
	}

	/*
	 * If we have queued up more data than passed offset and we haven't received
	 * an action from user space for a while (the user space filter might have crashed),
	 * return action timed out.
	 */
	if (cfil_info->cfi_snd.cfi_pending_last > cfil_info->cfi_snd.cfi_pass_offset ||
	    cfil_info->cfi_rcv.cfi_pending_last > cfil_info->cfi_rcv.cfi_pass_offset) {
		microuptime(&current_tv);

		for (int kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			if (entry->cfe_filter == NULL) {
				continue;
			}

			if (cfil_info->cfi_snd.cfi_pending_last > entry->cfe_snd.cfe_pass_offset ||
			    cfil_info->cfi_rcv.cfi_pending_last > entry->cfe_rcv.cfe_pass_offset) {
				// haven't gotten an action from this filter, check timeout
				timersub(&current_tv, &entry->cfe_last_action, &diff_time);
				if (diff_time.tv_sec >= timeout) {
#if GC_DEBUG
					cfil_info_log(LOG_ERR, cfil_info, "CFIL: flow ACTION timeout expired");
#endif
					return true;
				}
			}
		}
	}
	return false;
}

bool
cfil_info_buffer_threshold_exceeded(struct cfil_info *cfil_info)
{
	if (cfil_info == NULL) {
		return false;
	}

	/*
	 * Clean up flow if it exceeded queue thresholds
	 */
	if (cfil_info->cfi_snd.cfi_tail_drop_cnt ||
	    cfil_info->cfi_rcv.cfi_tail_drop_cnt) {
#if GC_DEBUG
		CFIL_LOG(LOG_ERR, "CFIL: queue threshold exceeded: mbuf max <count: %d bytes: %d> tail drop count <OUT: %d IN: %d>",
		    cfil_udp_gc_mbuf_num_max,
		    cfil_udp_gc_mbuf_cnt_max,
		    cfil_info->cfi_snd.cfi_tail_drop_cnt,
		    cfil_info->cfi_rcv.cfi_tail_drop_cnt);
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: queue threshold exceeded");
#endif
		return true;
	}

	return false;
}

static void
cfil_udp_gc_thread_sleep(bool forever)
{
	if (forever) {
		(void) assert_wait((event_t) &cfil_sock_udp_attached_count,
		    THREAD_INTERRUPTIBLE);
	} else {
		uint64_t deadline = 0;
		nanoseconds_to_absolutetime(UDP_FLOW_GC_RUN_INTERVAL_NSEC, &deadline);
		clock_absolutetime_interval_to_deadline(deadline, &deadline);

		(void) assert_wait_deadline(&cfil_sock_udp_attached_count,
		    THREAD_INTERRUPTIBLE, deadline);
	}
}

static void
cfil_udp_gc_thread_func(void *v, wait_result_t w)
{
#pragma unused(v, w)

	ASSERT(cfil_udp_gc_thread == current_thread());
	thread_set_thread_name(current_thread(), "CFIL_UPD_GC");

	// Kick off gc shortly
	cfil_udp_gc_thread_sleep(false);
	thread_block_parameter((thread_continue_t) cfil_info_udp_expire, NULL);
	/* NOTREACHED */
}

static void
cfil_info_udp_expire(void *v, wait_result_t w)
{
#pragma unused(v, w)

	static uint64_t expired_array[UDP_FLOW_GC_MAX_COUNT];
	static uint32_t expired_count = 0;

	struct cfil_info *cfil_info;
	struct cfil_hash_entry *hash_entry;
	struct cfil_db *db;
	struct socket *so;
	u_int32_t current_time = 0;

	current_time = net_uptime();

	// Get all expired UDP flow ids
	cfil_rw_lock_shared(&cfil_lck_rw);

	if (cfil_sock_udp_attached_count == 0) {
		cfil_rw_unlock_shared(&cfil_lck_rw);
		goto go_sleep;
	}

	TAILQ_FOREACH(cfil_info, &cfil_sock_head, cfi_link) {
		if (expired_count >= UDP_FLOW_GC_MAX_COUNT) {
			break;
		}

		if (IS_UDP(cfil_info->cfi_so)) {
			if (cfil_info_idle_timed_out(cfil_info, UDP_FLOW_GC_IDLE_TO, current_time) ||
			    cfil_info_action_timed_out(cfil_info, UDP_FLOW_GC_ACTION_TO) ||
			    cfil_info_buffer_threshold_exceeded(cfil_info)) {
				expired_array[expired_count] = cfil_info->cfi_sock_id;
				expired_count++;
			}
		}
	}
	cfil_rw_unlock_shared(&cfil_lck_rw);

	if (expired_count == 0) {
		goto go_sleep;
	}

	for (uint32_t i = 0; i < expired_count; i++) {
		// Search for socket (UDP only and lock so)
		so = cfil_socket_from_sock_id(expired_array[i], true);
		if (so == NULL) {
			continue;
		}

		cfil_info = cfil_db_get_cfil_info(so->so_cfil_db, expired_array[i]);
		if (cfil_info == NULL) {
			goto unlock;
		}

		db = so->so_cfil_db;
		hash_entry = cfil_info->cfi_hash_entry;

		if (db == NULL || hash_entry == NULL) {
			goto unlock;
		}

#if GC_DEBUG || LIFECYCLE_DEBUG
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: LIFECYCLE: GC CLEAN UP");
#endif

		cfil_db_delete_entry(db, hash_entry);
		cfil_info_free(cfil_info);
		OSIncrementAtomic(&cfil_stats.cfs_sock_detached);

		if (so->so_flags & SOF_CONTENT_FILTER) {
			if (db->cfdb_count == 0) {
				so->so_flags &= ~SOF_CONTENT_FILTER;
			}
			VERIFY(so->so_usecount > 0);
			so->so_usecount--;
		}
unlock:
		socket_unlock(so, 1);
	}

#if GC_DEBUG
	CFIL_LOG(LOG_ERR, "CFIL: UDP flow idle timeout check: expired %d idle flows", expired_count);
#endif
	expired_count = 0;

go_sleep:

	// Sleep forever (until waken up) if no more UDP flow to clean
	cfil_rw_lock_shared(&cfil_lck_rw);
	cfil_udp_gc_thread_sleep(cfil_sock_udp_attached_count == 0 ? true : false);
	cfil_rw_unlock_shared(&cfil_lck_rw);
	thread_block_parameter((thread_continue_t)cfil_info_udp_expire, NULL);
	/* NOTREACHED */
}

struct m_tag *
cfil_udp_save_socket_state(struct cfil_info *cfil_info, struct mbuf *m)
{
	struct m_tag *tag = NULL;
	struct cfil_tag *ctag = NULL;
	struct cfil_hash_entry *hash_entry = NULL;

	if (cfil_info == NULL || cfil_info->cfi_so == NULL ||
	    cfil_info->cfi_hash_entry == NULL || m == NULL || !(m->m_flags & M_PKTHDR)) {
		return NULL;
	}

	/* Allocate a tag */
	tag = m_tag_create(KERNEL_MODULE_TAG_ID, KERNEL_TAG_TYPE_CFIL_UDP,
	    sizeof(struct cfil_tag), M_DONTWAIT, m);

	if (tag) {
		ctag = (struct cfil_tag*)(tag + 1);
		ctag->cfil_so_state_change_cnt = cfil_info->cfi_so->so_state_change_cnt;
		ctag->cfil_so_options = cfil_info->cfi_so->so_options;

		hash_entry = cfil_info->cfi_hash_entry;
		if (hash_entry->cfentry_family == AF_INET6) {
			fill_ip6_sockaddr_4_6(&ctag->cfil_faddr,
			    &hash_entry->cfentry_faddr.addr6,
			    hash_entry->cfentry_fport);
		} else if (hash_entry->cfentry_family == AF_INET) {
			fill_ip_sockaddr_4_6(&ctag->cfil_faddr,
			    hash_entry->cfentry_faddr.addr46.ia46_addr4,
			    hash_entry->cfentry_fport);
		}
		m_tag_prepend(m, tag);
		return tag;
	}
	return NULL;
}

struct m_tag *
cfil_udp_get_socket_state(struct mbuf *m, uint32_t *state_change_cnt, short *options,
    struct sockaddr **faddr)
{
	struct m_tag *tag = NULL;
	struct cfil_tag *ctag = NULL;

	tag = m_tag_locate(m, KERNEL_MODULE_TAG_ID, KERNEL_TAG_TYPE_CFIL_UDP, NULL);
	if (tag) {
		ctag = (struct cfil_tag *)(tag + 1);
		if (state_change_cnt) {
			*state_change_cnt = ctag->cfil_so_state_change_cnt;
		}
		if (options) {
			*options = ctag->cfil_so_options;
		}
		if (faddr) {
			*faddr = (struct sockaddr *) &ctag->cfil_faddr;
		}

		/*
		 * Unlink tag and hand it over to caller.
		 * Note that caller will be responsible to free it.
		 */
		m_tag_unlink(m, tag);
		return tag;
	}
	return NULL;
}
