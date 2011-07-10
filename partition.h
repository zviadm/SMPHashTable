#ifndef __PARTITION_H_
#define __PARTITION_H_

#include <sys/queue.h>

#include "alock.h"
#include "hashprotocol.h"
#include "util.h"

/**
 * Hash Table Storage Data Structures
 * struct elem       - element in table
 * struct bucket     - a bucket in a partition
 * struct partition  - hash table partition for server
 */
struct elem {
  // size must be 64 bytes
  hash_key key;
  uint64_t ref_count;
  size_t size;
  uint64_t padding;
  TAILQ_ENTRY(elem) chain;
  TAILQ_ENTRY(elem) lru;
  
  // data goes here
  char value[0];
};

TAILQ_HEAD(elist, elem);

struct bucket {
  struct elist chain;
};

// supported eviction algorithms
enum evictalgo {
  EVICT_LRU    = 1,
  EVICT_RANDOM = 2
};

struct partition {
  int nservers;
  int nhash;
  size_t max_size;
  size_t size;
  enum evictalgo evictalgo;
  struct bucket *table;
  struct alock *bucketlocks;
  struct elist lru;

  // stats
  int nhits;
  int nlookups;
  int ninserts;

  struct alock lock;   // partition lock for locking implementation
} __attribute__ ((aligned (CACHELINE)));

typedef void release_value_f(struct elem *e);

void init_hash_partition(struct partition *p, size_t max_size, int nservers, int evictalgo);
void destroy_hash_partition(struct partition *p, release_value_f *release);

struct elem * hash_lookup(struct partition *p, hash_key key);
struct elem * hash_insert(struct partition *p, hash_key key, int size, release_value_f *release);
int hash_get_bucket(const struct partition *p, hash_key key);

#endif
