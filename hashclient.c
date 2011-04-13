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
  FILE * fin;
  FILE * fout;
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

void sendqueries(struct hashconn *conn, int nqueries, struct hash_query *queries, void **values)
{
  size_t r;
  assert(conn != NULL);
  assert(conn->fout != NULL);

  r = fwrite(&nqueries, sizeof(int), 1, conn->fout);
  assert(r == 1);
  r = fwrite(queries, sizeof(struct hash_query), nqueries, conn->fout); 
  assert(r == nqueries);
  fflush(conn->fout);
  for (int i = 0; i < nqueries; i++) {
    if (queries[i].optype == OPTYPE_INSERT) {
      //printf("%d, %d, %p\n", i, queries[i].size, values[i]);
      r = fwrite(values[i], 1, queries[i].size, conn->fout);
      assert(r == queries[i].size);
    }
  }
  fflush(conn->fout);
}

int readvalue(struct hashconn *conn, struct hash_query *query, void *value)
{
  size_t r;
  assert(conn != NULL);
  assert(conn->fin != NULL);
  assert(query->optype == OPTYPE_LOOKUP);

  int size;
  r = fread(&size, sizeof(int), 1, conn->fin);
  assert(r == 1);

  if (size > 0) {
    r = fread(value, 1, size, conn->fin);
    assert(r == size);
  }
  return size;
}
