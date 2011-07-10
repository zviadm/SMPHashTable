#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "hashprotocol.h"
#include "mpbuffers.h"
#include "partition.h"
#include "smphashtable.h"
#include "util.h"

/** 
 * Hash Table Operations
 */
#define HASHOP_MASK       0xF000000000000000
#define HASHOP_LOOKUP     0x1000000000000000 
#define HASHOP_INSERT     0x2000000000000000 
#define HASHOP_RELEASE    0x3000000000000000 
#define INSERT_MSG_LENGTH 2

#define DATA_READY_MASK   0x8000000000000000

// Maximum pending Requests for a client
#define MAX_PENDING_SIZE        (OUTB_SIZE * MAX_SERVERS)
// Maximum pending requests for a client for a single server
#define MAX_PENDING_PER_SERVER  (OUTB_SIZE - (CACHELINE >> 3))

/**
 * Server/Client Message Passing Data Structures
 */
struct box {
  struct inputbuffer in;
  struct outputbuffer out;
}  __attribute__ ((aligned (CACHELINE)));

struct box_array {
  struct box *boxes;
} __attribute__ ((aligned (CACHELINE)));

struct thread_args {
  int id;
  int core;
  struct hash_table *hash_table;
};

/*
 * Per Client Pending Data
 */
struct pending_data {
  void * pending_values[MAX_PENDING_SIZE];
  int pending_servers[MAX_PENDING_SIZE];
  unsigned long rd_index;
  unsigned long wr_index;
  unsigned long pending_index;
  int * pending_count;
};

/*
 * Hash Table data structure
 */
struct hash_table {
  int nservers;
  volatile int nclients;
  size_t max_size;
  size_t overhead;

  pthread_mutex_t create_client_lock;

  volatile int quitting;
  pthread_t *threads;
  struct thread_args *thread_data;

  struct partition *partitions;
  struct pending_data **pending_data;
  struct box_array *boxes;
};

// Forward declarations
void *hash_table_server(void* args);
int is_value_ready(struct elem *e);
void mp_release_value_(struct elem *e);
void atomic_release_value_(struct elem *e);

/**
 * hash_get_server: returns server that should handle given key
 */
static inline int hash_get_server(const struct hash_table *hash_table, hash_key key)
{
  return key % hash_table->nservers;
}

struct hash_table *create_hash_table(size_t max_size, int nservers) 
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
  hash_table->pending_data = memalign(CACHELINE, MAX_CLIENTS * sizeof(struct pending_data *));
  hash_table->boxes = memalign(CACHELINE, MAX_CLIENTS * sizeof(struct box_array));
  for (int i = 0; i < hash_table->nservers; i++) {
    init_hash_partition(&hash_table->partitions[i], max_size / nservers, nservers);
  }

  hash_table->threads = (pthread_t *)malloc(nservers * sizeof(pthread_t));
  hash_table->thread_data = (struct thread_args *)malloc(nservers * sizeof(struct thread_args));
  return hash_table;
}

void destroy_hash_table(struct hash_table *hash_table)
{
  for (int i = 0; i < hash_table->nservers; i++) {
    destroy_hash_partition(&hash_table->partitions[i], atomic_release_value_);
  }
  free(hash_table->partitions);

  for (int i = 0; i < hash_table->nclients; i++) {
    free(hash_table->pending_data[i]->pending_count);
    free(hash_table->pending_data[i]);
    free(hash_table->boxes[i].boxes);
  }
  free(hash_table->pending_data);
  free(hash_table->boxes);

  free(hash_table->threads);
  free(hash_table->thread_data);
  pthread_mutex_destroy(&hash_table->create_client_lock); 
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

  // flush all client buffers
  for (int i = 0; i < hash_table->nservers; i++) {
    for (int k = 0; k < hash_table->nclients; k++) {
      inpb_flush(&hash_table->boxes[k].boxes[i].in);
    }
  }

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
    assert((unsigned long) &hash_table->boxes[client].boxes[i].out % CACHELINE == 0);
  }

  hash_table->pending_data[client] = memalign(CACHELINE, sizeof(struct pending_data));
  hash_table->pending_data[client]->rd_index = 0;  
  hash_table->pending_data[client]->wr_index = 0;  
  hash_table->pending_data[client]->pending_index = 0;  
  hash_table->pending_data[client]->pending_count = (int*)memalign(CACHELINE, hash_table->nservers * sizeof(int));
  memset(hash_table->pending_data[client]->pending_count, 0, hash_table->nservers * sizeof(int));
  
  __sync_add_and_fetch(&hash_table->nclients, 1);
  pthread_mutex_unlock(&hash_table->create_client_lock);
  return client;
}

