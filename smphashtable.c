#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <xmmintrin.h>

#include "alock.h"
#include "onewaybuffer.h"
#include "smphashtable.h"
#include "util.h"

#define MAX_CLIENTS       50
#define BUCKET_LOAD       2
#define SERVER_READ_COUNT BUFFER_FLUSH_COUNT 
#define ELEM_OVERHEAD     (sizeof(hash_key) + sizeof(struct hash_value))
#define HASH_INSERT_MASK  0x8000000000000000 
#define INSERT_MSG_LENGTH 2

/**
 * Hash Table Storage Data Structures
 * struct elem       - element in table
 * struct bucket     - a bucket in a partition
 * struct partition  - hash table partition for server
 */
struct elem {
  hash_key key;
  void *value;
  TAILQ_ENTRY(elem) chain;
  TAILQ_ENTRY(elem) lru;
  TAILQ_ENTRY(elem) free;
};

struct bucket {
  TAILQ_HEAD(elist, elem) chain;
};

struct partition {
  struct bucket *table;
  TAILQ_HEAD(lrulist, elem) lru;
  TAILQ_HEAD(freelist, elem) free;
  int max_size;
  int nhash;
  int size;
  int nhits;
  struct elem *elems;
  struct alock lock;
} __attribute__ ((aligned (CACHELINE)));

/**
 * Server/Client Message Passing Data Structures
 */
struct box {
  struct onewaybuffer in;
  struct onewaybuffer out;
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
  volatile int nclients;
  int max_size;
  size_t overhead;

  pthread_mutex_t create_client_lock;

  volatile int quitting;
  pthread_t *threads;
  struct thread_args *thread_data;

  struct partition *partitions;
  struct box_array *boxes;
};

// Forward declaration of functions
void init_hash_partition(struct hash_table *hash_table, struct partition *p);
void destroy_hash_partition(struct partition *p);
void *hash_table_server(void* args);

struct elem * hash_lookup(struct partition *p, hash_key key);
struct elem * hash_insert(struct partition *p, hash_key key, void *value);
void hash_remove(struct partition *p, struct elem *e);
void lru(struct partition *p, struct elem *e);

/**
 * Hash Distribution functions
 */
static inline int hash_get_server(const struct hash_table *hash_table, hash_key key)
{
  return key % hash_table->nservers;
}

static inline int hash_get_bucket(const struct partition *p, hash_key key)
{
  return key % p->nhash;
}

struct hash_table *create_hash_table(int max_size, int nservers) 
{
  struct hash_table *hash_table = (struct hash_table *)malloc(sizeof(struct hash_table));
  hash_table->nservers = nservers;
  hash_table->nclients = 0;
  hash_table->max_size = max_size;

  pthread_mutex_init(&hash_table->create_client_lock, NULL);

  hash_table->overhead = 
    sizeof(struct hash_table) + nservers * sizeof(struct partition) + 
    MAX_CLIENTS * sizeof(struct box_array) + nservers * sizeof(pthread_t) +
    nservers * sizeof(struct thread_args);
  
  hash_table->partitions = memalign(CACHELINE, nservers * sizeof(struct partition));
  hash_table->boxes = memalign(CACHELINE, MAX_CLIENTS * sizeof(struct box_array));
  for (int i = 0; i < hash_table->nservers; i++) {
    init_hash_partition(hash_table, &hash_table->partitions[i]);
  }

  hash_table->threads = (pthread_t *)malloc(nservers * sizeof(pthread_t));
  hash_table->thread_data = (struct thread_args *)malloc(nservers * sizeof(struct thread_args));
  return hash_table;
}

void destroy_hash_table(struct hash_table *hash_table)
{
  for (int i = 0; i < hash_table->nservers; i++) {
    destroy_hash_partition(&hash_table->partitions[i]);
  }
  free(hash_table->partitions);

  for (int i = 0; i < hash_table->nclients; i++) {
    free(hash_table->boxes[i].boxes);
  }
  free(hash_table->boxes);

  free(hash_table->threads);
  free(hash_table->thread_data);
  pthread_mutex_destroy(&hash_table->create_client_lock); 
  free(hash_table);
}

