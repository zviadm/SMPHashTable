#include <arpa/inet.h>
#include <assert.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hashclient.h"
#include "hashprotocol.h"

struct hashconn {
  int socket;
  FILE * fin;
  FILE * fout;
};

int openconn(struct hashconn **conn, const char *serverip, int port) 
{
  *conn = (struct hashconn *)malloc(sizeof(struct hashconn));

  struct sockaddr_in sin;
  int ret, yes = 1;

  (*conn)->socket = socket(AF_INET, SOCK_STREAM, 0);
  assert((*conn)->socket >= 0);

  setsockopt((*conn)->socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = inet_addr(serverip);
  ret = connect((*conn)->socket, (struct sockaddr *) &sin, sizeof(sin));
  if(ret < 0) {
    return -1;
  }

  (*conn)->fin = fdopen((*conn)->socket, "r");
  (*conn)->fout = fdopen((*conn)->socket, "w");
  if ((*conn)->fin == NULL || (*conn)->fout == NULL) {
    return -1;
  }
  return 0;
}

void closeconn(struct hashconn *conn) 
{
  fclose(conn->fin);
  fclose(conn->fout);
  close(conn->socket);
  free(conn);
}

int sendquery_(struct hashconn *conn, struct client_query *query)
{
  size_t r;
  if (conn == NULL || conn->fout == NULL) return 0;

  struct hash_query q;
  q.optype = query->optype;
  q.size   = query->size;
  q.key    = query->key;

  r = fwrite(&q, sizeof(struct hash_query), 1, conn->fout);
  if (r != 1) return 0;

  if (query->optype == OPTYPE_INSERT && query->size > 0) {
    r = fwrite(query->value, 1, query->size, conn->fout);
    if (r != query->size) return 0;
  }
  return 1;
}

int sendquery(struct hashconn *conn, struct client_query *query)
{
  int r = sendquery_(conn, query);
  if (r) fflush(conn->fout);
  return r;
}

int sendqueries(struct hashconn *conn, int nqueries, struct client_query *queries)
{
  int r;
  for (int i = 0; i < nqueries; i++) {
    r = sendquery_(conn, &queries[i]);
    if (r == 0) return r;
  }
  fflush(conn->fout);
  return 1;
}

int readvalue(struct hashconn *conn, void *value)
{
  size_t r;
  assert(conn != NULL);
  assert(conn->fin != NULL);

  uint32_t size;
  r = fread(&size, sizeof(uint32_t), 1, conn->fin);
  if (r != 1) return -1;

  if (size > 0) {
    r = fread(value, 1, size, conn->fin);
    if (r != size) return -1;
  }
  return (int)size;
}
