// The various memcached commands that we support.

#include "commands.h"
#include "connections.h"
#include "locking.h"
#include "protocol.h"
#include "server.h"
#include "threads.h"
#include "utils.h"

#include <assert.h>
#include <gsl/gsl_randist.h>
#include <stdio.h>
#include <stdlib.h>

#define GC_THREADS
#include <gc.h>
#define GC_CALLOC(m,n) GC_MALLOC((m)*(n))

static const char* default_key = "skeleton";

/* // process a memcached get(s) command. (we don't support CAS). This function */
/* // performs the request parsing and setup of backend RPC's. */
/* void process_get_command(conn *c, token_t *tokens, size_t ntokens, */
/*                                 bool return_cas) { */
/* 	char *key; */
/* 	size_t nkey; */
/* 	int i = 0; */
/* 	item *it; */
/* 	token_t *key_token = &tokens[KEY_TOKEN]; */
/* 	char *suffix; */
/* 	worker_thread_t *t = c->thread; */
/* 	memcached_t *mc; */
/*  */
/* 	assert(c != NULL); */
/*  */
/* 	key  = key_token->value; */
/* 	nkey = key_token->length; */
/*  */
/* 	if (config.use_dist) { */
/* 		long size = config.dist_arg1 + gsl_ran_gaussian(config.r, config.dist_arg2); */
/* 		if (config.verbose > 1) { */
/* 			fprintf(stderr, "allocated blob: %ld\n", size); */
/* 		} */
/* 		c->mem_blob = malloc(sizeof(char) * size); */
/* 	} */
/*  */
/* 	if(nkey > KEY_MAX_LENGTH) { */
/* 		error_response(c, "CLIENT_ERROR bad command line format"); */
/* 		return; */
/* 	} */
/*  */
/* 	// lookup key-value. */
/* 	it = item_get(key, nkey); */
/*  */
/* 	// hit. */
/* 	if (it) { */
/* 		if (i >= c->isize && !conn_expand_items(c)) { */
/* 			item_remove(it); */
/* 			error_response(c, "SERVER_ERROR out of memory writing get response"); */
/* 			return; */
/* 		} */
/* 		// add item to remembered list (i.e., we've taken ownership of them */
/* 		// through refcounting and later must release them once we've */
/* 		// written out the iov associated with them). */
/* 		item_update(it); */
/* 		*(c->ilist + i) = it; */
/* 		i++; */
/* 	} */
/*  */
/* 	// make sure it's a single get */
/* 	key_token++; */
/* 	if (key_token->length != 0 || key_token->value != NULL) { */
/* 		error_response(c, "SERVER_ERROR only support single `get`"); */
/* 		return; */
/* 	} */
/*  */
/* 	// update our rememberd reference set. */
/* 	c->icurr = c->ilist; */
/* 	c->ileft = i; */
/*  */
/* 	// setup RPC calls. */
/* 	for (i = 0; i < t->memcache_used; i++) { */
/* 		mc = t->memcache[i]; */
/* 		if (!conn_add_msghdr(mc) != 0) { */
/* 			error_response(mc, "SERVER_ERROR out of memory preparing response"); */
/* 			return; */
/* 		} */
/* 		memcache_get(mc, c, default_key); */
/* 	} */
/* 	conn_set_state(c, conn_rpc_wait); */
/* } */

// complete the response to a get request.
void finish_get_command(conn *c) {
	item *it;
	int i;

	// setup all items for writing out.
	for (i = 0; i < c->ileft; i++) {
		it = *(c->ilist + i);
		if (it) {
			// Construct the response. Each hit adds three elements to the
			// outgoing data list:
			//   "VALUE <key> <flags> <data_length>\r\n"
			//   "<data>\r\n"
			// The <data> element is stored on the connection item list, not on
			// the iov list.
			if (!conn_add_iov(c, "VALUE ", 6) ||
				 !conn_add_iov(c, ITEM_key(it), it->nkey) ||
				 !conn_add_iov(c, ITEM_suffix(it), it->nsuffix + it->nbytes)) {
				item_remove(it);
				error_response(c, "SERVER_ERROR out of memory writing get response");
				return;
			}

			if (config.verbose > 1) {
				fprintf(stderr, ">%d sending key %s\n", c->sfd, ITEM_key(it));
			}
		} else {
			fprintf(stderr, "ERROR corrupted ilist!\n");
			exit(1);
		}
	}

	if (config.verbose > 1) {
		fprintf(stderr, ">%d END\n", c->sfd);
	}

	if (!conn_add_iov(c, "END\r\n", 5) != 0) {
		error_response(c, "SERVER_ERROR out of memory writing get response");
	} else {
		conn_set_state(c, conn_mwrite);
	}
}

