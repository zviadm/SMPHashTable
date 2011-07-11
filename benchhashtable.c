#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

//#include <google/profiler.h>

#include "ia32perf.h"
#include "partition.h"
#include "smphashtable.h"
#include "util.h"

#if defined(INTEL64)
  // Event Select values for Intel I7 core processor
  // Counter Mask (8 bits) - INV - EN - ANY - INT - PC - E - OS - USR - UMASK (8 bits) - Event Select (8 bits)
  #define NEVT 1
  uint64_t evts[NEVT] = {
    0x00410224, // L2 Misses
  };
#elif defined(AMD64)
  // Reserved (22 bits) - HO - GO - Reserved (4 bits) - Event Select (8 bits)
  // Counter Mask (8 bits) - INV - EN - ANY - INT - PC - E - OS - USR - UMASK (8 bits) - Event Select (8 bits)
  #define NEVT 2
  uint64_t evts[NEVT] = {
    0x000041077E, // L2 Misses
    0x04004107E1, // L3 Misses, needs to be ORed with (core# << (12))
  };
#else
  #define NEVT 0
#endif

int design          = 1;
int nservers        = 1;
int nclients        = 1;
int first_core      = -1;
int batch_size      = 1000;
int niters          = 100000000;
size_t size         = 6400000;
int query_mask      = (1 << 20) - 1;
int query_shift     = (31 - 20);
int write_threshold = (0.3f * (double)RAND_MAX);
int evictalgo       = EVICT_LRU;

struct hash_table *hash_table;
long rand_data[10000] = { 0 };
int iters_per_client; 

uint64_t pmccount[NEVT][MAX_SERVERS + MAX_CLIENTS];
uint64_t pmclast[NEVT][MAX_SERVERS + MAX_CLIENTS];

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

  while((opt_char = getopt(argc, argv, "s:c:f:i:n:t:m:w:d:f:b:e:")) != -1) {
    switch (opt_char) {
      case 's':
        nservers = atoi(optarg);
        break;
      case 'c':
        nclients = atoi(optarg);
        break;
      case 'f':
        first_core = atoi(optarg);
        break;
      case 'i':
        niters = atoi(optarg);
        break;
      case 't':
        size = atol(optarg);
        break;
      case 'm':
        query_shift = 31 - atoi(optarg); 
        query_mask = (1 << atoi(optarg)) - 1;
        assert(query_shift > 0);
        break;
      case 'w':
        write_threshold = (int)(atof(optarg) * (double)RAND_MAX);
        break;
      case 'd':
        design = atoi(optarg);
        break;
      case 'b':
        batch_size = atoi(optarg);
        break;
      case 'e':
        evictalgo = atoi(optarg);
  break;
      default:
        printf("benchmark options are: \n"
               "   -d design (1 = server/client, 2 = old-style server/client, 3 = locking)\n"
               "   -s number of servers / partitions\n"
               "   -c number of clients\n"
               "   -b batch size (for design 2)\n"
               "   -i number of iterations\n"
               "   -t max size of cache (in bytes)\n"
               "   -m log of max hash key\n"
               "   -w hash insert ratio over total number of queries\n"
               "   -e eviction algorithm (1 = LRU, 2 = random)\n"
               "example './benchmarkhashtable -d 2 -s 3 -c 3 -f 3 -b 1000 -i 100000000 -t 640000 -m 15 -w 0.3'\n");
        exit(-1);
    }
  }
  if (first_core == -1) first_core = nclients;

  iters_per_client = niters / nclients;
  run_benchmark();
  return 0;
}