void * hash_table_server(void* args)
{
  int s = ((struct thread_args *) args)->id;
  int c = ((struct thread_args *) args)->core;
  struct hash_table *hash_table = ((struct thread_args *) args)->hash_table;
  struct partition *p = &hash_table->partitions[s];
  struct box_array *boxes = hash_table->boxes;
  uint64_t localbuf[INPB_SIZE];
  int quitting = 0;

  set_affinity(c);

  while (quitting == 0) {
    // after server receives quit signal it should make sure to complete all 
    // queries that are in the buffers
    quitting = hash_table->quitting;

    int nclients = hash_table->nclients;
#if 0
    for (int i = 0; i < nclients; i++) {
      inpb_prefetch(&boxes[i].boxes[s].in);
    }
#endif
    for (int i = 0; i < nclients; i++) {
      int count = inpb_read(&boxes[i].boxes[s].in, localbuf);
      if (count == 0) continue;

      int k = 0;
      int j = 0;
      struct elem *e;
      unsigned long value;
      while (k < count) {
        value = 0;

        switch (localbuf[k] & HASHOP_MASK) {
          case HASHOP_LOOKUP:
            {
              p->nlookups++;
              e = hash_lookup(p, localbuf[k] & (~HASHOP_MASK));
              if ((e != NULL) && (is_value_ready(e))) {
                p->nhits++;
                e->ref_count++;
                value = (unsigned long)e->value;
              }
              k++;

              localbuf[j] = value;
              j++;
              break;
            }
          case HASHOP_INSERT:
            {
              p->ninserts++;
              e = hash_insert(p, localbuf[k] & (~HASHOP_MASK), localbuf[k + 1], mp_release_value_);
              if (e != NULL) {
                e->ref_count = DATA_READY_MASK | 2; 
                value = (unsigned long)e->value;
              }
              k += INSERT_MSG_LENGTH;

              localbuf[j] = value;
              j++;
              break;
            }
          case HASHOP_RELEASE:
            {
              struct elem *e = (struct elem *)(localbuf[k] & (~HASHOP_MASK));
              mp_release_value_(e);
              k++;
              break;
            }
          default:
            assert(0);
            break;
        }
      }

      outb_write(&boxes[i].boxes[s].out, j, localbuf);
    }
  }

#if 0
  unsigned long tmp0 = 0;
  unsigned long tmp1 = 0;
  for (int i = 0; i < hash_table->nclients; i++) {
    tmp0 += boxes[i].boxes[s].in.local_waitcnt;
    tmp1 += boxes[i].boxes[s].out.local_waitcnt;
  }
  printf("%2d %10lu %10lu\n", s, tmp0, tmp1);
#endif
  return NULL;
}

void read_next_pending_value(struct hash_table *hash_table, int client_id)
{
  struct pending_data *pd = hash_table->pending_data[client_id];
  int ps = pd->pending_servers[pd->wr_index & (MAX_PENDING_SIZE - 1)]; 
  uint64_t val;
  int r = outb_read(&hash_table->boxes[client_id].boxes[ps].out, &val);
  if (r == 0) {
    inpb_flush(&hash_table->boxes[client_id].boxes[ps].in);
    val = outb_blocking_read(&hash_table->boxes[client_id].boxes[ps].out);
  }
  pd->pending_values[pd->wr_index & (MAX_PENDING_SIZE - 1)] = (void *)(unsigned long)val;
  pd->pending_count[ps]--;
  pd->wr_index++;
}

int smp_hash_lookup(struct hash_table *hash_table, int client_id, hash_key key)
{
  struct pending_data *pd = hash_table->pending_data[client_id];
  if (pd->pending_index - pd->rd_index >= MAX_PENDING_SIZE) return 0;

  uint64_t msg_data = key | HASHOP_LOOKUP;
  int s = hash_get_server(hash_table, key);
  while (pd->pending_count[s] >= MAX_PENDING_PER_SERVER) {
    read_next_pending_value(hash_table, client_id);
  }
  inpb_write(&hash_table->boxes[client_id].boxes[s].in, 1, &msg_data);
  pd->pending_count[s]++;
  pd->pending_servers[pd->pending_index & (MAX_PENDING_SIZE - 1)] = s;
  pd->pending_index++;
  return 1;
}

int smp_hash_insert(struct hash_table *hash_table, int client_id, hash_key key, int size)
{
  struct pending_data *pd = hash_table->pending_data[client_id];
  if (pd->pending_index - pd->rd_index >= MAX_PENDING_SIZE) return 0;

  uint64_t msg_data[INSERT_MSG_LENGTH];
  msg_data[0] = (unsigned long)key | HASHOP_INSERT;
  msg_data[1] = (unsigned long)size;
  int s = hash_get_server(hash_table, key);
  while (pd->pending_count[s] >= MAX_PENDING_PER_SERVER) {
    read_next_pending_value(hash_table, client_id);
  }
  inpb_write(&hash_table->boxes[client_id].boxes[s].in, INSERT_MSG_LENGTH, msg_data);
  pd->pending_count[s]++;
  pd->pending_servers[pd->pending_index & (MAX_PENDING_SIZE - 1)] = s;
  pd->pending_index++;
  return 1;
}

