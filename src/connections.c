// Server connection handling

#include "connections.h"
#include "items.h"
#include "locking.h"
#include "server.h"

#include <assert.h>
#include <event.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <ctype.h>
#include <stdarg.h>

#include <memory>

// need this to get IOV_MAX on some platforms.
#ifndef __need_IOV_MAX
#define __need_IOV_MAX
#endif
#include <limits.h>

#define GC_THREADS
#include <gc.h>
#define GC_CALLOC(m,n) GC_MALLOC((m)*(n))

// prototypes.
static bool conn_add_to_freelist(conn *c);
static void conn_free(conn *c);

// Free list management for connections.
static conn **freeconns;
static int freetotal;
static int freecurr;
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

// live connections list for GC to trace.
static conn *liveconns;

// Initialize our connection system (i.e., free list).
void conn_init(void) {
	liveconns = NULL;
	freetotal = 200;
	freecurr = 0;
	if ((freeconns = (conn **) GC_CALLOC(freetotal, sizeof(conn *))) == NULL) {
		fprintf(stderr, "Failed to allocate connection structures\n");
	}
	return;
}

// Returns a connection from the freelist, if any.
static conn *conn_from_freelist() {
	conn *c = NULL;
	/* pthread_mutex_lock(&conn_lock); */
	/* if (freecurr > 0) { */
	/* 	c = freeconns[--freecurr]; */
	/* } */
	/* pthread_mutex_unlock(&conn_lock); */
	return c;
}

// Adds a connection to the freelist. 0 = success.
static bool conn_add_to_freelist(conn *c) {
    int ret = false;
    pthread_mutex_lock(&conn_lock);
    if (freecurr < freetotal) {
        freeconns[freecurr++] = c;
        ret = true;
    } else {
        /* try to enlarge free connections array */
        size_t newsize = freetotal * 2;
        conn **new_freeconns = (conn **) GC_REALLOC(freeconns, sizeof(conn *) * newsize);
        if (new_freeconns) {
            freetotal = newsize;
            freeconns = new_freeconns;
            freeconns[freecurr++] = c;
            ret = true;
        }
    }
    pthread_mutex_unlock(&conn_lock);
    return ret;
}

// Create a new connection value.
conn *conn_new(enum conn_type type,
					const int client_id,
               const int sfd,
               enum conn_states init_state,
               const int event_flags,
               const int read_buffer_size,
               struct event_base *base) {
	conn *c = conn_from_freelist();

	if (NULL == c) {
		if (!(c = (conn *)GC_CALLOC(1, sizeof(conn)))) {
			fprintf(stderr, "calloc()\n");
			return NULL;
		}

		c->rsize = read_buffer_size;
		c->wsize = DATA_BUFFER_SIZE;
		c->rbuf = (char *)GC_MALLOC_ATOMIC((size_t)c->rsize);
		c->wbuf = (char *)GC_MALLOC_ATOMIC((size_t)c->wsize);

		c->iovsize = IOV_LIST_INITIAL;
		c->iov = (struct iovec *)GC_MALLOC_ATOMIC(sizeof(struct iovec) * c->iovsize);

		c->msgsize = MSG_LIST_INITIAL;
		c->msglist = (struct msghdr *)GC_MALLOC_ATOMIC(sizeof(struct msghdr) * c->msgsize);

		c->isize = ITEM_LIST_INITIAL;
		c->ilist = (item **)GC_MALLOC_ATOMIC(sizeof(item *) * c->isize);

		c->rpcsize = MEMCACHE_RPC_QUEUE;
		c->rpc = (conn **)GC_MALLOC(sizeof(conn *) * c->rpcsize);

		if (c->rbuf == NULL || c->wbuf == NULL ||  c->iov == NULL
		    || c->msglist == NULL || c->ilist == NULL) {
			conn_free(c);
			fprintf(stderr, "malloc()\n");
			return NULL;
		}
	}

	/* void conn_finalizer(void *obj, void *arg) { */
	/* 	fprintf(stderr, "Connection free'd! %p\n", obj); */
	/* } */
	/* GC_REGISTER_FINALIZER(c, conn_finalizer, NULL, NULL, NULL); */
	pthread_mutex_lock(&conn_lock);
	c->prev = NULL;
	c->next = liveconns;
	if (liveconns != NULL) liveconns->prev = c;
	liveconns = c;
	pthread_mutex_unlock(&conn_lock);

	c->client_id = client_id;
	c->type = type;
	c->sfd = sfd;
	c->state = init_state;
	c->after_write = init_state;
	c->rbytes = c->wbytes = 0;
	c->rcurr = c->rbuf;
	c->wcurr = c->wbuf;
	c->iovused = 0;
	c->msgcurr = 0;
	c->msgused = 0;
	c->ileft = 0;
	c->icurr = c->ilist;
	c->rpcused = 0;
	c->rpcdone = 0;
	c->rpcwaiting = 0;

	std::shared_ptr<client_stats> cs = get_client_stats(config.stats, c->client_id);
	mutex_lock(&cs->lock);
	cs->total_connections++;
	cs->live_connections++;
	mutex_unlock(&cs->lock);
	c->stats = cs;

	event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
	event_base_set(base, &c->event);
	c->ev_flags = event_flags;

	if (event_add(&c->event, 0) == -1) {
		if (!conn_add_to_freelist(c)) {
			conn_free(c);
		}
		perror("event_add");
		return NULL;
	}

	return c;
}

