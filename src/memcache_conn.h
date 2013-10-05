// memcached connection handling

#ifndef _MEMCACHED_CONN_H
#define _MEMCACHED_CONN_H

#include <stdbool.h>

typedef struct _memcache_conn_t {
	int sfd;
} memcached_t;

memcached_t* memcache_connect(char *host);

#endif