void init_hash_partition(struct hash_table *hash_table, struct partition *p)
{
  assert((unsigned long)p % CACHELINE == 0);
  p->max_size = hash_table->max_size / hash_table->nservers;
  p->nhash = max(10, p->max_size / BUCKET_LOAD);
  p->nhits = 0;
  p->size = 0;
  hash_table->overhead += p->nhash * sizeof(struct bucket) + p->max_size * sizeof(struct elem);

  p->table = memalign(CACHELINE, p->nhash * sizeof(struct bucket));
  assert((unsigned long) &(p->table[0]) % CACHELINE == 0);
  for (int i = 0; i < p->nhash; i++) {
    TAILQ_INIT(&(p->table[i].chain));
  }

  TAILQ_INIT(&p->lru);
  TAILQ_INIT(&p->free);
  p->elems = memalign(CACHELINE, p->max_size * sizeof(struct elem));
  for (int i = 0; i < p->max_size; i++) {
    struct elem *e = &p->elems[i];
    TAILQ_INSERT_TAIL(&p->free, e, free);
  }

  anderson_init(&p->lock, hash_table->nservers);
}

void destroy_hash_partition(struct partition *p)
{
  free(p->table);
  free(p->elems); 
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
  pthread_mutex_lock(&hash_table->create_client_lock);
  int client = hash_table->nclients;
  assert(hash_table->nclients <= MAX_CLIENTS);
  
  hash_table->overhead += hash_table->nservers * sizeof(struct box);
  hash_table->boxes[client].boxes = memalign(CACHELINE, hash_table->nservers * sizeof(struct box));
  assert((unsigned long) &hash_table->boxes[client] % CACHELINE == 0);
  for (int i = 0; i < hash_table->nservers; i++) {
    memset((void*)&hash_table->boxes[client].boxes[i], 0, sizeof(struct box));
    assert((unsigned long) &hash_table->boxes[client].boxes[i].in % CACHELINE == 0);
    assert((unsigned long) &hash_table->boxes[client].boxes[i].in.rd_index % CACHELINE == 0);
    assert((unsigned long) &hash_table->boxes[client].boxes[i].in.wr_index % CACHELINE == 0);
    assert((unsigned long) &hash_table->boxes[client].boxes[i].in.tmp_wr_index % CACHELINE == 0);
    assert((unsigned long) &hash_table->boxes[client].boxes[i].out % CACHELINE == 0);
  }
  
  __sync_add_and_fetch(&hash_table->nclients, 1);
  pthread_mutex_unlock(&hash_table->create_client_lock);
  return client;
}

void *hash_table_server(void* args)
{
  int s = ((struct thread_args *) args)->id;
  int c = ((struct thread_args *) args)->core;
  struct hash_table *hash_table = ((struct thread_args *) args)->hash_table;
  struct partition *p = &hash_table->partitions[s];
  struct box_array *boxes = hash_table->boxes;
  unsigned long localbuf[ONEWAY_BUFFER_SIZE + 2];
  int quitting = 0;

  set_affinity(c);
  while (quitting == 0) {
    // after server receives quit signal it should make sure to complete all 
    // queries that are in the buffers
    quitting = hash_table->quitting;

    int nclients = hash_table->nclients;
    for (int i = 0; i < nclients; i++) {
      int count = buffer_read_all(&boxes[i].boxes[s].in, (quitting == 0) ? SERVER_READ_COUNT : ONEWAY_BUFFER_SIZE, localbuf);
      if (count == 0) continue;

      int k = 0;
      int j = 0;
      while (k < count) {
        if (localbuf[k] & HASH_INSERT_MASK) {
          if (k + INSERT_MSG_LENGTH > count) {
            // handle scenario when hash insert message gets cut off 
            int tmpcnt = 
              buffer_read_all(&boxes[i].boxes[s].in, k + INSERT_MSG_LENGTH - count, &localbuf[count]);
            assert(tmpcnt == k + INSERT_MSG_LENGTH - count);
          }
            
          hash_insert(p, localbuf[k] & (HASH_INSERT_MASK - 1), (void *)localbuf[k + 1]);
          localbuf[j] = 0; // TODO: it could return element that was evicted or something like that
          j++;
          k += INSERT_MSG_LENGTH;
        } else {
          struct elem *e = hash_lookup(p, localbuf[k]);
          if (e != NULL) {
            localbuf[j] = (unsigned long)e->value;
          } else {
            localbuf[j] = 0;
          }
          j++;
          k++;
        }
      }

      buffer_write_all(&boxes[i].boxes[s].out, j, localbuf, 1);
    }
  }
  return 0;
}

