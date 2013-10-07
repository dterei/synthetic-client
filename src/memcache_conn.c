#include "memcache_conn.h"
#include "server.h"

#include <event.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

static char *memcache_port = "11211";

// connect to a memcache server
memcached_t* memcache_connect(struct event_base *base, char *host) {
	int flags = 1, error = 0, sfd;
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM };
	struct addrinfo *ai;
	struct linger ling = {0, 0};
	memcached_t *mc;

	if (config.verbose > 0) {
		fprintf(stderr, "Connecting to host %s...", host);
	}

	// dns lookup
	error = getaddrinfo(host, memcache_port, &hints, &ai);
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

	mc = conn_new(memcached_conn, sfd, conn_listening,
                 EV_READ | EV_PERSIST, DATA_BUFFER_SIZE, base);
	return mc;
}

// get a memcache value associated with the given key.
bool memcache_get(conn *mc, conn *c, char *key) {
	int keylen = strnlen(key, KEY_MAX_LENGTH);
	if (!conn_add_iov(mc, "GET ", 4) ||
	    !conn_add_iov(mc, key, keylen) ||
		 !conn_add_iov(mc, "\r\n", 2)) {
		if (config.verbose > 0) {
			fprintf(stderr, "memcache_get(): error with conn_add_iov");
		}
		return false;
	}
	
	// queue the rpc on the memcached connection.
	conn_ensure_rpc_space(mc);
	mc->rpc[mc->rpcused] = c;
	mc->rpcused++;
	
	// increase the wait count of the client connection.
	c->rpcwaiting++;
	return true;
}

