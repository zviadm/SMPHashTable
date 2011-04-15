#ifndef __HASHCLIENT_H_
#define __HASHCLIENT_H_

#include "smphashtable.h"

/**
 * handler for connection to hash table server
 */
typedef struct hashconn * hashconn_t;

int openconn(struct hashconn **conn, const char *serverip);

void closeconn(hashconn_t conn);

void sendqueries(hashconn_t conn, int nqueries, struct hash_query *queries, void **values);

int readvalue(hashconn_t conn, void *value);

#endif
