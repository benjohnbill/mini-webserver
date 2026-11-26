#ifndef CONNECTION_H
#define CONNECTION_H

#include "handlers.h"
#include <poll.h>

typedef struct Connection Connection;

/* The caller owns the client fd. Each step does bounded work, then yields. */
Connection *connection_new(int client, const ServerConfig *config);
void connection_free(Connection *connection);
struct pollfd connection_interest(const Connection *connection);
int64_t connection_deadline(const Connection *connection);
void connection_step(Connection *connection);
void connection_timeout(Connection *connection);
void handle_client(int client, const ServerConfig *config);

#endif
