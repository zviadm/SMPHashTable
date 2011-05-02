#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libmemcached/memcached.h>

#include "hashclient.h"
#include "hashprotocol.h"
#include "smphashtable.h"
#include "util.h"

#define MAX_VALUE_SIZE  2048
#define MAX_BATCH_SIZE  1024

int design          = 1;
int nclients        = 1;
int nservers        = 1;
int batch_size      = 1000;
int niters          = 100000;
int query_mask      = 0xFFFFF;
int min_val_length  = 8;
int max_val_length  = 8;
int first_core      = -1;
int end_core        = -1;
double write_threshold = 0.3f;
char serverip[100]  = "127.0.0.1";
int port            = 2117;

int iters_per_client; 
memcached_server_st *servers = NULL;
int total_nlookup;
int total_nhit;

struct client_data {
  unsigned int seed;
} __attribute__ ((aligned (CACHELINE)));
struct client_data *cdata;

// Forward declarations
void run_benchmark();
void get_random_query(int client_id, double write_threshold, size_t minval, size_t maxval,
    enum optype *optype, uint64_t *key, size_t *size, char *value);
void * client_hashserver2(void *xargs);
void * client_memcached(void *xargs);
void * client_memcached_mget(void *xargs);

// Buffer to store values
struct value_buffer {
  char data[MAX_BATCH_SIZE][MAX_VALUE_SIZE];
};
struct value_buffer *valuesbuf;

