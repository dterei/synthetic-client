// The various memcached commands that we support.

#ifndef _COMMANDS_H
#define _COMMANDS_H

#include "connections.h"
#include "protocol.h"

#include <stdbool.h>
#include <unistd.h>

void process_get_command(conn *c, token_t *tokens, size_t ntokens,
                         bool return_cas);
void process_update_command(conn *c, token_t *tokens, const size_t ntokens,
                            int comm, bool handle_cas);

#endif

