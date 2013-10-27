// Memcached fake, benchmarking server.
//
// This implements the architecture of memcached, so a fairly high-performance
// network service for request/response style workloads. Multi-threaded with
// whole connections as the unit of distribution for load-balancing.
// Connections are round-robbin'd to threads and once assigned, are stuck.
//
// No load-balancing or QoS after that.
//
// Code is largely taken from memcached itself but cleaned up a lot.
//
// Implemented commands:
// * GET -- just respond with a fixed value to every single key.
//
#include "commands.h"
#include "connections.h"
#include "fsm.h"
#include "items.h"
#include "protocol.h"
#include "server.h"
#include "settings.h"
#include "threads.h"

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#define GC_THREADS
#include <gc.h>
#define GC_CALLOC(m,n) GC_MALLOC((m)*(n))

#define GC_INTERVAL 10

#define REQ_PER_EVENT 20

// possible results reading from network.
typedef enum {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,
    READ_MEMORY_ERROR // out-of-memory
} read_result;

// possible results writing to network.
typedef enum {
	TRANSMIT_COMPLETE,   // All done writing.
	TRANSMIT_INCOMPLETE, // More data remaining to write.
	TRANSMIT_SOFT_ERROR, // Can't write any more right now.
	TRANSMIT_HARD_ERROR  // Can't write (c->state is set to conn_closing).
} write_result;

// prototypes
static int server_socket(int port);
static void drive_machine(conn *c);
static void reset_cmd_handler(conn *c);
static read_result read_network(conn *c);
static bool read_command(conn *c);
static void process_command(conn *c, char *command);
static write_result transmit(conn *c);
static void time_loop(const int fd, const short which, void *arg);

// globals
settings config;
static conn *listen_conn = NULL;
static struct event_base *main_base;

// SIGINT signal handler.
static void sig_handler(const int sig) {
    printf("SIGINT handled.\n");
    exit(EXIT_SUCCESS);
}

// launch the server.
int main (int argc, char **argv) {
   int retval = EXIT_SUCCESS;

	signal(SIGINT, sig_handler);
	setbuf(stderr, NULL);

	// ignore SIGPIPE signals; we can use errno == EPIPE if we
	// need that information
	if (sigignore(SIGPIPE) == -1) {
		perror("failed to ignore SIGPIPE; sigaction");
		exit(EX_OSERR);
	}

	// init GC
	GC_INIT();
	fprintf(stderr, "GC Enabled: %d\n", !GC_is_disabled());
	fprintf(stderr, "Parallel GC: %d\n", GC_get_parallel());
	fprintf(stderr, "Minor freq: %d\n", GC_get_full_freq());
	fprintf(stderr, "Free space divider: %ld\n", GC_get_free_space_divisor());
	fprintf(stderr, "Pause time bound: %ldms\n", GC_get_time_limit());
	fprintf(stderr, "Heap size: %ldkb\n", GC_get_heap_size() / 1024);
	fprintf(stderr, "Free size: %ldkb\n", GC_get_free_bytes() / 1024);
	fprintf(stderr, "Incremental needs: %d\n", GC_incremental_protection_needs());
	/* GC_enable_incremental(); */

	// parse settings.
	config = settings_init();
	if (!settings_parse(argc, argv, &config)) {
		return 1;
	}

	// initialize main thread libevent instance.
	main_base = event_init();

	// initialize other stuff.
	conn_init();

	// start up worker threads.
	thread_init(config.threads, main_base);

	// generate some user test data if asked for.
	if (config.users > 0) {
		fprintf(stderr, "Generating %d test users\n", config.users);
		config.stats = new_stats(config.users / 3 + 1);
		stat_test_data(config.stats, config.users);
	} else {
		config.stats = new_stats(STATS_HASH_MAP_SIZE);
	}

	// start our timer loop.
	time_loop(0, 0, NULL);

	// create the listening socket, bind it, and init.
	errno = 0;
	if (server_socket(config.tcpport)) {
		fprintf(stderr, "failed to listen on TCP port %d", config.tcpport);
		exit(EX_OSERR);
	}

	// Give the sockets a moment to open. I know this is dumb, but the error is
	// only an advisory.
	usleep(1000);

	// enter the event loop.
	if (event_base_loop(main_base, 0) != 0) {
		retval = EXIT_FAILURE;
	}

	return retval;
}

