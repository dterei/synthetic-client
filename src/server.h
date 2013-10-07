// Memcached fake, benchmarking server.
#ifndef _SERVER_H
#define _SERVER_H

#include "connections.h"
#include "protocol.h"
#include "settings.h"

extern settings config;
void event_handler(const int fd, const short which, void *arg);
void error_response(conn *c, const char *str);

#endif

