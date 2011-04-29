#ifndef __HASHCLIENT_H_
#define __HASHCLIENT_H_

#include "hashprotocol.h"

/**
 * handler for connection to hash table server
 */
typedef struct hashconn * hashconn_t;

/**
 * open connection to the hash server
 */
int openconn(hashconn_t *conn, const char *serverip, int port);

/**
 * close connection to hash server and free buffers
 */
void closeconn(hashconn_t conn);

/**
 * Send single query to hash server.
 */
int sendquery(hashconn_t conn, struct client_query *query);

/**
 * Send queries to hash server as batch.
 */
int sendqueries(hashconn_t conn, int nqueries, struct client_query *queries);

/**
 * Read value received from server, values are received in same order as queries are sent
 */
int readvalue(hashconn_t conn, void *value);

#endif
