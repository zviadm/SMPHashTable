#ifndef __ALOCK_H__
#define __ALOCK_H__

#include "util.h"

// anderson lock
#define MAX_NTHREAD 100

struct myalock {
  volatile int x;
} __attribute__ ((aligned (CACHELINE)));

struct alock {
  volatile struct myalock has_lock[MAX_NTHREAD] __attribute__ ((aligned (CACHELINE)));
  volatile int next_slot;
  volatile int nthread;
} __attribute__ ((aligned (CACHELINE)));


void anderson_init(struct alock *alock, int nthread);
void anderson_acquire(struct alock *lock, int *extra);
void anderson_release(struct alock *lock, int *extra);


#endif
