#ifndef __HASHCLIENT_H_
#define __HASHCLIENT_H_

#include "smphashtable.h"

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
 * Send queries to hash server as batch. This works with first version of hash server
 */
void sendqueries(hashconn_t conn, int nqueries, struct hash_query *queries, void **values);

/**
 * Send queries to hash server. This works with second version of hash server
 */
void sendqueries2(struct hashconn *conn, int nqueries, struct hash_query *queries, void **values);

/**
 * Read value received from server, values are received in same order as queries are sent
 */
int readvalue(hashconn_t conn, void *value);

#endif
