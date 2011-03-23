#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "onewaybuffer.h"
#include "smphashtable.h"

#define MAX_CLIENTS       50
#define BUCKET_LOAD       64
#define SERVER_READ_COUNT BUFFER_FLUSH_COUNT
#define ELEM_OVERHEAD     (sizeof(hash_key) + sizeof(struct hash_value))
#define HASH_INSERT_MASK  0x8000000000000000 

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
  TAILQ_ENTRY(elem) free;
};

struct bucket {
  TAILQ_HEAD(elist, elem) chain;
} __attribute__ ((aligned (CACHELINE)));

struct partition {
  struct bucket *table;
  TAILQ_HEAD(lrulist, elem) lru;
  TAILQ_HEAD(freelist, elem) free;
  int nhash;
  size_t size;
  size_t max_size;
  int nhit;
  struct elem *elems;
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
  int nclients;
  size_t max_size;

  int quitting;
  pthread_t *threads;
  struct thread_args *thread_data;

  struct partition *partitions __attribute__ ((aligned (CACHELINE)));
  struct box_array boxes[MAX_CLIENTS] __attribute__ ((aligned (CACHELINE)));
};

// Forward declaration of functions
void init_hash_table(struct hash_table *hash_table);
void init_hash_partition(struct hash_table *hash_table, struct partition *p);
void *hash_table_server(void* args);
void set_affinity(int cpu_id);
struct elem * hash_lookup(struct partition *p, hash_key key);
struct elem * hash_insert(struct partition *p, hash_key key, struct hash_value* value);
void hash_remove(struct partition *p, struct elem *e);
void lru(struct partition *p, struct elem *e);
void release_hash_value(struct hash_value *value);

struct hash_table *create_hash_table(size_t max_size, int nservers) 
{
  struct hash_table *hash_table = (struct hash_table *)malloc(sizeof(struct hash_table));
  hash_table->nservers = nservers;
  hash_table->nclients = 0;
  hash_table->max_size = max_size;

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
    init_hash_partition(hash_table, &hash_table->partitions[i]);
  }

  assert((unsigned long) &hash_table->partitions[0] % CACHELINE == 0);
  assert((unsigned long) &hash_table->partitions[1] % CACHELINE == 0);
}

void init_hash_partition(struct hash_table *hash_table, struct partition *p)
{
  p->max_size = hash_table->max_size / hash_table->nservers;
  p->nhash = p->max_size / BUCKET_LOAD;
  p->nhit = 0;
  p->size = 0;

  p->table = memalign(CACHELINE, p->nhash * sizeof(struct bucket));
  assert((unsigned long) &(p->table[0]) % CACHELINE == 0);
  for (int i = 0; i < p->nhash; i++) {
    TAILQ_INIT(&(p->table[i].chain));
  }

  TAILQ_INIT(&p->lru);
  TAILQ_INIT(&p->free);
  p->elems = memalign(CACHELINE, (p->max_size / ELEM_OVERHEAD) * sizeof(struct elem));
  for (int i = 0; i < p->max_size / ELEM_OVERHEAD; i++) {
    struct elem *e = &p->elems[i];
    TAILQ_INSERT_TAIL(&p->free, e, free);
  }
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
  int s = ((struct thread_args *) args)->id;
  int c = ((struct thread_args *) args)->core;
  struct hash_table *hash_table = ((struct thread_args *) args)->hash_table;
  struct partition *p = &hash_table->partitions[s];
  struct box_array *boxes = hash_table->boxes;
  unsigned long localbuf[SERVER_READ_COUNT];

  set_affinity(c);

//  start_counters(c, cpuseq[c]);
//  read_counters(c);

  while (hash_table->quitting == 0) {
    int nclients = hash_table->nclients;
    for (int i = 0; i < nclients; i++) {
      int count = buffer_read_all(&boxes[i].boxes[s].in, SERVER_READ_COUNT, localbuf);
      if (count > 0) {
        int k = 0;
        int j = 0;
        while (k < count) {
          if (localbuf[k] & HASH_INSERT_MASK) {
            assert((k + 1) < count);
            hash_insert(p, localbuf[k] & (HASH_INSERT_MASK - 1), (struct hash_value *)localbuf[k + 1]);
            k += 2;
          } else {
            struct elem *e = hash_lookup(p, localbuf[k]);
            localbuf[j] = (unsigned long)e->value;
            j++;
            k++;
          }
        }

        if (j > 0) {
          buffer_write_all(&boxes[i].boxes[s].out, j, localbuf);
        }
      }
    }
  }

//  read_counters(c);
  return 0;
}

void set_affinity(int cpu_id)
{
 int tid = syscall(__NR_gettid);
 cpu_set_t mask;
 CPU_ZERO(&mask);
 CPU_SET(cpu_id, &mask);
 int r = sched_setaffinity(tid, sizeof(mask), &mask);
 if (r < 0) {
   fprintf(stderr, "couldn't set affinity for cpu_id:%d\n", cpu_id);
   exit(1);
 }
}

