// The various memcached commands that we support.

#include "commands.h"
#include "connections.h"
#include "protocol.h"
#include "server.h"
#include "utils.h"

#include <gsl/gsl_randist.h>

#include <assert.h>
#include <stdio.h>

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

	// process the whole command line, (only part of it may be tokenized right now)
	do {
		// process all tokenized keys at this stage.
		while(key_token->length != 0) {
			key = key_token->value;
			nkey = key_token->length;

			if(nkey > KEY_MAX_LENGTH) {
				out_string(c, "CLIENT_ERROR bad command line format");
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
		out_string(c, "SERVER_ERROR out of memory writing get response");
	} else {
		if (config.use_dist) {
			double r = config.dist_arg1 + gsl_ran_gaussian(config.r, config.dist_arg2);
			fprintf(stderr, "delay: %f\n", r);
			conn_set_state(c, conn_timeout);
			c->after_timeout = conn_mwrite;
			c->timeout = r;
			c->msgcurr = 0;
		} else {
			conn_set_state(c, conn_mwrite);
		}
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
		out_string(c, "CLIENT_ERROR bad command line format");
		return;
	}

	if (vlen < 0) {
		out_string(c, "CLIENT_ERROR bad command line format");
		return;
	}

	// setup value to be read
	c->sbytes = vlen + 2; // for \r\n consumption.
	conn_set_state(c, conn_read_value);
}

