#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libmemcached/memcached.h>

#include "hashclient.h"
#include "smphashtable.h"
#include "util.h"

int design          = 1;
int nservers        = 1;
int nclients        = 1;
int batch_size      = 1000;
int niters          = 100000;
int query_mask      = 0xFFFFF;
int first_core      = 0;
int first_port      = 11212;
int write_threshold = (0.3f * (double)RAND_MAX);
char serverip[100]  = "127.0.0.1";

int iters_per_client; 
memcached_server_st *servers = NULL;
int total_nlookup;
int total_nhit;

struct client_data {
  unsigned int seed;
} __attribute__ ((aligned (CACHELINE)));
struct client_data *cdata;

void run_benchmark();
void get_random_query(int client_id, struct hash_query *query);
void * client(void *xargs);
void * client_multiget(void *xargs);
void * client_fastmultiget(void *xargs);

int main(int argc, char *argv[])
{
  int opt_char;

  while((opt_char = getopt(argc, argv, "s:n:c:b:i:m:w:f:d:p:")) != -1) {
    switch (opt_char) {
      case 's':
        if (strlen(optarg) < 100) {
          strcpy(serverip, optarg);
        } else {
          printf("server ip address is too long\n");
          exit(-1);
        }
        break;
      case 'n':
        nservers = atoi(optarg);
        break;
      case 'c':
        nclients = atoi(optarg);
        break;
      case 'b':
        batch_size = atoi(optarg);
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
      case 'f':
        first_core = atoi(optarg);
        break;
      case 'd':
        design = atoi(optarg);
        break;
      case 'p':
        first_port = atoi(optarg);
        break;
      default:
        printf("benchmark options are: \n"
               "   -s server ip address\n"
               "   -c number of clients\n"
               "   -b batch size \n"
               "   -i number of iterations\n"
               "   -m log of max hash key\n"
               "   -w hash insert ratio over total number of queries\n"
               "   -f first core to run first client\n"
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
  srand(19890811 + (int)getpid());

  cdata = malloc(nclients * sizeof(struct client_data));
  for (int i = 0; i < nclients; i++) {
    cdata[i].seed = rand();
  }
 
  // add servers
  memcached_return rc;
  for (int i = 0; i < nservers; i++) {
    servers = memcached_server_list_append(servers, serverip, first_port + i, &rc);
  }

  printf("Benchmark starting..., pid: %d\n", (int)getpid()); 
  // start the clients
  double tstart = now();
  total_nhit = 0;
  total_nlookup = 0;

  int r;
  pthread_t *cthreads = (pthread_t *)malloc(nclients * sizeof(pthread_t));
  int *thread_id = (int *)malloc(nclients * sizeof(pthread_t));
  for (int i = 0; i < nclients; i++) {
    thread_id[i] = i;
    r = pthread_create(&cthreads[i], NULL, 
        (design == 1) ? client : 
        (design == 2) ? client_multiget :
                        client_fastmultiget, 
        &thread_id[i]);
    assert(r == 0);
  }

  void *value;
  for (int i = 0; i < nclients; i++) {
    r = pthread_join(cthreads[i], &value);
    assert(r == 0);
  }

  double tend = now();

  printf("Benchmark Done. Total time: %.3f, Iterations: %d\n", 
      tend - tstart, niters);
  printf("nhit: %d, nlookup: %d, nhit / nlookup: %.3f\n", total_nhit, total_nlookup, (double)total_nhit / total_nlookup);

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
  query->key = (r + 256) & query_mask;
  query->size = 8;
}

void * client(void *xargs)
{
  int c = *(int *)xargs;
  //set_affinity(c + first_core);
  
  memcached_return rc;
  memcached_st *memc; 
  
  memc = memcached_create(NULL);
  memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, (uint64_t) 1);
  rc = memcached_server_push(memc, servers);

  if (rc != MEMCACHED_SUCCESS) {
    printf("Client cid: %d couldn't add server: %s\n", c, memcached_strerror(memc, rc));
    exit(1);
  }

  int nhit = 0;
  int nlookup = 0;
  struct hash_query query;
  for (int i = 0; i < iters_per_client; i++) {
    get_random_query(c, &query);
 
    if (query.optype == OPTYPE_INSERT) {
      rc = memcached_set(memc, (char *)&query.key, sizeof(long), (char *)&query.key, sizeof(long), (time_t)0, (uint32_t)0);
      if (rc != MEMCACHED_SUCCESS) {
        printf("Client cid: %d fail: %s\n", c, memcached_strerror(memc, rc));
        return NULL;
      }
    } else{
      size_t size;
      uint32_t flags;
      char *value = memcached_get(memc, (char *)&query.key, sizeof(long), &size, &flags, &rc);
      if (rc != MEMCACHED_SUCCESS && rc != MEMCACHED_NOTFOUND) {
        printf("Client cid: %d fail: %s\n", c, memcached_strerror(memc, rc));
        return NULL;
      } else {
        nlookup++;
        if (rc != MEMCACHED_NOTFOUND) {
          nhit++;
          assert(size == 8);
          assert(value != NULL);
          if (memcmp(value, &query.key, sizeof(long)) != 0) {
            printf("ERROR: invalid value %s, should be %ld\n", value, query.key);
          }
          free(value);
        }
      }
    }
  }
  __sync_fetch_and_add(&total_nhit, nhit);
  __sync_fetch_and_add(&total_nlookup, nlookup);

  memcached_free(memc);
  return NULL;
}

void * client_multiget(void *xargs)
{
  int c = *(int *)xargs;
  set_affinity(c + first_core);
  
  memcached_return rc;
  memcached_st *memc; 
  
  memc = memcached_create(NULL);
  memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, (uint64_t) 1);
  rc = memcached_server_push(memc, servers);

  if (rc != MEMCACHED_SUCCESS) {
    printf("Client cid: %d couldn't add server: %s\n", c, memcached_strerror(memc, rc));
    exit(1);
  }

  int nhit = 0;
  int nlookup = 0;

  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  char **keys = (char **)memalign(CACHELINE, batch_size * sizeof(char *));
  size_t *lens = (size_t *)memalign(CACHELINE, batch_size * sizeof(size_t));
  int i = 0;
  while (i < iters_per_client) {
    int nqueries = min(iters_per_client - i, batch_size);
    for (int k = 0; k < nqueries; k++) {
      get_random_query(c, &queries[k]);
      keys[k] = (char *)&queries[k].key;
      lens[k] = sizeof(long);
    }
    i += nqueries;
    nlookup += nqueries;

    rc = memcached_mget(memc, (const char * const *)keys, lens, nqueries);
    if (rc != MEMCACHED_SUCCESS) {
      printf("Client cid: %d mget fail: %s\n", c, memcached_strerror(memc, rc));
      return NULL;
    }

    char key[MEMCACHED_MAX_KEY];
    size_t key_length;
    size_t size;
    uint32_t flags;      
    char *value;
    while ((value = memcached_fetch(memc, key, &key_length, &size, &flags, &rc)) != NULL) {
      if (rc != MEMCACHED_SUCCESS) {
        printf("Client cid: %d fetch fail: %s\n", c, memcached_strerror(memc, rc));
      }
      nhit++;
      assert(key_length == sizeof(long));
      assert(size == sizeof(long));
      assert(value != NULL);      
      if (memcmp(value, key, key_length) != 0) {        
        printf("ERROR: invalid value\n");
      }
      free(value);
    }
  }
  __sync_fetch_and_add(&total_nhit, nhit);
  __sync_fetch_and_add(&total_nlookup, nlookup);

  memcached_free(memc);
  return NULL;
}

void * client_fastmultiget(void *xargs)
{
  int c = *(int *)xargs;
  set_affinity(c + first_core);
  
  memcached_return rc;
  memcached_st *memc; 
  
  memc = memcached_create(NULL);
  memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, (uint64_t) 1);
  rc = memcached_server_push(memc, servers);

  if (rc != MEMCACHED_SUCCESS) {
    printf("Client cid: %d couldn't add server: %s\n", c, memcached_strerror(memc, rc));
    exit(1);
  }

  int nlookup = 0;

  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  char **keys = (char **)memalign(CACHELINE, batch_size * sizeof(char *));
  size_t *lens = (size_t *)memalign(CACHELINE, batch_size * sizeof(size_t));
  int i = 0;

  while (i < iters_per_client) {
    int nqueries = min(iters_per_client - i, batch_size);
    i += nqueries;
    nlookup += nqueries;
    for (int k = 0; k < nqueries; k++) {
      get_random_query(c, &queries[k]);
      keys[k] = (char *)&queries[k].key;
      lens[k] = sizeof(long);
    }

    rc = memcached_mget(memc, (const char * const *)keys, lens, nqueries);
    if (rc != MEMCACHED_SUCCESS) {
      printf("Client cid: %d mget fail: %s\n", c, memcached_strerror(memc, rc));
      return NULL;
    }
  }

  memcached_free(memc);
  return NULL;
}