// process a memcached get(s) command. (we don't support CAS).
void process_get_command(conn *c, token_t *tokens, size_t ntokens,
                                bool return_cas) {
	char *key;
	size_t nkey;
	int i = 0;
	item *it;
	token_t *key_token = &tokens[KEY_TOKEN];
	char *suffix;

	assert(c != NULL);

	if (config.alloc && c->mem_blob == NULL) {
		long size = config.alloc_mean + gsl_ran_gaussian(c->thread->r, config.alloc_stddev);
		size = size <= 0 ? 10 : size;
		if (config.verbose > 0) {
			fprintf(stderr, "allocated blob: %ld\n", size);
		}

		c->mem_blob = GC_MALLOC_ATOMIC(sizeof(char) * size);
		c->mem_free_delay = 0;

		if (config.rtt_delay) {
			double r = config.rtt_mean + gsl_ran_gaussian(c->thread->r, config.rtt_stddev);
			if (r >= config.rtt_cutoff) {
				int wait = r / 100;
				if (config.verbose > 0) {
					fprintf(stderr, "delay: %d\n", wait);
				}
				c->mem_free_delay = wait;
				conn_set_state(c, conn_mwrite);
			}
		}
	}

	// process the whole command line, (only part of it may be tokenized right now)
	do {
		// process all tokenized keys at this stage.
		while(key_token->length != 0) {
			key = key_token->value;
			nkey = key_token->length;

			if(nkey > KEY_MAX_LENGTH) {
				error_response(c, "CLIENT_ERROR bad command line format");
				return;
			}

			// lookup key-value.
			it = item_get(key, nkey);
			
			// hit.
			if (it) {
				if (i >= c->isize && !conn_expand_items(c)) {
					item_remove(it);
					break;
				}

				// Construct the response. Each hit adds three elements to the
				// outgoing data list:
				//   "VALUE <key> <flags> <data_length>\r\n"
				//   "<data>\r\n"
				// The <data> element is stored on the connection item list, not on
				// the iov list.
				if (!conn_add_iov(c, "VALUE ", 6) != 0 ||
				    !conn_add_iov(c, ITEM_key(it), it->nkey) != 0 ||
				    !conn_add_iov(c, ITEM_suffix(it), it->nsuffix + it->nbytes) != 0) {
					item_remove(it);
					break;
				}

				if (config.verbose > 1) {
					fprintf(stderr, ">%d sending key %s\n", c->sfd, key);
				}

				// add item to remembered list (i.e., we've taken ownership of them
				// through refcounting and later must release them once we've
				// written out the iov associated with them).
				item_update(it);
				*(c->ilist + i) = it;
				i++;
			}

			key_token++;
		}

		/*
		 * If the command string hasn't been fully processed, get the next set
		 * of tokens.
		 */
		if(key_token->value != NULL) {
			ntokens = tokenize_command(key_token->value, tokens, MAX_TOKENS);
			key_token = tokens;
		}

	} while(key_token->value != NULL);

	c->icurr = c->ilist;
	c->ileft = i;

	if (config.verbose > 1) {
		fprintf(stderr, ">%d END\n", c->sfd);
	}

	// If the loop was terminated because of out-of-memory, it is not reliable
	// to add END\r\n to the buffer, because it might not end in \r\n. So we
	// send SERVER_ERROR instead.
	if (key_token->value != NULL || !conn_add_iov(c, "END\r\n", 5) != 0) {
		error_response(c, "SERVER_ERROR out of memory writing get response");
	} else {
		conn_set_state(c, conn_mwrite);
	}
}

// process a memcached set command.
void process_update_command(conn *c, token_t *tokens,
                            const size_t ntokens,
                            int comm, bool handle_cas) {
	int vlen;
	assert(c != NULL);

	if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH ||
	    !safe_strtol(tokens[4].value, (int32_t *)&vlen)) {
		error_response(c, "CLIENT_ERROR bad command line format");
		return;
	}

	if (vlen < 0) {
		error_response(c, "CLIENT_ERROR bad command line format");
		return;
	}

	// setup value to be read
	c->sbytes = vlen + 2; // for \r\n consumption.
	conn_set_state(c, conn_read_value);
}

// process a memcached stat command.
void process_stat_command(conn *c, token_t *tokens, const size_t ntokens) {
	mutex_lock(&c->stats->lock);

	// just for debugging right now
	fprintf(stderr, "STAT client_id %d\n", c->stats->client_id);
	fprintf(stderr, "STAT total_connections %d\n", c->stats->total_connections);
	fprintf(stderr, "STAT live_connections %d\n", c->stats->live_connections);
	fprintf(stderr, "STAT requests %d\n", c->stats->requests);

	mutex_unlock(&c->stats->lock);
	conn_set_state(c, conn_new_cmd);
}

