#include <stdio.h>
#include <assert.h>

#include "alock.h"

void anderson_init(struct alock *al, int nthread)
{
  assert(al);
  assert(nthread < MAX_NTHREAD);
  al->has_lock[0].x = 1;
  al->nthread = nthread;
}

void anderson_acquire(struct alock *lock, int *extra)
{
  int me = __sync_fetch_and_add(&lock->next_slot, 1);
  if(me > 0 && (me % lock->nthread) == 0)
    __sync_fetch_and_add(&lock->next_slot, -lock->nthread);
  me = me % lock->nthread;
  while(lock->has_lock[me].x == 0) {
    __asm __volatile("pause");
  }
  lock->has_lock[me].x = 0;
  *extra = me;
}

void anderson_release(struct alock *lock, int *extra)
{
  int me = *extra;
  lock->has_lock[(me + 1) % lock->nthread].x = 1;
}