void * smp_hash_lookup(struct hash_table *hash_table, int client_id, hash_key key)
{
  unsigned long res;
  int s = hash_get_server(hash_table, key);
  buffer_write_all(&hash_table->boxes[client_id].boxes[s].in, 1, (unsigned long*)&key, 1);

  while (buffer_read_all(&hash_table->boxes[client_id].boxes[s].out, 1, &res) == 0) {
    _mm_pause();
  }
  return (void *)res;
}

void smp_hash_insert(struct hash_table *hash_table, int client_id, hash_key key, void *value)
{
  unsigned long res;
  unsigned long msg_data[INSERT_MSG_LENGTH];
  msg_data[0] = (unsigned long)key | HASH_INSERT_MASK;
  msg_data[1] = (unsigned long)value;

  int s = hash_get_server(hash_table, key);
  buffer_write_all(&hash_table->boxes[client_id].boxes[s].in, INSERT_MSG_LENGTH, msg_data, 1);
  while (buffer_read_all(&hash_table->boxes[client_id].boxes[s].out, 1, &res) == 0) {
    _mm_pause();
  }
  assert(res == 0);
}

void smp_hash_doall(struct hash_table *hash_table, int client_id, int nqueries, struct hash_query *queries, void **values)
{
  int pindex = 0;

  int *pending_count = (int*)memalign(CACHELINE, hash_table->nservers * sizeof(int));
  memset(pending_count, 0, hash_table->nservers * sizeof(int));

  int *localbuf_index = (int*)memalign(CACHELINE, hash_table->nservers * sizeof(int));
  int *localbuf_size = (int*)memalign(CACHELINE, hash_table->nservers * sizeof(int));
  unsigned long *localbuf = 
    (unsigned long*)memalign(CACHELINE, hash_table->nservers * ONEWAY_BUFFER_SIZE * sizeof(unsigned long));
  memset(localbuf_index, 0, hash_table->nservers * sizeof(int));
  memset(localbuf_size, 0, hash_table->nservers * sizeof(int));
  memset(localbuf, 0, hash_table->nservers * ONEWAY_BUFFER_SIZE * sizeof(unsigned long));
  
  struct box_array *boxes = hash_table->boxes;
  unsigned long msg_data[INSERT_MSG_LENGTH];
  
  for(int i = 0; i < nqueries; i++) {
    int s = hash_get_server(hash_table, queries[i].key); 

    while (pending_count[s] >= ONEWAY_BUFFER_SIZE) {
      int ps = hash_get_server(hash_table, queries[pindex].key); 
      if (localbuf_index[ps] == localbuf_size[ps]) {
        int count;
        if ((count = buffer_read_all(&boxes[client_id].boxes[ps].out, ONEWAY_BUFFER_SIZE, &localbuf[ps * ONEWAY_BUFFER_SIZE])) == 0) {
          buffer_flush(&boxes[client_id].boxes[ps].in);
          while ((count = buffer_read_all(&boxes[client_id].boxes[ps].out, ONEWAY_BUFFER_SIZE, &localbuf[ps * ONEWAY_BUFFER_SIZE])) == 0) {
            _mm_pause();
          }
        }
        pending_count[ps] -= count;
        localbuf_index[ps] = 0;
        localbuf_size[ps] = count;
      }

      values[pindex] = (void *)localbuf[ps * ONEWAY_BUFFER_SIZE + localbuf_index[ps]];
      pindex++;
      localbuf_index[ps]++;
    }

    if (queries[i].optype == 0) {
      buffer_write(&boxes[client_id].boxes[s].in, queries[i].key);
    } else {
      msg_data[0] = (unsigned long)queries[i].key | HASH_INSERT_MASK;
      msg_data[1] = (unsigned long)queries[i].value;
      buffer_write_all(&boxes[client_id].boxes[s].in, INSERT_MSG_LENGTH, msg_data, 0);
    }
    pending_count[s]++;
  }

  for (int i = 0; i < hash_table->nservers; i++) {
    buffer_flush(&boxes[client_id].boxes[i].in);
  }

  while (pindex < nqueries) {
    int ps = hash_get_server(hash_table, queries[pindex].key); 
    if (localbuf_index[ps] == localbuf_size[ps]) {
      int count;
      while ((count = buffer_read_all(&boxes[client_id].boxes[ps].out, ONEWAY_BUFFER_SIZE, &localbuf[ps * ONEWAY_BUFFER_SIZE])) == 0) {
        _mm_pause();
      }

      pending_count[ps] -= count;
      localbuf_index[ps] = 0;
      localbuf_size[ps] = count;
    }

    values[pindex] = (void *)localbuf[ps * ONEWAY_BUFFER_SIZE + localbuf_index[ps]];
    pindex++;
    localbuf_index[ps]++;
  }

  free(pending_count);
  free(localbuf_index);
  free(localbuf_size);
  free(localbuf);
}