// create a new socket set in non-blocking mode.
static int new_socket(struct addrinfo *ai) {
	int sfd; int flags;

	if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
		return -1;
	}

	if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
		fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("setting O_NONBLOCK"); close(sfd); return -1;
	}
	return sfd;
}

// Create a socket and bind it to a specific port number
static int server_socket(int port) {
	int sfd;
	struct linger ling = {0, 0};
	struct addrinfo *ai;
	struct addrinfo *next;
	struct addrinfo hints = { .ai_flags = AI_PASSIVE,
	                          .ai_family = AF_UNSPEC,
	                          .ai_socktype = SOCK_STREAM };
	char port_buf[NI_MAXSERV];
	int error;
	int success = 0;
	int flags =1;

	if (port == -1) {
		port = 0;
	}

	snprintf(port_buf, sizeof(port_buf), "%d", port);
	error = getaddrinfo(NULL, port_buf, &hints, &ai);
	if (error != 0) {
		if (error != EAI_SYSTEM) {
			fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
		} else {
			perror("getaddrinfo()");
		}
		return 1;
	}

	for (next = ai; next; next = next->ai_next) {
		conn *c;
		if ((sfd = new_socket(next)) == -1) {
			/* getaddrinfo can return "junk" addresses,
			* we make sure at least one works before erroring.
			*/
			if (errno == EMFILE) {
				/* ...unless we're out of fds */
				perror("server_socket");
				exit(EX_OSERR);
			}
			continue;
		}

		setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
		error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
		if (error != 0) perror("setsockopt");
		error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
		if (error != 0) perror("setsockopt");
		error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
		if (error != 0) perror("setsockopt");

		if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
			if (errno != EADDRINUSE) {
				perror("bind()");
				close(sfd);
				freeaddrinfo(ai);
				return 1;
			}
			close(sfd);
			continue;
		}

		success++;
		if (listen(sfd, 1024) == -1) {
			perror("listen()");
			close(sfd);
			freeaddrinfo(ai);
			return 1;
		}

		if (!(c = conn_new(client_conn, 0, sfd, conn_listening,
		                   EV_READ | EV_PERSIST,
		                   1, main_base))) {
			fprintf(stderr, "failed to create listening connection\n");
			exit(EXIT_FAILURE);
		}
		c->next = listen_conn;
		listen_conn = c;
	}

	freeaddrinfo(ai);

	/* Return zero iff we detected no errors in starting up connections */
	return success == 0;
}

// event loop entry point.
void event_handler(const int fd, const short which, void *arg) {
	conn *c;
	c = (conn *)arg;
	assert(c != NULL);
	c->which = which;

	/* sanity */
	if (fd != c->sfd) {
		if (config.verbose > 0) {
			fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
		}
		conn_close(c);
		return;
	}

	drive_machine(c);

	// onto next event.
	return;
}