void run_benchmark() 
{
  srand(19890811);

  printf(" Design:       %d (%s)\n", design,
      (evictalgo == EVICT_LRU)    ? "LRU" :
      (evictalgo == EVICT_RANDOM) ? "random" : "XXXXXX");
  printf(" # clients:    %d\n", nclients);
  if (design == 1 || design == 2)
    printf(" # servers:    %d\n", nservers);
  if (design == 3)
    printf(" # partitions: %d\n", nservers);
  printf(" Key range:    0..2^%d\n", 31-query_shift);
  printf(" Write ratio:  %.3f\n", (double)write_threshold / (double)RAND_MAX);
  printf(" Total memory: %ld bytes\n", size);
  printf(" Iterations:   %d\n", niters);

  hash_table = create_hash_table(size, nservers, evictalgo);
  cdata = malloc(nclients * sizeof(struct client_data));
  for (int i = 0; i < nclients; i++) {
    cdata[i].seed = rand();
  }
 
  for (int i = 0; i < 10000; i++) {
    rand_data[i] = i;
  }

  // start the clients
  //ProfilerStart("cpu.info"); 
  double tstart = now();

  for (int i = 0; i < nclients; i++) {
    for (int k = 0; k < NEVT; k++) {
      if (StartCounter(i, k, 
            (k == 1) ? (evts[k] | ((i % 6) << 12)) : 
            evts[k]) != 0) {
        printf("Failed to start counter on cpu %d, make sure you have run \"modprobe msr\"" 
            " and are running benchmark with sudo privileges\n", i);
      }
      ReadCounter(i, k, &pmclast[k][i]);
    }
  }

  if (design == 1 || design == 2) {
    for (int i = first_core; i < first_core + nservers; i++) {
      for (int k = 0; k < NEVT; k++) {
        if (StartCounter(i, k, 
              (k == 1) ? (evts[k] | ((i % 6) << 12)) : 
              evts[k]) != 0) {
          printf("Failed to start counter on cpu %d, make sure you have run \"modprobe msr\"" 
              " and are running benchmark with sudo privileges\n", i);
        }
        ReadCounter(i, k, &pmclast[k][i]);
      }
    }
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

  double clients_totalpmc[NEVT] = { 0 };
  double servers_totalpmc[NEVT] = { 0 };
  for (int i = 0; i < nclients; i++) {
    for (int k = 0; k < NEVT; k++) {
      uint64_t tmp;
      ReadCounter(i, k, &tmp);
      clients_totalpmc[k] += tmp - pmclast[k][i];
    }
  }
  if (design == 1 || design == 2) {
    for (int i = first_core; i < first_core + nservers; i++) {
      for (int k = 0; k < NEVT; k++) {
        uint64_t tmp;
        ReadCounter(i, k, &tmp);
        servers_totalpmc[k] += tmp - pmclast[k][i];
      }
    }
  }

  double tend = now();
  //ProfilerStop();

  // print out all the important information
  printf("== results ==\n");
  printf(" Total time:      %.3f\n", tend - tstart);
  printf(" Lookup hit rate: %.3f\n", (double)stats_get_nhits(hash_table) / stats_get_nlookups(hash_table));
  printf(" Servr CPU usage: %.3f\n", stats_get_cpu_usage(hash_table));
  if (NEVT > 0) {
    printf(" L2 Misses per iteration: clients - %.3f, servers - %.3f, total - %.3f\n", 
        clients_totalpmc[0] / niters, servers_totalpmc[0] / niters, (clients_totalpmc[0] + servers_totalpmc[0]) / niters);
  }
  if (NEVT > 1) {
    printf(" L3 Misses per iteration: clients - %.3f, servers - %.3f, total - %.3f\n", 
        clients_totalpmc[1] / niters, servers_totalpmc[1] / niters, (clients_totalpmc[1] + servers_totalpmc[1]) / niters);
  }

#if 0
  double avg, stddev;
  for (int i = 0; i < nservers; i++) {
    stats_get_buckets(hash_table, i, &avg, &stddev);
    printf(" Server %d Buckets, avg %.3f, stddev %.3f\n", i, avg, stddev);
  }
#endif

  free(thread_id);
  free(cthreads);
  free(cdata);
  destroy_hash_table(hash_table);
}

void get_random_query(int client_id, struct hash_query *query)
{
  enum optype optype = 
    (rand_r(&cdata[client_id].seed) < write_threshold) ? OPTYPE_INSERT : OPTYPE_LOOKUP; 
  unsigned long r = rand_r(&cdata[client_id].seed);

  query->optype = optype;
  query->key = (r >> query_shift) & query_mask;
  query->size = 0;
  if (optype == OPTYPE_INSERT) {
    query->size = 8;
  }
}

void handle_query_result(int client_id, struct hash_query *query, void *value)
{
  long *val = value;
  if (query->optype == OPTYPE_LOOKUP) {
    if (val != NULL) {
      if (val[0] != query->key) {
        printf("ERROR: values do not match: %ld, should be %ld\n", val[0], query->key);
      }

      if (design == 1 || design == 2) {
        mp_release_value(hash_table, client_id, value);
      } else {
        atomic_release_value(value);
      }
    }
  } else {
    assert(val != NULL);
    val[0] = query->key;

    if (design == 1 || design == 2) {
      mp_mark_ready(hash_table, client_id, value);
    } else {
      atomic_mark_ready(value);
    }
  }
}

void * client_design1(void *args)
{
  int r;
  int c = *(int *)args;
  set_affinity(c);
  
  int cid = create_hash_table_client(hash_table);

  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  void *value;
  int k = 0;
  int j = 0;
  for (int i = 0; i < iters_per_client; i++) {
    // generate random query
    get_random_query(c, &queries[k]);
    if (queries[k].optype == OPTYPE_LOOKUP) {
      r = smp_hash_lookup(hash_table, cid, queries[k].key);
    } else {
      r = smp_hash_insert(hash_table, cid, queries[k].key, queries[k].size);
    }
    assert(r == 1);
    k = (k + 1) % batch_size;

    if (j == 1) {
      r = smp_get_next(hash_table, cid, &value);
      assert(r == 1);
    
      handle_query_result(cid, &queries[k], value);   
    } else if (k == batch_size - 1) {
      j = 1;
    }
  }

  while ((r = smp_get_next(hash_table, cid, &value)) == 1) {
    k = (k + 1) % batch_size;
    handle_query_result(cid, &queries[k], value);
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
      values[k] = 0;
    }
    smp_hash_doall(hash_table, cid, nqueries, queries, values);

    for (int k = 0; k < nqueries; k++) {
      handle_query_result(cid, &queries[k], values[k]);
    }
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
    if (query.optype == OPTYPE_LOOKUP) {
      value = locking_hash_lookup(hash_table, query.key);
    } else {
      value = locking_hash_insert(hash_table, query.key, query.size);
    }

    handle_query_result(c, &query, value);
  }
  return NULL;
}