int smp_try_get_next(struct hash_table *hash_table, int client_id, void **value)
{
  struct pending_data *pd = hash_table->pending_data[client_id];
  if (pd->rd_index == pd->wr_index) return 0;
  *value = pd->pending_values[pd->rd_index & (MAX_PENDING_SIZE - 1)];
  pd->rd_index++;
  return 1;
}

int smp_get_next(struct hash_table *hash_table, int client_id, void **value)
{
  struct pending_data *pd = hash_table->pending_data[client_id];
  if (pd->rd_index == pd->pending_index) return 0;
  if (pd->rd_index == pd->wr_index) {
    read_next_pending_value(hash_table, client_id);
  }
  *value = pd->pending_values[pd->rd_index & (MAX_PENDING_SIZE - 1)];
  pd->rd_index++;
  return 1;
}

/**
 * XXX: This method is useless now since it can be effectively replaced 
 * by other more flexible methods. And their performance is essentially the same if not better
 */
void smp_hash_doall(struct hash_table *hash_table, int client_id, int nqueries, struct hash_query *queries, void **values)
{
  int pindex = 0;

  int *pending_count = (int*)memalign(CACHELINE, hash_table->nservers * sizeof(int));
  memset(pending_count, 0, hash_table->nservers * sizeof(int));

  struct box_array *boxes = hash_table->boxes;
  uint64_t msg_data[INSERT_MSG_LENGTH];

  for(int i = 0; i < nqueries; i++) {
    int s = hash_get_server(hash_table, queries[i].key); 

    while (pending_count[s] >= MAX_PENDING_PER_SERVER) {
      // we need to read values from server "s" buffer otherwise if we try to write
      // more queries to server "s" it will be blocked trying to write the return value 
      // to the buffer and it can easily cause deadlock between clients and servers
      //
      // however instead of reading server "s"-s buffer immediatelly we will read elements 
      // in same order that they were queued till we reach elements that were queued for 
      // server "s"
      int ps = hash_get_server(hash_table, queries[pindex].key); 
      uint64_t val;
      int r = outb_read(&boxes[client_id].boxes[ps].out, &val);
      if (r == 0) {
        inpb_flush(&boxes[client_id].boxes[ps].in);
        val = outb_blocking_read(&boxes[client_id].boxes[ps].out);
      }
      values[pindex] = (void *)(unsigned long)val;
      pending_count[ps]--;
      pindex++;
    }

    switch (queries[i].optype) {
      case OPTYPE_LOOKUP:
        msg_data[0] = (uint64_t)queries[i].key | HASHOP_LOOKUP;
        inpb_write(&boxes[client_id].boxes[s].in, 1, msg_data);
        break;
      case OPTYPE_INSERT:
        msg_data[0] = (uint64_t)queries[i].key | HASHOP_INSERT;
        msg_data[1] = (uint64_t)queries[i].size;
        inpb_write(&boxes[client_id].boxes[s].in, INSERT_MSG_LENGTH, msg_data);
        break;
      default:
        assert(0);
        break;
    }
    pending_count[s]++;
  }

  // after queueing all the queries we flush all buffers and read all remaining values
  for (int i = 0; i < hash_table->nservers; i++) {
    inpb_flush(&boxes[client_id].boxes[i].in);
  }

  while (pindex < nqueries) {
    int ps = hash_get_server(hash_table, queries[pindex].key); 
    values[pindex] = (void *)(unsigned long)outb_blocking_read(&boxes[client_id].boxes[ps].out);
    pending_count[ps]--;
    pindex++;
  }

  free(pending_count);
}

void * locking_hash_lookup(struct hash_table *hash_table, hash_key key)
{
  int s = hash_get_server(hash_table, key);
  void *value = NULL;
  int extra;
  anderson_acquire(&hash_table->partitions[s].lock, &extra);
  hash_table->partitions[s].nlookups++;
  struct elem *e = hash_lookup(&hash_table->partitions[s], key);
  if (e != NULL && is_value_ready(e)) {
    hash_table->partitions[s].nhits++;
    __sync_add_and_fetch(&(e->ref_count), 1);
    value = e->value;
  }
  anderson_release(&hash_table->partitions[s].lock, &extra);
  return value;
}

void * locking_hash_insert(struct hash_table *hash_table, hash_key key, int size)
{
  int s = hash_get_server(hash_table, key);
  void *value = NULL;
  int extra;
  anderson_acquire(&hash_table->partitions[s].lock, &extra);
  hash_table->partitions[s].ninserts++;
  struct elem *e = hash_insert(&hash_table->partitions[s], key, size, atomic_release_value_);
  if (e != NULL) {
    e->ref_count = DATA_READY_MASK | 1;
    value = e->value;
  }
  anderson_release(&hash_table->partitions[s].lock, &extra);
  return value;
}

