#include "memcache_conn.h"
#include "server.h"
#include "utils.h"

#include <assert.h>
#include <event.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

// prototypes.
static void process_hit_response(conn *c, token_t *tokens, size_t ntokens);
static void process_miss_response(conn *c, token_t *tokens, size_t ntokens);

// default memcached port.
static const char *memcache_port = "11211";

// connect to a memcache server
memcached_t* memcache_connect(struct event_base *base, char *host) {
	int flags = 1, error = 0, sfd;
	struct addrinfo *ai;
	struct linger ling = {0, 0};
	memcached_t *mc;

	struct addrinfo hints;
	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_socktype = SOCK_STREAM;

	if (config.verbose > 0) {
		fprintf(stderr, "Connecting to host %s...", host);
	}

	int slen = strlen(host);
	char *port = (char *) memchr(host, ':', slen);
	if (port == NULL || (port - host) == slen) {
		port = (char *) memcache_port;
	} else {
		// XXX: Hack, we destroy the string...
		*port = '\0';
		port++;
	}

	// dns lookup
	error = getaddrinfo(host, port, &hints, &ai);
	if (error != 0) {
		if (error != EAI_SYSTEM) {
			fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
		} else {
			perror("getaddrinfo()");
		}
		if (config.verbose > 0) {
			fprintf(stderr, "failed!\n");
		}
		return NULL;
	}

	// create socket
	if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
		fprintf(stderr, "Couldn't create new socket!\n");
		return NULL;
	}

	// set socket options
	setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
	error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
	if (error != 0) perror("setsockopt");
	error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
	if (error != 0) perror("setsockopt");
	error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
	if (error != 0) perror("setsockopt");

	// connect
	error = connect(sfd, ai->ai_addr, ai->ai_addrlen);
	if (error != 0) {
		perror("connect");
		if (config.verbose > 0) {
			fprintf(stderr, "failed!\n");
		}
		return NULL;
	}

	// put in non-blocking mode
	if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
		fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("setting O_NONBLOCK");
		close(sfd);
		return NULL;
	}

	if (config.verbose > 0) {
		fprintf(stderr, "success!\n");
	}

	mc = conn_new(memcached_conn, 0, sfd, conn_new_cmd,
                 EV_READ | EV_PERSIST, DATA_BUFFER_SIZE, base);
	return mc;
}

// get a memcache value associated with the given key.
bool memcache_get(conn *mc, conn *c, char *key) {
	assert(mc != NULL);
	assert(c != NULL);
	assert(key != NULL);

	int keylen = strnlen(key, KEY_MAX_LENGTH);
	if (!conn_add_iov(mc, "get ", 4) ||
	    !conn_add_iov(mc, key, keylen) ||
		 !conn_add_iov(mc, "\r\n", 2)) {
		if (config.verbose > 0) {
			error_response(c, "SERVER_ERROR out of memory performing backend rpc");
		}
		return false;
	}
	
	// queue the rpc on the memcached connection.
	conn_ensure_rpc_space(mc);
	mc->rpc[mc->rpcused] = c;
	mc->rpcused++;
	
	// increase the wait count of the client connection.
	c->rpcwaiting++;

	// tell memcacche connection to write output.
	conn_set_state(mc, conn_mwrite);
	// questionable if we should always change it to write mode, may want better
	// balancing between read and write but this is easiest. Also may be better
	// to queue the memcache connection through another method than event
	// notification.
	if (!conn_update_event(mc, EV_WRITE | EV_PERSIST)) {
		conn_set_state(mc, conn_closing);
		return false;
	}
	return true;
}

// parse a single memcached response (rpc).
bool memcached_response(conn *mc, char *response) {
	token_t tokens[MAX_TOKENS];
	size_t ntokens;

	assert(mc != NULL);
	assert(response != NULL);

	ntokens = tokenize_command(response, tokens, MAX_TOKENS);
	if (ntokens >= 5 && (strcmp(tokens[COMMAND_TOKEN].value, "VALUE") == 0)) {
		process_hit_response(mc, tokens, ntokens);
	} else if (ntokens >= 2 && (strcmp(tokens[COMMAND_TOKEN].value, "END") == 0)) {
		process_miss_response(mc, tokens, ntokens);
	} else {
		fprintf(stderr, "unknown command: %s [%lu]\n",
			tokens[COMMAND_TOKEN].value, ntokens);
		return false;
	}

	return true;
}

// process a hit response to a get rpc.
static void process_hit_response(conn *mc, token_t *tokens, size_t ntokens) {
	// get conn this response is for.
	conn *c = mc->rpc[mc->rpcdone];
	mc->rpcdone++;

	if (config.verbose > 1) {
		fprintf(stderr, "%d: received rpc response (waiting on %d more)\n",
			c->sfd, c->rpcwaiting - 1);
	}

	// if 0 can respond...
	c->rpcwaiting--;
	if (c->rpcwaiting == 0) {
		if (!conn_update_event(c, EV_WRITE | EV_PERSIST)) {
			// TODO: we need to reschedule c for this to actually occur...
			conn_set_state(c, conn_closing);
		} else {
			conn_set_state(c, conn_rpc_done);
		}
	}
	
	// get value length.
	int vlen;
	if (!safe_strtol(tokens[3].value, (int32_t *)&vlen)) {
		fprintf(stderr, "ERROR parsing rpc response! (%s)\n", tokens[3].value);
		exit(1);
	}

	// NOTE: We assume we only ever request one key at a time, so that we can
	// parse the END value now.
	// swallow: <VALUE>\r\nEND\r\n
	mc->sbytes = vlen + 7;
	conn_set_state(mc, conn_swallow);
}

// process a miss response to a get rpc.
static void process_miss_response(conn *mc, token_t *tokens, size_t ntokens) {
	// get conn this response is for.
	conn *c = mc->rpc[mc->rpcdone];
	mc->rpcdone++;

	// if 0 can respond...
	c->rpcwaiting--;
	if (c->rpcwaiting == 0) {
		if (!conn_update_event(c, EV_WRITE | EV_PERSIST)) {
			// TODO: we need to reschedule c for this to actually occur...
			conn_set_state(c, conn_closing);
		} else {
			conn_set_state(c, conn_rpc_done);
		}
	}

	conn_set_state(mc, conn_new_cmd);
}