// state machine driver for handling an event.
static void drive_machine(conn *c) {
	int stop = 0;
	int sfd, flags = 1;
	socklen_t addrlen;
	struct sockaddr_storage addr;
	int nreqs = REQ_PER_EVENT;
	int res;
	const char *str;
	struct timeval t = { 0, 0 };

	assert(c != NULL);

	while (!stop) {
		switch(c->state) {

		case conn_listening:
			addrlen = sizeof(addr);
			if ((sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen)) == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					/* these are transient, so don't log anything */
				} else if (errno == EMFILE) {
					if (config.verbose > 0)
						fprintf(stderr, "Too many open connections\n");
				} else {
					perror("accept()");
				}
				stop = 1;
				break;
			}
			if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
			    fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
				perror("setting O_NONBLOCK");
				close(sfd);
				break;
			}

			dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
			                  DATA_BUFFER_SIZE);
			stop = 1;
			break;

		case conn_new_cmd:
			if (c->mem_blob != NULL && c->mem_free_delay == 0) {
				/* free(c->mem_blob); */
				c->mem_blob = NULL;
			} else if (c->mem_free_delay > 0) {
				if (config.verbose > 1) {
					fprintf(stderr, "wait left %d\n", c->mem_free_delay);
				}
				c->mem_free_delay--;
			}

			// Only process nreqs at a time to avoid starving other connections
			--nreqs;
			if (nreqs >= 0) {
				reset_cmd_handler(c);
			} else {
				if (c->rbytes > 0) {
					// We have already read in data into the input buffer, so
					// libevent will most likely not signal read events on the
					// socket (unless more data is available. As a hack we should
					// just put in a request to write data, because that should be
					// possible ;-)
					if (!conn_update_event(c, EV_WRITE | EV_PERSIST)) {
						conn_set_state(c, conn_closing);
						break;
					}
				} else {
					if (!conn_update_event(c, EV_READ | EV_PERSIST)) {
						conn_set_state(c, conn_closing);
						break;
					}
				}
				stop = 1;
			}
			break;

			case conn_waiting:
				if (!conn_update_event(c, EV_READ | EV_PERSIST)) {
					conn_set_state(c, conn_closing);
					break;
				}
				conn_set_state(c, conn_read);
				stop = 1;
				break;

			case conn_read:
				res = read_network(c);
				switch (res) {
				case READ_NO_DATA_RECEIVED:
					conn_set_state(c, conn_waiting);
					break;
				case READ_DATA_RECEIVED:
					conn_set_state(c, conn_parse_cmd);
					break;
				case READ_ERROR:
					conn_set_state(c, conn_closing);
					break;
				case READ_MEMORY_ERROR:
					c->rbytes = 0; // ignore what we read.
					error_response(c, "SERVER_ERROR out of memory reading request");
					c->after_write = conn_closing;
					break;
				}
				break;

			case conn_parse_cmd:
				if (!read_command(c)) {
					// we need more data!
					conn_set_state(c, conn_waiting);
				}
				break;

			case conn_read_value:
				// read value just swallows the value.
				if (c->sbytes == 0) {
					error_response(c, "STORED");
					break;
				}
				// fall through...

			case conn_swallow:
				// we are reading sbytes and throwing them away.
				if (c->sbytes == 0) {
					conn_set_state(c, conn_new_cmd);
					break;
				}

				// first check if we have leftovers in the conn_read buffer.
				if (c->rbytes > 0) {
					int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
					c->sbytes -= tocopy;
					c->rcurr += tocopy;
					c->rbytes -= tocopy;
					break;
				}

				//  now try reading from the socket.
				res = read(c->sfd, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
				if (res > 0) {
					c->sbytes -= res;
					break;
				} else if (res == 0) { // end of stream.
					conn_set_state(c, conn_closing);
					break;
				} else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
					if (!conn_update_event(c, EV_READ | EV_PERSIST)) {
						conn_set_state(c, conn_closing);
						break;
					}
					stop = true;
					break;
				} else {
					// otherwise we have a real error, on which we close the connection.
					if (config.verbose > 0) {
						fprintf(stderr, "Failed to read, and not due to blocking\n");
					}
					conn_set_state(c, conn_closing);
				}
				break;

			case conn_rpc_wait:
				// stop listening for events, we'll awaken when RPC's done.
				if (!conn_update_event(c, 0)) {
					conn_set_state(c, conn_closing);
					break;
				}
				stop = true;
				break;

			case conn_rpc_done:
				finish_get_command(c);
				break;

			case conn_write:
				// We want to write out a simple response. If we haven't already,
				// assemble it into a msgbuf list (this will be a single-entry list
				// for TCP).
            if (c->iovused == 0) {
					if (!conn_add_iov(c, c->wcurr, c->wbytes)) {
						if (config.verbose > 0) {
							fprintf(stderr, "Couldn't build response\n");
						}
						conn_set_state(c, conn_closing);
						break;
					}
				}
            // fall through...

			case conn_mwrite:
				switch (transmit(c)) {
				case TRANSMIT_COMPLETE:
					if (c->state == conn_mwrite) {
						// we reach this when all remaining data is output, so we know we
						// can safely release _all_ retained items. We perhaps could have
						// released them earlier but this way is easy and safe.
						while (c->ileft > 0) {
							// release the item.
							item *it = *(c->icurr);
							item_remove(it);
							c->icurr++;
							c->ileft--;
						}
						conn_set_state(c, conn_new_cmd);
					} else if (c->state == conn_write) {
						conn_set_state(c, c->after_write);
					} else {
						if (config.verbose > 0) {
							fprintf(stderr, "Unexpected state %d\n", c->state);
						}
						conn_set_state(c, conn_closing);
					}
					break;

				case TRANSMIT_INCOMPLETE:
				case TRANSMIT_HARD_ERROR:
					break; // Continue in state machine.

				case TRANSMIT_SOFT_ERROR:
					stop = true;
					break;
				}
				break;

		case conn_timeout:
			c->old_ev_flags = c->ev_flags;
			t.tv_usec = c->timeout;
			if (!conn_update_event_t(c, 0, &t)) {
				conn_set_state(c, conn_closing);
			} else {
				conn_set_state(c, conn_timeout_post);
			}
			stop = true;
			break;

		case conn_timeout_post:
			conn_set_state(c, c->after_timeout);
			if (!conn_update_event(c, c->old_ev_flags)) {
				conn_set_state(c, conn_closing);
				break;
			}
			break;

		case conn_closing:
			conn_close(c);
			stop = true;
			break;

		case conn_min_state:
		case conn_max_state:
			assert(false);
			break;
		}
	}
}