// Frees a connection.
static void conn_free(conn *c) {
	conn *prev = c->prev;
	conn *next = c->next;

	if (prev != NULL) prev->next = next;
	if (next != NULL) next->prev = prev;
	c->next = NULL;
	c->prev = NULL;
	if (liveconns == c) liveconns = NULL;

	/* if (c) { */
	/* 	if (c->rbuf) free(c->rbuf); */
	/* 	if (c->wbuf) free(c->wbuf); */
	/* 	if (c->iov)  free(c->iov); */
	/* 	if (c->msglist) free(c->msglist); */
	/* 	if (c->ilist) free(c->ilist); */
	/* 	if (c->rpc) free(c->rpc); */
	/* 	free(c); */
	/* } */
}

// Close a connection.
void conn_close(conn *c) {
	assert(c != NULL);

	/* delete the event, the socket and the conn */
	event_del(&c->event);
	if (config.verbose > 1) {
		fprintf(stderr, "<%d connection closed.\n", c->sfd);
	}
	close(c->sfd);
	c->stats = NULL;
	if (c->mem_blob != NULL) {
		/* free(c->mem_blob); */
		c->mem_blob = NULL;
	}

	/* if the connection has big buffers, just free it */
	if (!conn_add_to_freelist(c)) {
		conn_free(c);
	}
}

// Sets a connection's current state in the state machine. Any special
// processing that needs to happen on certain state transitions can happen
// here.
void conn_set_state(conn *c, enum conn_states state) {
	assert(c != NULL);
	assert(state > conn_min_state && state < conn_max_state);

	if (state != c->state) {
		if (config.verbose > 2) {
			fprintf(stderr, "%d: going from %s to %s\n",
			        c->sfd, state_text(c->state), state_text(state));
		}
		c->state = state;
	}
}

// update the event type for connection that we are listening on.
int conn_update_event(conn *c, const int new_flags) {
	return conn_update_event_t(c, new_flags, NULL);
}

// update the event type for connection that we are listening on (with timeout).
int conn_update_event_t(conn *c, const int new_flags, struct timeval *t) {
	assert(c != NULL);
	struct event_base *base = c->event.ev_base;

	if (c->ev_flags == new_flags) return 1;

	if (event_del(&c->event) == -1) {
		if (config.verbose > 0) {
			fprintf(stderr, "Couldn't update event\n");
		}
		return 0;
	}

	event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
	event_base_set(base, &c->event);
	c->ev_flags = new_flags;

	if (event_add(&c->event, t) == -1) {
		if (config.verbose > 0) {
			fprintf(stderr, "Couldn't update event\n");
		}
		return 0;
	}
	return 1;
}

// Shrinks a connection's buffers if they're too big.  This prevents periodic
// large "get" requests from permanently chewing lots of server memory.
//
// This should only be called in between requests since it can wipe output
// buffers!
void conn_shrink(conn *c) {
	assert(c != NULL);
}

