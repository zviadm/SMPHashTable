#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "util.h"

pid_t gettid(void) 
{
  return syscall(__NR_gettid);
}

void set_affinity(int cpu_id)
{
 int tid = gettid();
 cpu_set_t mask;
 CPU_ZERO(&mask);
 CPU_SET(cpu_id, &mask);
 int r = sched_setaffinity(tid, sizeof(mask), &mask);
 if (r < 0) {
   fprintf(stderr, "couldn't set affinity for cpu_id:%d\n", cpu_id);
   exit(1);
 }
}

double now()
{
 struct timeval tv;
 gettimeofday(&tv, 0);
 return tv.tv_sec + tv.tv_usec / 1000000.0;
}
