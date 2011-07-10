#ifndef __PARTITION_H_
#define __PARTITION_H_

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
  // size must be 48 bytes
  hash_key key;
  uint64_t ref_count;
  TAILQ_ENTRY(elem) chain;
  TAILQ_ENTRY(elem) lru;
  
  // data goes here
  char value[0];
};

struct bucket {
  TAILQ_HEAD(elist, elem) chain;
};

struct partition {
  int nservers;
  int nhash;
  size_t max_size;
  struct bucket *table;
  TAILQ_HEAD(lrulist, elem) lru;

  // stats
  int nhits;
  int nlookups;
  int ninserts;

  struct alock lock;   // partition lock for locking implementation
} __attribute__ ((aligned (CACHELINE)));

typedef void release_value_f(struct elem *e);

void init_hash_partition(struct partition *p, size_t max_size, int nservers);
void destroy_hash_partition(struct partition *p, release_value_f *release);

struct elem * hash_lookup(struct partition *p, hash_key key);
struct elem * hash_insert(struct partition *p, hash_key key, int size, release_value_f *release);

#endif
