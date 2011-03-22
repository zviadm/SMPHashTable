#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "smphashtable.h"

#define CACHELINE   64
#define MAX_CLIENTS 50
#define BUCKET_LOAD 64

/**
 * Hash Table Storage Data Structures
 * struct elem       - element in table
 * struct bucket     - a bucket in a partition
 * struct partition  - hash table partition for server
 */
struct elem {
  hash_key key;
  struct hash_value *value;
  TAILQ_ENTRY(elem) chain;
  TAILQ_ENTRY(elem) lru;
} __attribute__ ((aligned (CACHELINE)));

struct bucket {
  TAILQ_HEAD(elist, elem) chain;
} __attribute__ ((aligned (CACHELINE)));

struct partition {
  struct bucket *table;
  TAILQ_HEAD(lrulist, elem) lru;
  int nhash;
  int size;
  int nhit;
} __attribute__ ((aligned (CACHELINE)));

/**
 * Server/Client Message Passing Data Structures
 */
struct box {
  //struct twowaybuffer buffer;
  int buffer;
}  __attribute__ ((aligned (CACHELINE)));

struct box_array {
  struct box *boxes;
} __attribute__ ((aligned (CACHELINE)));

struct thread_args {
  int id;
  int core;
  struct hash_table *hash_table;
};

struct hash_table {
  int nservers;
  int nclients;
  size_t partition_max_size;

  int quitting;
  pthread_t *threads;
  struct thread_args *thread_data;

  struct partition *partitions __attribute__ ((aligned (CACHELINE)));
  struct box_array boxes[MAX_CLIENTS] __attribute__ ((aligned (CACHELINE)));
};

// Forward declaration of functions
void init_hash_table(struct hash_table *hash_table);
void *hash_table_server(void* args);

struct hash_table *create_hash_table(size_t max_size, int nservers) 
{
  struct hash_table *hash_table = (struct hash_table *)malloc(sizeof(struct hash_table));
  hash_table->nservers = nservers;
  hash_table->nclients = 0;

  hash_table->partition_max_size = max_size / nservers;
  init_hash_table(hash_table);  

  hash_table->threads = (pthread_t *)malloc(nservers * sizeof(pthread_t));
  hash_table->thread_data = (struct thread_args *)malloc(nservers * sizeof(struct thread_args));
  return hash_table;
}

void init_hash_table(struct hash_table *hash_table)
{
  //for (i = 0; i < NRANGE; i++) {
  //  map[i] = i % npartition;
  //}

  hash_table->partitions = memalign(CACHELINE, hash_table->nservers * sizeof(struct partition));
  for (int i = 0; i < hash_table->nservers; i++) {
    hash_table->partitions[i].nhash = hash_table->partition_max_size / BUCKET_LOAD;
    hash_table->partitions[i].nhit = 0;
    hash_table->partitions[i].size = 0;

    hash_table->partitions[i].table = memalign(CACHELINE, hash_table->partitions[i].nhash * sizeof(struct bucket));
    assert((unsigned long) &(hash_table->partitions[i].table[0]) % CACHELINE == 0);
    for (int j = 0; j < hash_table->partitions[i].nhash; j++) {
      TAILQ_INIT(&(hash_table->partitions[i].table[j].chain));
    }
    TAILQ_INIT(&hash_table->partitions[i].lru);
  }

  assert((unsigned long) &hash_table->partitions[0] % CACHELINE == 0);
  assert((unsigned long) &hash_table->partitions[1] % CACHELINE == 0);
}

void destroy_hash_table(struct hash_table *hash_table)
{
  for (int i = 0; i < hash_table->nservers; i++) {
    free(hash_table->partitions[i].table);
  }
  free(hash_table->partitions);

  for (int i = 0; i < hash_table->nclients; i++) {
    free(hash_table->boxes[i].boxes);
  }

  free(hash_table->threads);
  free(hash_table->thread_data);
  free(hash_table);
}

void start_hash_table_servers(struct hash_table *hash_table, int first_core) 
{
  int r;
  hash_table->quitting = 0;
  for (int i = 0; i < hash_table->nservers; i++) {
    hash_table->thread_data[i].id = i;
    hash_table->thread_data[i].core = first_core + i;
    hash_table->thread_data[i].hash_table = hash_table;

    r = pthread_create(&hash_table->threads[i], NULL, hash_table_server, (void *) (&hash_table->thread_data[i]));
    assert(r == 0);
  }
}

void stop_hash_table_servers(struct hash_table *hash_table)
{
  int r;
  void *value;
  hash_table->quitting = 1;
  for (int i = 0; i < hash_table->nservers; i++) {
    r = pthread_join(hash_table->threads[i], &value);
    assert(r == 0);
  }
}

int create_hash_table_client(struct hash_table *hash_table)
{
  int client = hash_table->nclients;
  hash_table->nclients++;
  assert(hash_table->nclients <= MAX_CLIENTS);

  hash_table->boxes[client].boxes = memalign(CACHELINE, hash_table->nservers * sizeof(struct box));
  for (int i = 0; i < hash_table->nservers; i++) {
    memset((void*)&hash_table->boxes[client].boxes[i], 0, sizeof(struct box));
  }
  
  assert((unsigned long) &hash_table->boxes[client] % CACHELINE == 0);
  assert((unsigned long) &hash_table->boxes[client].boxes[0] % CACHELINE == 0);
  assert((unsigned long) &hash_table->boxes[client].boxes[1] % CACHELINE == 0);
  return client;
}

void *hash_table_server(void* args)
{
  return NULL;
}
