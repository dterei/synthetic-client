// Server connection handling
#ifndef _CONNECTIONS_H
#define _CONNECTIONS_H

#include "fsm.h"
#include "items.h"
#include "stats.h"

#include <event.h>
#include <stdbool.h>

// size of initial read/write buffers for request/responses.
#define DATA_BUFFER_SIZE 2048
// Initial size of the sendmsg() scatter/gather array.
#define IOV_LIST_INITIAL 400
// Initial number of sendmsg() argument structures to allocate.
#define MSG_LIST_INITIAL 10
// Initial size of list of items being returned by "get".
#define ITEM_LIST_INITIAL 200
// Initial TCP packet max size (for handling MTU gracefully).
#define MAX_PAYLOAD_SIZE 1400
// Initial queue size for memcached rpc's
#define MEMCACHE_RPC_QUEUE 2048

// connection type.
enum conn_type {
	client_conn,
	memcached_conn
};

// Representation of a connection. Used for both client connections and our
// listening socket.
typedef struct _conn {
	enum conn_type type;             // type of connection (for casting).
	struct _conn *next;              // allow link-listing of connections.
	int client_id;                   // the client id connecting.

	// socket handling.
	int sfd;                         // underlying socket.
	struct event event;
	short ev_flags;
	short old_ev_flags;              // used for save ev_flags over timeout.
	struct _worker_thread_t *thread; // thread managing this connection.

	// state handling.
	enum conn_states state;          // fsm state.
	enum conn_states after_write;    // fsm state to transition to after writing wbuf.
	enum conn_states after_timeout;  // fsm state to transition to after timeout.
	short which;                     // which event generate the event.
	int cmd;                         // which memcached cmd are we processing.
	suseconds_t timeout;             // amount to delay connection for (microseconds).

	// rbuf -- read input buffer (only one).
	// [..........|...........^...................]
	// ^          ^                               ^
	// rbuf       rcurr       rcurr+rbytes        rbuf+rsize
	//
	int  rsize;                      // size of rbuf.
	char *rbuf;                      // buffer to read commands into.
	char *rcurr;                     // pointer into rbuf to end of parsed data.
	int  rbytes;                     // how much data, starting from rcur, do we have unparsed.

	// wbuf -- only used for error_response.
	// [..........|...........^...................]
	// ^          ^                               ^
	// wbuf       wcurr       wcurr+wbytes        wbuf+wsize
	//
	int  wsize;                      // size of wbuf.
	char *wbuf;                      // buffer for data to write to network.
	char *wcurr;                     // pointer into wbuf to end of written data.
	int  wbytes;                     // remaining data in wbuf not yet written.

	int sbytes;                      // bytes to swallow of the wire.

	// msghdr & iovec are used for vectored output. (flat arrays).
	struct msghdr *msglist;
	int    msgsize;                  // number of elements allocated in msglist[].
	int    msgused;                  // number of elements used in msglist[].
	int    msgcurr;                  // element in msglist[] being transmitted now.
	int    msgbytes;                 // number of bytes in current msg.

	struct iovec *iov;
	int    iovsize;                  // number of elements allocated in iov[].
	int    iovused;                  // number of elements used in iov[].

	// item reference counting.
	// The relation between an item and it's msghdr/iovec structure is not
	// recorded. So to be safe we can only release items when we clear out all
	// pending output data.
	item   **ilist;                  // list of items we currently have retained
	                                 // owned. Done for ref-counting purposes
												// while we write out data associated with
												// them.
	int    isize;                    // number of elements in ilist.
	item   **icurr;                  // current free slot in ilist.
	int    ileft;                    // space left in ilist for allocaiton.

	// how many memcached rpc's a client_conn is waiting on.
	int          rpcwaiting;

	// memcached backend rpcs. (used only by memcached_conn types).
	// [........|..........|...........]
	// ^        ^          ^           ^
	// rpc      rpcdone    rpcused     rpcsize
 	struct _conn **rpc;              // inflight RPC's for connections (ordered queue).
	int          rpcsize;            // number of elements allocated in rpc[].
 	int          rpcused;            // number of elements used in rpc[].
	int          rpcdone;            // last+1 element in rpc[] that we've
												// received response for.
	
	client_stats *stats;             // client specific stats.
	void *mem_blob;                  // some random allocated memory for synthetic testing.
	int   mem_free_delay;            // number of requests to wait before free'ing mem_blob.

	unsigned short refcnt_conn;
	unsigned short refcnt_rbuf;
	unsigned short refcnt_wbuf;
	unsigned short refcnt_iov;
	unsigned short refcnt_msg;
	unsigned short refcnt_ilist;
	unsigned short refcnt_rpc;
	unsigned short refcnt_blob;
} conn;

// new connection management.
void conn_init(void);
conn *conn_new(enum conn_type type,
					const int client_id,
               const int sfd,
               enum conn_states init_state,
               const int event_flags,
               const int read_buffer_size,
               struct event_base *base);
void conn_close(conn *c);

// in-flight connection management.
void conn_set_state(conn *c, enum conn_states state);
int conn_update_event(conn *c, const int new_flags);
int conn_update_event_t(conn *c, const int new_flags, struct timeval *t);
void conn_shrink(conn *c);
int conn_add_msghdr(conn *c);
bool conn_add_iov(conn *c, const void *buf, int len);
bool conn_expand_items(conn *c);
bool conn_ensure_rpc_space(conn *c);

#endif