// Adds a message header to a connection. Returns 1 on success, 0 on
// out-of-memory.
int conn_add_msghdr(conn *c) {
	struct msghdr *msg;

	assert(c != NULL);

	if (c->msgsize == c->msgused) {
		msg = (struct msghdr *) GC_REALLOC(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
		if (!msg) return 0;
		c->msglist = msg;
		c->msgsize *= 2;
	}

	msg = c->msglist + c->msgused;
	// this wipes msg_iovlen, msg_control, msg_controllen, and msg_flags, the
	// last 3 of which aren't defined on solaris:
	memset(msg, 0, sizeof(struct msghdr));
	msg->msg_iov = &c->iov[c->iovused];
	c->msgbytes = 0;
	c->msgused++;

	return 1;
}

// Ensures that there is room for another struct iovec in a connection's
// iov list.
// @return true on success, false on out-of-memory.
static bool conn_ensure_iov_space(conn *c) {
	assert(c != NULL);

	if (c->iovused >= c->iovsize) {
		int i, iovnum;
		struct iovec *new_iov =
			(struct iovec *)GC_REALLOC(c->iov, (c->iovsize * 2) * sizeof(struct iovec));
		if (!new_iov) return false;
		c->iov = new_iov;
		c->iovsize *= 2;

		// Point all the msghdr structures at the new list.
		for (i = 0, iovnum = 0; i < c->msgused; i++) {
			c->msglist[i].msg_iov = &c->iov[iovnum];
			iovnum += c->msglist[i].msg_iovlen;
		}
	}

	return true;
}


// Adds data to the list of pending data that will be written out to a
// connection.
//
// @return true on success, false on out-of-memory.
bool conn_add_iov(conn *c, const void *buf, int len) {
	assert(c != NULL);
	assert(c->msglist != NULL);

	int leftover;
	do {
		struct msghdr *m = &c->msglist[c->msgused - 1];

		// Limit the first payloads of TCP replies, to MAX_PAYLOAD_SIZE bytes.
		bool limit_to_mtu = 1 == c->msgused;

		// We may need to start a new msghdr if this one is full.
		if (m->msg_iovlen == IOV_MAX ||
		    (limit_to_mtu && c->msgbytes >= MAX_PAYLOAD_SIZE)) {
			conn_add_msghdr(c);
			m = &c->msglist[c->msgused - 1];
		}

		if (!conn_ensure_iov_space(c)) return false;

		// If the fragment is too big to fit in the datagram, split it up.
		if (limit_to_mtu && len + c->msgbytes > MAX_PAYLOAD_SIZE) {
			leftover = len + c->msgbytes - MAX_PAYLOAD_SIZE;
			len -= leftover;
		} else {
			leftover = 0;
		}

		assert(m != NULL);
		assert(m->msg_iov != NULL);
		m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
		m->msg_iov[m->msg_iovlen].iov_len = len;

		c->msgbytes += len;
		c->iovused++;
		m->msg_iovlen++;

		buf = ((char *)buf) + len;
		len = leftover;
	} while (leftover > 0);

	return true;
}

// grow the item list of a connection.
bool conn_expand_items(conn *c) {
	item **new_list = (item **) GC_REALLOC(c->ilist, sizeof(item *) * c->isize * 2);
	if (new_list) {
		c->isize *= 2;
		c->ilist = new_list;
		return true;
	} else {
		return false;
	}
}

// grow the rpc queue if needed.
bool conn_ensure_rpc_space(conn *mc) {
	assert(mc->type == memcached_conn);

	conn **rpc;
	if (mc->rpcused >= mc->rpcsize) {
		if (mc->rpcdone == 0) {
			// expand queue as out of slots.
			rpc = (conn **) GC_REALLOC(mc->rpc, sizeof(conn **) * mc->rpcsize * 2);
			if (!rpc) return false;
			mc->rpc = rpc;
			mc->rpcsize *= 2;
		} else {
			// try remove dead (done) rpc items for space.
			int used = mc->rpcused - mc->rpcdone;
			memmove(mc->rpc, mc->rpc + mc->rpcdone, used);
			mc->rpcused = used;
			mc->rpcdone = 0;
		}
	}
	return true;
}

