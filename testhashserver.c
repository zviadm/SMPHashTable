#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashclient.h"
#include "smphashtable.h"

void test1() 
{
  printf("----------- Test 1 Start -----------\n");
  printf("Create connection to server...\n");
  
  hashconn_t conn;
  if (openconn(&conn, "127.0.0.1") < 0) {
    printf("failed to connect to server\n");
    return;
  }

  const int nqueries = 1000;
  struct hash_query queries[2 * nqueries];
  void * values[nqueries];
  long data[nqueries];

  for (int i = 0; i < nqueries; i++) {
    queries[i].optype = OPTYPE_INSERT;
    queries[i].key = i;
    queries[i].size = 8;

    data[i] = i;
    values[i] = &data[i];

    queries[nqueries + i].optype = OPTYPE_LOOKUP;
    queries[nqueries + i].key = i;
  }

  printf("Sending Insert Queries...\n");
  sendqueries(conn, nqueries, queries, values);
  printf("Sending Lookup Queries...\n");
  sendqueries(conn, nqueries, &queries[nqueries], NULL);

  printf("Reading received values...\n");
  for (int i = 0; i < nqueries; i++) {
    long val;
    int size = readvalue(conn, &val);
    assert(size == 8);
    assert(val == i);
  }

  closeconn(conn);      

  printf("----------- Test 1 Done! -----------\n");
}

int main(int argc, char *argv[])
{
  test1();
  return 0;
}