int main(int argc, char *argv[])
{
  int opt_char;

  while((opt_char = getopt(argc, argv, "s:p:c:n:i:m:l:w:b:f:d:")) != -1) {
    switch (opt_char) {
      case 's':
        if (strlen(optarg) < 100) {
          strcpy(serverip, optarg);
        } else {
          printf("server ip address is too long\n");
          exit(-1);
        }
        break;
      case 'p':
        port = atoi(optarg);
        break;
      case 'c':
        nclients = atoi(optarg);
        break;
      case 'n':
        nservers = atoi(optarg);
        break;
      case 'i':
        niters = atoi(optarg);
        break;
      case 'm':
        query_mask = (1 << atoi(optarg)) - 1;
        break;
      case 'l':
        sscanf(optarg, "%d/%d", &min_val_length, &max_val_length);
        break;
      case 'w':
        write_threshold = atof(optarg);
        break;
      case 'b':
        batch_size = atoi(optarg);
        break;
      case 'f':
        sscanf(optarg, "%d/%d", &first_core, &end_core);
        break;
      case 'd':
        design = atoi(optarg);
        break;
      default:
        printf("benchmark options are: \n"
               "   -s server ip address\n"
               "   -p port\n"
               "   -c number of clients\n"
               "   -n number of servers (for memcached)\n"
               "   -i number of iterations\n"
               "   -m log of max hash key\n"
               "   -l min_val_length/max_val_length\n"
               "   -w hash insert ratio over total number of queries\n"
               "   -b batch size \n"
               "   -f start_core/end_core -- fix to cores [start_core .. end_core]\n"
               "   -d design -- 1 - key/value store, 2 - memcached\n"
               );
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

  // initialize randomness
  cdata = malloc(nclients * sizeof(struct client_data));
  for (int i = 0; i < nclients; i++) {
    cdata[i].seed = rand();
  }

  // initialize value buffers
  if ((design == 1 || design == 2) && write_threshold > 0.0) {
    valuesbuf = (struct value_buffer *)memalign(CACHELINE, nclients * sizeof(struct value_buffer));
  }

  void * client_func = NULL;
  // initialize clients
  switch (design) {
    case 1:
      client_func = client_hashserver2;
      break;
    case 2: 
      {      
      // add servers
      memcached_return rc;
      for (int i = 0; i < nservers; i++) {
        servers = memcached_server_list_append(servers, serverip, port + i, &rc);
      }
      client_func = (write_threshold == 0.0 && batch_size > 1) ? client_memcached_mget : client_memcached;
      break;
      }
    default:
      assert(0);
      break;
  }
  total_nhit = 0;
  total_nlookup = 0;

  printf("Benchmark starting..., pid: %d\n", (int)getpid()); 
  // start the clients
  double tstart = now();

  int r;
  pthread_t *cthreads = (pthread_t *)malloc(nclients * sizeof(pthread_t));
  int *thread_id = (int *)malloc(nclients * sizeof(pthread_t));
  for (int i = 0; i < nclients; i++) {
    thread_id[i] = i;
    r = pthread_create(&cthreads[i], NULL, client_func, (void *) &thread_id[i]);
    assert(r == 0);
  }

  void *value;
  for (int i = 0; i < nclients; i++) {
    r = pthread_join(cthreads[i], &value);
    assert(r == 0);
  }

  double tend = now();

  printf("Benchmark Done. Total time: %.3f, Iterations: %d\n", tend - tstart, niters);
  printf("nhit: %d, nlookup: %d, nhit / nlookup: %.3f\n", 
      total_nhit, total_nlookup, (double)total_nhit / total_nlookup);

  free(thread_id);
  free(cthreads);
  free(cdata);
}

void get_random_query(int client_id, double write_threshold, size_t minval, size_t maxval,
    enum optype *optype, uint64_t *key, size_t *size, char *value)
{
  *optype = (rand_r(&cdata[client_id].seed) < write_threshold * RAND_MAX) ? OPTYPE_INSERT : OPTYPE_LOOKUP; 
  *key = rand_r(&cdata[client_id].seed) & query_mask;
  if (*optype == OPTYPE_INSERT) {
    *size = ((rand_r(&cdata[client_id].seed) % (maxval - minval + 1)) + minval) & 0xFFFFFFF8;
    assert(*size > 0);
    assert((*size) % sizeof(uint64_t) == 0);

    uint64_t *val = (uint64_t *)value;
    val[0] = *key;
    val[((*size) >> 3) - 1] = *key;
  }
}

void * client_hashserver2(void *xargs)
{
  int c = *(int *)xargs;
  if (first_core != -1) set_affinity(first_core + c % (end_core - first_core + 1));
  
  hashconn_t conn;
  if (openconn(&conn, serverip, port) < 0) {
    printf("failed to connect to server, %d\n", c);
    return NULL;
  }

  struct client_query *queries = (struct client_query *)memalign(CACHELINE, batch_size * sizeof(struct client_query));
  uint64_t val[MAX_VALUE_SIZE >> 3];
  int nhit = 0;
  int nlookup = 0;

  int i = 0;
  while (i < iters_per_client) {
    int nqueries = min(iters_per_client - i, batch_size);
    for (int k = 0; k < nqueries; k++) {
      queries[k].value = valuesbuf[c].data[k];
      get_random_query(c, write_threshold, min_val_length, max_val_length, 
          &queries[k].optype, &queries[k].key, &queries[k].size, (char *)queries[k].value);
    }

    int r = sendqueries(conn, nqueries, queries);
    assert(r == 1);
 
    for (int k = 0; k < nqueries; k++) {
      if (queries[k].optype == OPTYPE_LOOKUP) {
        nlookup++;
        int size = readvalue(conn, (char *)val);
        assert(size >= 0);
        if (size > 0) {
          nhit++;
          assert((size % sizeof(uint64_t)) == 0);
          if ((val[0] != queries[k].key) || (val[(size >> 3) - 1] != queries[k].key)) {
            printf("ERROR: on iter %d, invalid value %ld/%ld, should be %ld\n", 
                i + k, val[0], val[(size >> 3) - 1], queries[k].key);
          }
        }
      }
    }
    i += nqueries;
  }
  __sync_fetch_and_add(&total_nhit, nhit);
  __sync_fetch_and_add(&total_nlookup, nlookup);

  free(queries);
  closeconn(conn);      
  return NULL;
}

void * client_memcached(void *xargs)
{
  int c = *(int *)xargs;
  if (first_core != -1) set_affinity(first_core + c % (end_core - first_core + 1));
  
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
  for (int i = 0; i < iters_per_client; i++) {
    enum optype optype;
    uint64_t key;
    size_t size;
    char val[MAX_VALUE_SIZE];
    get_random_query(c, write_threshold, min_val_length, max_val_length, 
        &optype, &key, &size, val);

    if (optype == OPTYPE_INSERT) {
      rc = memcached_set(memc, (char *)&key, sizeof(uint64_t), val, size, (time_t)0, (uint32_t)0);
      if (rc != MEMCACHED_SUCCESS) {
        printf("Client cid: %d fail: %s\n", c, memcached_strerror(memc, rc));
        return NULL;
      }
    } else{
      uint32_t flags;
      char *value = memcached_get(memc, (char *)&key, sizeof(uint64_t), &size, &flags, &rc);
      if (rc != MEMCACHED_SUCCESS && rc != MEMCACHED_NOTFOUND) {
        printf("Client cid: %d fail: %s\n", c, memcached_strerror(memc, rc));
        return NULL;
      } else {
        nlookup++;
        if (rc != MEMCACHED_NOTFOUND) {
          nhit++;
          assert((size % sizeof(uint64_t)) == 0);
          assert(value != NULL);
          if (memcmp(value, &key, sizeof(uint64_t)) != 0 ||
              memcmp(value + size - sizeof(uint64_t), &key, sizeof(uint64_t) != 0)) {
            printf("ERROR: on iter %d, invalid value, should be %ld\n", i, key);
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

void * client_memcached_mget(void *xargs)
{
  int c = *(int *)xargs;
  if (first_core != -1) set_affinity(first_core + c % (end_core - first_core + 1));
  
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

  uint64_t keysbuf[MAX_BATCH_SIZE];
  char *keys[MAX_BATCH_SIZE];
  size_t lens[MAX_BATCH_SIZE];
  int i = 0;
  while (i < iters_per_client) {
    int nqueries = min(iters_per_client - i, batch_size);
    for (int k = 0; k < nqueries; k++) {
      enum optype optype;
      get_random_query(c, 0.0, 0, 0, &optype, &keysbuf[k], NULL, NULL);
      lens[k] = sizeof(uint64_t);
      keys[k] = (char *)&keysbuf[k];
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
      assert(key_length == sizeof(uint64_t));
      assert((size % sizeof(uint64_t)) == 0);
      assert(value != NULL);      
      if (memcmp(value, key, sizeof(uint64_t)) != 0 ||
          memcmp(value + size - sizeof(uint64_t), key, sizeof(uint64_t) != 0)) {
        printf("ERROR: on iter %d, invalid value\n", i);
      }
      free(value);
    }
  }
  __sync_fetch_and_add(&total_nhit, nhit);
  __sync_fetch_and_add(&total_nlookup, nlookup);

  memcached_free(memc);
  return NULL;
}
