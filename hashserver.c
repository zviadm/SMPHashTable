#include <assert.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xmmintrin.h>

#include "localmem.h"
#include "smphashtable.h"

int design          = 1;
int nservers        = 1;
int batch_size      = 1000;
int nelems          = 100000;
size_t size         = 6400000;
struct hash_table *hash_table;

// Forward Declarations
void run_server();
void * tcpgo(void *xarg);

int main(int argc, char *argv[])
{
  int opt_char;

  while((opt_char = getopt(argc, argv, "s:c:i:n:t:m:w:d:f:b:")) != -1) {
    switch (opt_char) {
      case 's':
        nservers = atoi(optarg);
        break;
      case 'n':
        nelems = atoi(optarg);
        break;
      case 't':
        size = atol(optarg);
        break;
      case 'd':
        design = atoi(optarg);
        assert(design == 2 || design == 3);
        break;
      case 'b':
        batch_size = atoi(optarg);
        break;
      default:
        printf("hash server options are: \n"
               "   -d design (1 = naive server/client, 2 = buffering server/client, 3 = locking)\n"
               "   -s number of servers / partitions\n"
               "   -b batch size (for design 2)\n"
               "   -n max number of elements in cache\n"
               "   -t max size of cache (in bytes)\n");
        exit(-1);
    }
  }

  run_server();
}

void run_server() 
{
  hash_table = create_hash_table(size, nelems, nservers);

  printf("Starting Hash Server...\n"); 

  if (design == 1 || design == 2) {
    start_hash_table_servers(hash_table, 0);
  }

  struct sockaddr_in sin;
  int yes = 1;
  int ret;

  bzero(&sin, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(2117);

  // TCP socket and threads
  int s = socket(AF_INET, SOCK_STREAM, 0);
  assert(s >= 0);
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  ret = bind(s, (struct sockaddr *) &sin, sizeof(sin));
  if(ret < 0){
    perror("bind");
    exit(1);
  }

  listen(s, 100);

  while (1) {
    long s1;
    struct sockaddr_in sin1;
    socklen_t sinlen = sizeof(sin1);

    bzero(&sin1, sizeof(sin1));
    s1 = accept(s, (struct sockaddr *) &sin1, &sinlen);
    assert(s1 >= 0);
    setsockopt(s1, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    
    pthread_t tcpth;
    ret = pthread_create(&tcpth, 0, tcpgo, (void *)s1);
    assert(ret == 0);
    pthread_detach(tcpth);
  }

//  if (design == 1 || design == 2) {
//    stop_hash_table_servers(hash_table);
//  } 
//  destroy_hash_table(hash_table);
}

// serve a client tcp socket, in a dedicated thread
void * tcpgo(void *xarg)
{
  size_t r;
  int s = (long)xarg;
  printf("Client connected\n");
  
  FILE *fin = fdopen(s, "r");
  FILE *fout = fdopen(s, "w");
  if (fin == NULL || fout == NULL) {
    perror("fdopen");
    return NULL;
  }
    
  int cid = create_hash_table_client(hash_table);
  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  void **values = (void **)memalign(CACHELINE, batch_size * sizeof(void *));

  while (1) {
    int nqueries;
    r = fread(&nqueries, sizeof(int), 1, fin);
    if (feof(fin)) break;

    assert(r == 1);
    assert(nqueries <= batch_size);

    r = fread(queries, sizeof(struct hash_query), nqueries, fin);
    assert(r == nqueries);
  
    if (design == 2) {
      smp_hash_doall(hash_table, cid, nqueries, queries, values);
    } else if (design == 3) {
      for (int k = 0; k < nqueries; k++) {
        if (queries[k].optype == OPTYPE_LOOKUP) {
          values[k] = locking_hash_lookup(hash_table, queries[k].key);
        } else {
          values[k] = locking_hash_insert(hash_table, queries[k].key, queries[k].size);
        }
      }
    }

    for (int k = 0; k < nqueries; k++) {
      if (queries[k].optype == OPTYPE_LOOKUP) {
        int size = (values[k] == NULL) ? 0 : queries[k].size;
        r = fwrite(&size, sizeof(int), 1, fout);
        assert(r == 1);
        if (size != 0) {
          r = fwrite(values[k], 1, size, fout);
          assert(r == size);
          localmem_release(values[k], 1);     
        } 
      } else if (queries[k].optype == OPTYPE_INSERT) {
        assert(values[k] != NULL);
        r = fread(values[k], 1, queries[k].size, fin);
        assert(r == queries[k].size);
        localmem_mark_ready(values[k]);
      } else {
        assert(0);
      }
    }
    fflush(fout);
  }

  free(queries);
  free(values);
  fclose(fin);
  fclose(fout);
  close(s);
  return NULL;
}