/*
void (void *xa)
{
  const int cli = ((thread_args_t*) xa)->id; // client number, 0..clients
  const int c = ((thread_args_t*) xa)->core;

  const unsigned int max_pending_count = 10 * nserver * ONEWAY_BUFFER_SIZE;
  int pending_head_index = 0;
  int pending_tail_index = 0;
  int *pending_servers = (int*)malloc(max_pending_count * sizeof(int));

  int *pending_count = (int*)malloc(nserver * sizeof(int));
  memset(pending_count, 0, nserver * sizeof(int));

  int *localbuf_index = (int*)malloc(nserver * sizeof(int));
  int *localbuf_size = (int*)malloc(nserver * sizeof(int));
  unsigned int *localbuf = (unsigned int*)malloc(nserver * ONEWAY_BUFFER_SIZE * sizeof(unsigned int));
  memset(localbuf_index, 0, nserver * sizeof(int));
  memset(localbuf_size, 0, nserver * sizeof(int));
  memset(localbuf, 0, nserver * ONEWAY_BUFFER_SIZE * sizeof(unsigned int));

  set_affinity(cpuseq[c]);
#ifdef COUNTER
  start_counters(c, cpuseq[c]);
  read_counters(c);
#endif
  for(int i = 0; i < niter; i++) {
    int r = draw(c) & query_mask;
    int s = map[r & MASK];
    assert(s < nserver);

    while (pending_count[s] >= ONEWAY_BUFFER_SIZE) {
      const unsigned int ps = pending_servers[pending_head_index];
      pending_head_index = (pending_head_index + 1) % max_pending_count;
      if (localbuf_index[ps] == localbuf_size[ps]) {
        int count;
        if ((count = BufferReadAll(&boxes[ps].boxes[cli].box.buffer.out, ONEWAY_BUFFER_SIZE, &localbuf[ps * ONEWAY_BUFFER_SIZE])) == 0) {
          BufferFlush(&boxes[ps].boxes[cli].box.buffer.in);
          while ((count = BufferReadAll(&boxes[ps].boxes[cli].box.buffer.out, ONEWAY_BUFFER_SIZE, &localbuf[ps * ONEWAY_BUFFER_SIZE])) == 0) {
            _mm_pause();
          }
        }
        pending_count[ps] -= count;
        localbuf_index[ps] = 0;
        localbuf_size[ps] = count;
      }

      // the result will be in localbuf[ps * ONEWAY_BUFFER_SIZE + localbuf_index[ps]]
      //printf("%d - %u\n", cli, localbuf[ps * ONEWAY_BUFFER_SIZE + localbuf_index[ps]]); 
      localbuf_index[ps]++;
    }

    BufferWrite(&boxes[s].boxes[cli].box.buffer.in, r);
    pending_servers[pending_tail_index] = s;
    pending_tail_index = (pending_tail_index + 1) % max_pending_count; 
    pending_count[s]++;
  }

  for (int i = 0; i < nserver; i++) {
    BufferFlush(&boxes[i].boxes[cli].box.buffer.in);
  }

  while (pending_head_index != pending_tail_index) {
    const unsigned int ps = pending_servers[pending_head_index];
    pending_head_index = (pending_head_index + 1) % max_pending_count;
    if (localbuf_index[ps] == localbuf_size[ps]) {
      int count;
      while ((count = BufferReadAll(&boxes[ps].boxes[cli].box.buffer.out, ONEWAY_BUFFER_SIZE, &localbuf[ps * ONEWAY_BUFFER_SIZE])) == 0) {
        _mm_pause();
      }

      pending_count[ps] -= count;
      localbuf_index[ps] = 0;
      localbuf_size[ps] = count;
    }

    // the result will be in localbuf[ps * ONEWAY_BUFFER_SIZE + localbuf_index[ps]]
    //printf("%d - %u\n", cli, localbuf[ps * ONEWAY_BUFFER_SIZE + localbuf_index[ps]]); 
    localbuf_index[ps]++;
  }

#ifdef COUNTER
  read_counters(c);
#endif
  counts[cli] = niter;

  free(pending_servers);
  free(pending_count);
  free(localbuf_index);
  free(localbuf_size);
  free(localbuf);
  return (void *) 0;
}
*/


/**
 * Hash Storage Operations
 */
inline int hash_get_bucket(struct partition *p, hash_key key);
int hash_get_bucket(struct partition *p, hash_key key)
{
  return key % p->nhash;
}

struct elem * hash_lookup(struct partition *p, hash_key key)
{
  struct elist *eh;
  eh = &(p->table[hash_get_bucket(p, key)].chain);
  struct elem *e = TAILQ_FIRST(eh);
  while (e != NULL) {
    if (e->key == key) {
      p->nhit++;
      return e;
    }
    e = TAILQ_NEXT(e, chain);
  }
  return e;
}

struct elem * hash_insert(struct partition *p, hash_key key, struct hash_value* value)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, key)].chain);
  p->size += ELEM_OVERHEAD + value->size;
  while (p->size > p->max_size) {
    struct elem *l = TAILQ_LAST(&p->lru, lrulist);
    assert(l);
    hash_remove(p, l);
  } 

  struct elem *e = hash_lookup(p, key);
  if (e == NULL) {
    e = TAILQ_FIRST(&p->free);
    assert(e);
    TAILQ_REMOVE(&p->free, e, free);
  } else {
    release_hash_value(e->value);
  }

  e->key = key;
  e->value = value;
  TAILQ_INSERT_TAIL(eh, e, chain);
  TAILQ_INSERT_HEAD(&p->lru, e, lru);
  return e;
}

void hash_remove(struct partition *p, struct elem *e)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, e->key)].chain);
  TAILQ_REMOVE(eh, e, chain);
  TAILQ_REMOVE(&p->lru, e, lru);
  p->size -= ELEM_OVERHEAD + e->value->size;
  assert(p->size >= 0);
  release_hash_value(e->value);
  TAILQ_INSERT_TAIL(&p->free, e, free);
}

void lru(struct partition *p, struct elem *e)
{
  assert(e);
  TAILQ_REMOVE(&p->lru, e, lru);
  TAILQ_INSERT_HEAD(&p->lru, e, lru);
}

void release_hash_value(struct hash_value *value)
{
  if (__sync_sub_and_fetch(&value->ref_count, 1) == 0) {
    free(value);
  }
}