// reset a connection to a ready-state for processing a new request.
static void reset_cmd_handler(conn *c) {
	c->cmd = -1;
	conn_shrink(c);
	if (c->rbytes > 0) {
		conn_set_state(c, conn_parse_cmd);
	} else {
		conn_set_state(c, conn_waiting);
	}
}

// read from network as much as we can, handle buffer overflow and connection
// close. before reading, move the remaining incomplete fragment of a command
// (if any) to the beginning of the buffer.
// 
// To protect us from someone flooding a connection with bogus data causing the
// connection to eat up all available memory, break out and start looking at
// the data I've got after a number of reallocs...
static read_result read_network(conn *c) {
	read_result ret = READ_NO_DATA_RECEIVED;
	int res;
	#define MAX_ALLOCS 4
	int num_allocs = 0;
	assert(c != NULL);

	if (c->rcurr != c->rbuf) {
		if (c->rbytes != 0) {
			memmove(c->rbuf, c->rcurr, c->rbytes);
		}
		c->rcurr = c->rbuf;
	}

	while (1) {
		if (c->rbytes >= c->rsize) {
			if (num_allocs == MAX_ALLOCS) {
				return ret;
			}
			++num_allocs;
			char *new_rbuf = GC_REALLOC(c->rbuf, c->rsize * 2);
			if (!new_rbuf) {
				if (config.verbose > 0) {
					fprintf(stderr, "Couldn't realloc input buffer\n");
				}
				return READ_MEMORY_ERROR;
			}
			c->rcurr = c->rbuf = new_rbuf;
			c->rsize *= 2;
		}

		int avail = c->rsize - c->rbytes;
		res = read(c->sfd, c->rbuf + c->rbytes, avail);
		if (res > 0) {
			ret = READ_DATA_RECEIVED;
			c->rbytes += res;
			if (res < avail) {
				break; // all data read
			} else {
				continue; // still more on wire
			}
		} else if (res == 0) {
			return READ_ERROR;
		} else if (res == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			return READ_ERROR;
		}
	}
	return ret;
}

// if we have a complete line in the buffer, process it.
// @return false when more data is needed, true otherwise.
static bool read_command(conn *c) {
	assert(c != NULL);
	assert(c->rcurr <= (c->rbuf + c->rsize));
	assert(c->rbytes > 0);

	char *el, *cont;

	if (c->rbytes == 0) return false;

	// find end-of-line.
	el = memchr(c->rcurr, '\n', c->rbytes);

	// not found.
	if (!el) {
		if (c->rbytes > 1024) {
			// We didn't have a '\n' in the first k.
			char *ptr = c->rcurr;
			while (*ptr == ' ') ++ptr;
			// This _has_ to be a large multiget, if not we should just nuke the
			// connection.
			if (ptr - c->rcurr > 100 ||
			    (strncmp(ptr, "get ", 4) && strncmp(ptr, "gets ", 5))) {
				conn_set_state(c, conn_closing);
				return true;
			}
		}
		return false;
	}

	// found end-of-line.
	cont = el + 1;
	if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
		el--;
	}
	*el = '\0';

	// process the command.
	assert(cont <= (c->rcurr + c->rbytes));
	process_command(c, c->rcurr);
	c->rbytes -= (cont - c->rcurr);
	c->rcurr = cont;
	assert(c->rcurr <= (c->rbuf + c->rsize));
	return true;
}