void * locking_hash_lookup(struct hash_table *hash_table, hash_key key)
{
  int s = hash_get_server(hash_table, key);
  void *value = NULL;
  int extra;
  anderson_acquire(&hash_table->partitions[s].lock, &extra);
  struct elem *e = hash_lookup(&hash_table->partitions[s], key);
  if (e != NULL) {
    value = e->value;
  }
  anderson_release(&hash_table->partitions[s].lock, &extra);
  return value;
}

void locking_hash_insert(struct hash_table *hash_table, hash_key key, void *value)
{
  int s = hash_get_server(hash_table, key);
  int extra;
  anderson_acquire(&hash_table->partitions[s].lock, &extra);
  hash_insert(&hash_table->partitions[s], key, value);
  anderson_release(&hash_table->partitions[s].lock, &extra);
}

/**
 * Hash Storage Operations
 */
struct elem * hash_lookup(struct partition *p, hash_key key)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, key)].chain);
  struct elem *e = TAILQ_FIRST(eh);
  while (e != NULL) {
    if (e->key == key) {
      p->nhits++;
      lru(p, e);
      return e;
    }
    e = TAILQ_NEXT(e, chain);
  }
  return NULL;
}

struct elem * hash_insert(struct partition *p, hash_key key, void *value)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, key)].chain);
  struct elem *e = hash_lookup(p, key);

  if (e == NULL && p->size == p->max_size) {
    struct elem *l = TAILQ_LAST(&p->lru, lrulist);
    assert(l);
    hash_remove(p, l);
  }

  if (e == NULL) {
    e = TAILQ_FIRST(&p->free);
    assert(e);
    TAILQ_REMOVE(&p->free, e, free);
    TAILQ_INSERT_TAIL(eh, e, chain);
    TAILQ_INSERT_HEAD(&p->lru, e, lru);
    p->size++;
    assert(p->size <= p->max_size);
  }

  e->key = key;
  e->value = value;
  return e;
}

void hash_remove(struct partition *p, struct elem *e)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, e->key)].chain);
  TAILQ_REMOVE(eh, e, chain);
  TAILQ_REMOVE(&p->lru, e, lru);
  p->size--;
  assert(p->size >= 0);
  TAILQ_INSERT_TAIL(&p->free, e, free);
}

void lru(struct partition *p, struct elem *e)
{
  assert(e);
  TAILQ_REMOVE(&p->lru, e, lru);
  TAILQ_INSERT_HEAD(&p->lru, e, lru);
}

/**
 * Hash Table Counters and Stats
 */
int stats_get_nhits(struct hash_table *hash_table)
{
  int nhits = 0;
  for (int i = 0; i < hash_table->nservers; i++) {
    nhits += hash_table->partitions[i].nhits;
  }
  return nhits;
}

size_t stats_get_overhead(struct hash_table *hash_table)
{
  return hash_table->overhead;
}