/**
 * Value Memory Management Operations
 */
int is_value_ready(struct elem *e)
{
  return (e->ref_count & DATA_READY_MASK) == 0 ? 1 : 0;
}

void mp_release_value_(struct elem *e)
{
  e->ref_count = (e->ref_count & (~DATA_READY_MASK)) - 1;
  if (e->ref_count == 0) {
    free(e);
  }
}

void mp_send_release_msg_(struct hash_table *hash_table, int client_id, void *ptr, int force_flush)
{
  struct elem *e = (struct elem *)(ptr - sizeof(struct elem));
  uint64_t msg_data = (uint64_t)e | HASHOP_RELEASE;
  assert((uint64_t)e == (msg_data & (~HASHOP_MASK)));

  int s = hash_get_server(hash_table, e->key);
  inpb_write(&hash_table->boxes[client_id].boxes[s].in, 1, &msg_data);
  if (force_flush) {
    inpb_flush(&hash_table->boxes[client_id].boxes[s].in);
  }
}

void mp_release_value(struct hash_table *hash_table, int client_id, void *ptr)
{
  mp_send_release_msg_(hash_table, client_id, ptr, 0 /* force_flush */);
}

void mp_mark_ready(struct hash_table *hash_table, int client_id, void *ptr)
{
  mp_send_release_msg_(hash_table, client_id, ptr, 1 /* force_flush */);
}

void atomic_release_value_(struct elem *e)
{
  uint64_t ref_count = __sync_sub_and_fetch(&(e->ref_count), 1);
  if (ref_count == 0) {
    free(e);
  }
}

void atomic_release_value(void *ptr)
{
  struct elem *e = (struct elem *)(ptr - sizeof(struct elem));
  uint64_t ref_count = __sync_sub_and_fetch(&(e->ref_count), 1);
  if (ref_count == 0) {
    free(e);
  }
}

void atomic_mark_ready(void *ptr)
{
  struct elem *e = (struct elem *)(ptr - sizeof(struct elem));
  uint64_t ref_count = __sync_and_and_fetch(&(e->ref_count), (~DATA_READY_MASK));
  if (ref_count == 0) {
    free(e);
  }
}

/**
 * Hash Table Counters and Stats
 */
void stats_reset(struct hash_table *hash_table)
{
  for (int i = 0; i < hash_table->nservers; i++) {
    hash_table->partitions[i].nhits = 0;
    hash_table->partitions[i].nlookups = 0;
    hash_table->partitions[i].ninserts = 0;
  }
}

int stats_get_nhits(struct hash_table *hash_table)
{
  int nhits = 0;
  for (int i = 0; i < hash_table->nservers; i++) {
    nhits += hash_table->partitions[i].nhits;
  }
  return nhits;
}

int stats_get_nlookups(struct hash_table *hash_table)
{
  int nlookups = 0;
  for (int i = 0; i < hash_table->nservers; i++) {
    nlookups += hash_table->partitions[i].nlookups;
  }
  return nlookups;
}

int stats_get_ninserts(struct hash_table *hash_table)
{
  int ninserts = 0;
  for (int i = 0; i < hash_table->nservers; i++) {
    ninserts += hash_table->partitions[i].ninserts;
  }
  return ninserts;
}

size_t stats_get_overhead(struct hash_table *hash_table)
{
  return hash_table->overhead;
}

void stats_get_buckets(struct hash_table *hash_table, int server, double *avg, double *stddev)
{
  struct partition *p = &hash_table->partitions[server];

  int nelems = 0;
  struct elem *e;
 
  e = TAILQ_FIRST(&p->lru);
  while (e != NULL) {
    nelems++;
    e = TAILQ_NEXT(e, lru);
  }
  *avg = (double)nelems / p->nhash;
  *stddev = 0;

  for (int i = 0; i < p->nhash; i++) {
    e = TAILQ_FIRST(&p->table[i].chain);
    int length = 0;
    while (e != NULL) {
      e = TAILQ_NEXT(e, chain);
      length++;
    }

    *stddev += (*avg - length) * (*avg - length);
  }

  *stddev = sqrt(*stddev / (nelems - 1));
}

void stats_get_mem(struct hash_table *hash_table, size_t *used, size_t *total, double *util)
{
  struct partition *p;
  size_t m = 0, u = 0;

  for (int i = 0; i < hash_table->nservers; i++) {
    p = &hash_table->partitions[i];
  
    m += p->max_size;
    //u += localmem_used(&p->mem);
  }

  *total = m;
  *used = u;
  *util = 0.0;
}