// parse a single memcached request.
static void process_command(conn *c, char *command) {
	assert(c != NULL);

	if (config.verbose > 1) {
		fprintf(stderr, "<%d %s\n", c->sfd, command);
	}

	// We reset the output buffers as we are reading now. We keep
	// reading/writing as exclusive modes with no overlap.
	c->msgcurr = 0;
	c->msgused = 0;
	c->iovused = 0;
	if (!conn_add_msghdr(c) != 0) {
		error_response(c, "SERVER_ERROR out of memory preparing response");
		return;
	}

	// `parse_command` also handles dispatching.
	if (c->type == client_conn && !parse_command(c, command)) {
		error_response(c, "ERROR");
	} else if (c->type == memcached_conn && !memcached_response(c, command)) {
		err(1, "parse_rpc(): unknown response!");
	}
}

// Transmit the next chunk of data from our list of msgbuf structures.
// 
// Returns:
//   TRANSMIT_COMPLETE   All done writing.
//   TRANSMIT_INCOMPLETE More data remaining to write.
//   TRANSMIT_SOFT_ERROR Can't write any more right now.
//   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
static write_result transmit(conn *c) {
	assert(c != NULL);

	// check if current msg finished and more msgs remaining.
	if (c->msgcurr < c->msgused && c->msglist[c->msgcurr].msg_iovlen == 0) {
		c->msgcurr++;
	}

	// check remaining messages.
	if (c->msgcurr < c->msgused) {
		ssize_t res;
		struct msghdr *m = &c->msglist[c->msgcurr];

		res = sendmsg(c->sfd, m, 0);
		if (res > 0) {
			// We've written some of the data. Remove the completed iovec entries
			// from the list of pending writes.
			while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
				res -= m->msg_iov->iov_len;
				m->msg_iovlen--;
				m->msg_iov++;
			}

			// Might have written just part of the last iovec entry; adjust it so
			// the next write will do the rest.
			if (res > 0) {
				m->msg_iov->iov_base = (caddr_t)m->msg_iov->iov_base + res;
				m->msg_iov->iov_len -= res;
			}
			return TRANSMIT_INCOMPLETE;

		} else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!conn_update_event(c, EV_WRITE | EV_PERSIST)) {
				conn_set_state(c, conn_closing);
				return TRANSMIT_HARD_ERROR;
			}
			return TRANSMIT_SOFT_ERROR;

		} else {
			// if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK, we
			// have a real error, on which we close the connection.
			if (config.verbose > 0) {
				perror("Failed to write, and not due to blocking");
			}
			conn_set_state(c, conn_closing);
			return TRANSMIT_HARD_ERROR;
		}
	} else {
		return TRANSMIT_COMPLETE;
	}
}

// Write out a string to the connection, clearing an existing pending data.
// Only used for error cases.
void error_response(conn *c, const char *str) {
	size_t len;

	assert(c != NULL);

	if (config.verbose > 1) fprintf(stderr, ">%d %s\n", c->sfd, str);

	/* Nuke a partial output... */
	c->msgcurr = 0;
	c->msgused = 0;
	c->iovused = 0;
	conn_add_msghdr(c);

	len = strlen(str);
	if ((len + 2) > c->wsize) {
		/* ought to be always enough. just fail for simplicity */
		str = "SERVER_ERROR output line too long";
		len = strlen(str);
	}

	memcpy(c->wbuf, str, len);
	memcpy(c->wbuf + len, "\r\n", 2);
	c->wbytes = len + 2;
	c->wcurr = c->wbuf;

	conn_set_state(c, conn_write);
	c->after_write = conn_new_cmd;
	return;
}

static void time_loop(const int fd, const short which, void *arg) {
	struct timeval t = {.tv_sec = GC_INTERVAL, .tv_usec = 0};
	static struct event clockevent;
	static bool initialized = false;
	static int count = 0;

	if (initialized) {
		evtimer_del(&clockevent);
	} else {
		initialized = true;
	}

	evtimer_set(&clockevent, time_loop, 0);
	event_base_set(main_base, &clockevent);
	evtimer_add(&clockevent, &t);

	fprintf(stderr, "running collection...\n");
	while (GC_collect_a_little()) {}

	if (++count % 6) GC_gcollect();
}

