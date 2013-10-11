// memcached connection handling

#ifndef _MEMCACHED_CONN_H
#define _MEMCACHED_CONN_H

#include "connections.h"

#include <stdbool.h>

typedef conn memcached_t;

memcached_t* memcache_connect(struct event_base *base, char *host);
bool memcache_get(conn *mc, conn *c, char *key);
bool memcached_response(conn *mc, char *response);

#endif
