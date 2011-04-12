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
#include "smphashtable.h"

struct hashconn {
  int socket;
  FILE * file;
};

int openconn(struct hashconn **conn, const char *serverip) 
{
  *conn = (struct hashconn *)malloc(sizeof(struct hashconn));

  struct sockaddr_in sin;
  int ret, yes = 1;

  (*conn)->socket = socket(AF_INET, SOCK_STREAM, 0);
  assert((*conn)->socket >= 0);

  setsockopt((*conn)->socket, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(2117);
  sin.sin_addr.s_addr = inet_addr(serverip);
  ret = connect((*conn)->socket, (struct sockaddr *) &sin, sizeof(sin));
  if(ret < 0) {
    return -1;
  }

  (*conn)->file = fdopen((*conn)->socket, "r+");
  if ((*conn)->file == NULL) {
    return -1;
  }
  return 0;
}

void closeconn(struct hashconn *conn) 
{
  fclose(conn->file);
  close(conn->socket);
  free(conn);
}

void sendqueries(struct hashconn *conn, int nqueries, struct hash_query *queries, void **values)
{
  assert(conn != NULL);
  assert(conn->file != NULL);

  fwrite(&nqueries, sizeof(int), 1, conn->file);
  fwrite(queries, sizeof(struct hash_query), nqueries, conn->file); 
  fflush(conn->file);
  for (int i = 0; i < nqueries; i++) {
    if (queries[i].optype == OPTYPE_INSERT) {
      fwrite(values[i], 1, queries[i].size, conn->file);
    }
  }
  fflush(conn->file);
}

void readvalue(struct hashconn *conn, struct hash_query *query, void *value)
{
  assert(conn != NULL);
  assert(conn->file != NULL);
  assert(query->optype == OPTYPE_LOOKUP);

  int r = fread(value, 1, query->size, conn->file);
  assert(r == query->size);
}
