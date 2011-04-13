#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xmmintrin.h>

#include "hashclient.h"
#include "smphashtable.h"
#include "util.h"

int nclients        = 1;
int batch_size      = 1000;
int niters          = 100000;
int query_mask      = 0xFFFFF;
int write_threshold = (0.3f * (double)RAND_MAX);
char serverip[100]  = "127.0.0.1";

struct hash_table *hash_table;
long rand_data[10000] = { 0 };
int iters_per_client; 

struct client_data {
  unsigned int seed;
} __attribute__ ((aligned (CACHELINE)));
struct client_data *cdata;

void run_benchmark();
void get_random_query(int client_id, struct hash_query *query);
void * client(void *args);

int main(int argc, char *argv[])
{
  int opt_char;

  while((opt_char = getopt(argc, argv, "s:c:i:m:w:b:")) != -1) {
    switch (opt_char) {
      case 's':
        if (strlen(optarg) < 100) {
          strcpy(serverip, optarg);
        } else {
          printf("server ip address is too long\n");
          exit(-1);
        }
        break;
      case 'c':
        nclients = atoi(optarg);
        break;
      case 'i':
        niters = atoi(optarg);
        break;
      case 'm':
        query_mask = (1 << atoi(optarg)) - 1;
        break;
      case 'w':
        write_threshold = (int)(atof(optarg) * (double)RAND_MAX);
        break;
      case 'b':
        batch_size = atoi(optarg);
        break;
      default:
        printf("benchmark options are: \n"
               "   -s server ip address\n"
               "   -c number of clients\n"
               "   -b batch size \n"
               "   -i number of iterations\n"
               "   -m log of max hash key\n"
               "   -w hash insert ratio over total number of queries\n"
               "example './benchmarkhashserver -c 3 -b 1000 -i 100000000 -m 15 -w 0.3'\n");
        exit(-1);
    }
  }

  iters_per_client = niters / nclients;
  run_benchmark();
  return 0;
}

void run_benchmark() 
{
  srand(19890811);

  cdata = malloc(nclients * sizeof(struct client_data));
  for (int i = 0; i < nclients; i++) {
    cdata[i].seed = rand();
  }
 
  for (int i = 0; i < 10000; i++) {
    rand_data[i] = i;
  }

  printf("Benchmark starting...\n"); 
  // start the clients
  double tstart = now();

  int r;
  pthread_t *cthreads = (pthread_t *)malloc(nclients * sizeof(pthread_t));
  int *thread_id = (int *)malloc(nclients * sizeof(pthread_t));
  for (int i = 0; i < nclients; i++) {
    thread_id[i] = i;
    r = pthread_create(&cthreads[i], NULL, client, (void *) &thread_id[i]);
    assert(r == 0);
  }

  void *value;
  for (int i = 0; i < nclients; i++) {
    r = pthread_join(cthreads[i], &value);
    assert(r == 0);
  }

  double tend = now();

  // print out all the important information
  printf("Benchmark Done. Total time: %.3f, Iterations: %d\n", 
      tend - tstart, niters);

  free(thread_id);
  free(cthreads);
  free(cdata);
}

void get_random_query(int client_id, struct hash_query *query)
{
  enum optype optype = 
    (rand_r(&cdata[client_id].seed) < write_threshold) ? OPTYPE_INSERT : OPTYPE_LOOKUP; 

  unsigned long r1 = rand_r(&cdata[client_id].seed);
  unsigned long r2 = rand_r(&cdata[client_id].seed);
  unsigned long r = ((r1 << 16) + r2);

  query->optype = optype;
  query->key = r & query_mask;
  query->size = 8;
}

void * client(void *args)
{
  int c = *(int *)args;
  //set_affinity(c);
  
  hashconn_t conn;
  if (openconn(&conn, serverip) < 0) {
    printf("failed to connect to server\n");
    return NULL;
  }

  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  void **values = (void **)memalign(CACHELINE, batch_size * sizeof(void *));
  int i = 0;
  while (i < iters_per_client) {
    int nqueries = min(iters_per_client - i, batch_size);
    for (int k = 0; k < nqueries; k++) {
      get_random_query(c, &queries[k]);
      if (queries[k].optype == OPTYPE_INSERT) {
        values[k] = &queries[k].key;
      } else{
        values[k] = NULL;
      }
    }

    sendqueries(conn, nqueries, queries, values);
 
    for (int k = 0; k < nqueries; k++) {
      if (queries[k].optype == OPTYPE_LOOKUP) {
        long val;
        int size = readvalue(conn, &queries[k], &val);
        if (size != 0) {
          assert(size == 8);
          assert(val == queries[k].key);
        }
      }
    }
    i += nqueries;
  }

  free(queries);
  free(values);
  closeconn(conn);      
  return NULL;
}
