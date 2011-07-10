#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include "alock.h"
#include "hashprotocol.h"
#include "partition.h"
#include "util.h"

#define BUCKET_LOAD (2 * 128) // Min Element Size is 128 bytes, so bucket on average will hold 2 elements

void init_hash_partition(struct partition *p, size_t max_size, int nservers, int evictalgo)
{
  assert((unsigned long)p % CACHELINE == 0);
  p->nservers = nservers;
  p->max_size = max_size;
  p->size = 0;
  p->evictalgo = evictalgo;

  // below is a trick to make GCD of p->nhash and nservers equal to 1
  // it can be proved that if GCD of nhash and nservers is 1 then, hash_get_server and
  // hash_get_bucket will be unbiased when input is random
  p->nhash = ceil((double)max(10.0, p->max_size / BUCKET_LOAD) / nservers) * nservers - 1;

  p->nhits = 0;
  p->nlookups = 0;
  p->ninserts = 0;

  p->table = memalign(CACHELINE, p->nhash * sizeof(struct bucket));
  assert((unsigned long) &(p->table[0]) % CACHELINE == 0);
  for (int i = 0; i < p->nhash; i++) {
    TAILQ_INIT(&(p->table[i].chain));
  }

  if (p->evictalgo == EVICT_LRU) {
    TAILQ_INIT(&p->lru);
  }
  anderson_init(&p->lock, nservers);

  p->bucketlocks = memalign(CACHELINE, p->nhash * sizeof(struct alock));
  for (int i = 0; i < p->nhash; i++)
    anderson_init(&p->bucketlocks[i], nservers);
}

void destroy_hash_partition(struct partition *p, release_value_f *release)
{
  size_t dbg_p_size = 0;
  for (int i = 0; i < p->nhash; i++) {
    struct elist *eh = &p->table[i].chain;
    struct elem *e = TAILQ_FIRST(eh);
    while (e != NULL) {
      struct elem *next = TAILQ_NEXT(e, chain);
      dbg_p_size += e->size;
      release(e);
      e = next;
    }
  }

  assert(p->size == dbg_p_size);
  free(p->table);
  free(p->bucketlocks);
}

/**
 * hash_get_bucket: returns bucket were given key is or should be placed
 */
int hash_get_bucket(const struct partition *p, hash_key key)
{
  return key % p->nhash;
}

void hash_remove(struct partition *p, struct elem *e)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, e->key)].chain);
  p->size -= e->size;
  assert(p->size > 0);
  TAILQ_REMOVE(eh, e, chain);
  if (p->evictalgo == EVICT_LRU) {
    TAILQ_REMOVE(&p->lru, e, lru);
  }
}

void lru(struct partition *p, struct elem *e)
{
  assert(e);
  TAILQ_REMOVE(&p->lru, e, lru);
  TAILQ_INSERT_HEAD(&p->lru, e, lru);
}

struct elem * hash_lookup(struct partition *p, hash_key key)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, key)].chain);
  struct elem *e = TAILQ_FIRST(eh);
  while (e != NULL) {
    if (e->key == key) {
      if (p->evictalgo == EVICT_LRU) {
        lru(p, e);
      }

      return e;
    }
    e = TAILQ_NEXT(e, chain);
  }
  return NULL;
}

struct elem * hash_insert(struct partition *p, hash_key key, int size, release_value_f *release)
{
  struct elist *eh = &(p->table[hash_get_bucket(p, key)].chain);
  struct elem *e = hash_lookup(p, key);

  if (e != NULL) {
    hash_remove(p, e);
    release(e);
  } 

  size_t elem_size = (((sizeof(struct elem) + size) + (CACHELINE - 1)) & ~(CACHELINE - 1));
  p->size += elem_size;
  while (1) {
    if (p->size <= p->max_size) {
      // try to allocate space for new value
      if ((e = (struct elem *)memalign(sizeof(struct elem) + size, CACHELINE)) != NULL) {
        break;
      }
    }
    assert(p->size > elem_size);

    struct elem *l = NULL;
    switch (p->evictalgo) {
      case EVICT_LRU:
        l = TAILQ_LAST(&p->lru, elist);
        break;
      case EVICT_RANDOM:
        while (!l) {
          int i = read_tsc() % p->nhash;
          l = TAILQ_FIRST(&p->table[i].chain);
        }
        break;
      default:
        assert(0);
    }
    hash_remove(p, l);
    release(l);
  }

  e->key = key;
  e->size = elem_size;
  TAILQ_INSERT_TAIL(eh, e, chain);
  if (p->evictalgo == EVICT_LRU) {
    TAILQ_INSERT_HEAD(&p->lru, e, lru);
  }
  return e;
}

