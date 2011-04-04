#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ia32perf.h"
#include "smphashtable.h"
#include "util.h"

int nservers        = 1;
int nclients        = 1;
int niters          = 1000000;
int size            = 1000000;
int query_mask      = 0xFFFFF;
int write_threshold = (0.3f * (double)RAND_MAX);
int design          = 1;
int first_core      = 1;
int batch_size      = 10;
struct hash_table *hash_table;
char rand_data[10000] = { 0 };
int iters_per_client; 

uint64_t pmccount[100];
uint64_t pmclast[100];

struct client_data {
  unsigned int seed;
} __attribute__ ((aligned (CACHELINE)));
struct client_data *cdata;

void run_benchmark();
void get_random_query(int client_id, struct hash_query *query);
void * client_design1(void *args);
void * client_design2(void *args);
void * client_design3(void *args);

int main(int argc, char *argv[])
{
  int opt_char;

  /**
   * Hash Table Benchmark Options
   * @s: number of servers/partitions
   * @c: number of clients
   * @i: number of iterations
   * @n: number of elements in cache
   * @m: log of query mask
   * @w: hash inserts / iterations
   * @d: hash design to use
   *     1 - Server/Client no buffering
   *     2 - Server/Cleint with buffering
   *     3 - Naive algorithm
   * @f: first server core number 
   *
   */
  while((opt_char = getopt(argc, argv, "s:c:i:n:m:w:d:f:b:")) != -1) {
    switch (opt_char) {
      case 's':
        nservers = atoi(optarg);
        break;
      case 'c':
        nclients = atoi(optarg);
        break;
      case 'i':
        niters = atoi(optarg);
        break;
      case 'n':
        size = atol(optarg);
        break;
      case 'm':
        query_mask = (1 << atoi(optarg)) - 1;
        break;
      case 'w':
        write_threshold = (int)(atof(optarg) * (double)RAND_MAX);
        break;
      case 'd':
        design = atoi(optarg);
        break;
      case 'f':
        first_core = atoi(optarg);
        break;
      case 'b':
        batch_size = atoi(optarg);
        break;
      default:
        printf("usage\n");
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

  hash_table = create_hash_table(size, nservers);
  cdata = malloc(nclients * sizeof(struct client_data));
  for (int i = 0; i < nclients; i++) {
    cdata[i].seed = rand();
  }
 
  printf("Benchmark starting...\n"); 
  // start the clients
  double tstart = now();

  for (int i = 0; i < 6; i++) {
    // Event Select values for each pmc
    // Counter Mask (8 bits) - INV - EN - ANY - INT - PC - E - OS - USR - UMASK (8 bits) - Event Select (8 bits)
    if (StartCounter(i, 0, 0x00610224) != 0) {
      //printf("Failed to start counter on cpu %d, make sure you have run \"modprobe msr\"" 
      //       " and are running benchmark with sudo privileges\n", i);
    }
    ReadCounter(i, 0, &pmclast[i]);
  }

  if (design == 1 || design == 2) {
    start_hash_table_servers(hash_table, first_core);
  }

  int r;
  pthread_t *cthreads = (pthread_t *)malloc(nclients * sizeof(pthread_t));
  int *thread_id = (int *)malloc(nclients * sizeof(pthread_t));
  for (int i = 0; i < nclients; i++) {
    thread_id[i] = i;
    r = pthread_create(&cthreads[i], NULL, 
        (design == 1) ? client_design1 : 
        (design == 2) ? client_design2 : client_design3, 
        (void *) &thread_id[i]);
    assert(r == 0);
  }

  void *value;
  for (int i = 0; i < nclients; i++) {
    r = pthread_join(cthreads[i], &value);
    assert(r == 0);
  }

  if (design == 1 || design == 2) {
    stop_hash_table_servers(hash_table);
  }

  double totalpmc = 0;
  for (int i = 0; i < 6; i++) {
    uint64_t tmp;
    ReadCounter(i, 0, &tmp);
    totalpmc += tmp - pmclast[i];
  }

  double tend = now();

  // print out all the important information
  printf("Benchmark Done. Design %d - Total time: %.3f, Iterations: %d\n", 
      design, tend - tstart, niters);
  printf("nservers: %d, nclients: %d, partition overhead: %zu(bytes), nhits / niters: %.3f\n", 
      nservers, nclients, stats_get_overhead(hash_table) / nservers, (double)stats_get_nhits(hash_table) / niters);
  printf("L2 Misses per iteration: %.3f\n", totalpmc / niters);

  free(thread_id);
  free(cthreads);
  free(cdata);
  destroy_hash_table(hash_table);
}

void get_random_query(int client_id, struct hash_query *query)
{
  unsigned long optype = 
    (rand_r(&cdata[client_id].seed) < write_threshold) ? 1 : 0; 

  unsigned long r1 = rand_r(&cdata[client_id].seed);
  unsigned long r2 = rand_r(&cdata[client_id].seed);
  unsigned long r = ((r1 << 16) + r2);

  query->optype = optype;
  query->key = r & query_mask;
  if (optype == 1) {
    unsigned long v1 = rand_r(&cdata[client_id].seed);
    unsigned long v2 = rand_r(&cdata[client_id].seed);
    unsigned long v = ((v1 << 16) + v2);
    query->value = (void *)v;
  }
}

void * client_design1(void *args)
{
  int c = *(int *)args;
  set_affinity(c);
  
  int cid = create_hash_table_client(hash_table);

  struct hash_query query;
  void *value;
  for (int i = 0; i < iters_per_client; i++) {
    // generate random query
    get_random_query(c, &query);
    if (query.optype == 0) {
      value = smp_hash_lookup(hash_table, cid, query.key);
    } else {
      smp_hash_insert(hash_table, cid, query.key, query.value);
    }
  }
  return NULL;
}

void * client_design2(void *args)
{
  int c = *(int *)args;
  set_affinity(c);
  
  int cid = create_hash_table_client(hash_table);

  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  void **values = (void **)memalign(CACHELINE, batch_size * sizeof(void *));
  int i = 0;
  while (i < iters_per_client) {
    int nqueries = min(iters_per_client - i, batch_size);
    for (int k = 0; k < nqueries; k++) {
      get_random_query(c, &queries[k]);
    }
    smp_hash_doall(hash_table, cid, nqueries, queries, values);
    i += nqueries;
  }

  free(queries);
  free(values);
  return NULL;
}

void * client_design3(void *args)
{
  int c = *(int *)args;
  set_affinity(c);
  
  struct hash_query query;
  void *value = NULL;
  for (int i = 0; i < iters_per_client; i++) {
    // generate random query
    get_random_query(c, &query);
    if (query.optype == 0) {
      value = locking_hash_lookup(hash_table, query.key);
    } else {
      locking_hash_insert(hash_table, query.key, query.value);
    }
  }
  return NULL;
}

