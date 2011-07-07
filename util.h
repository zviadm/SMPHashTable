#ifndef __UTIL_H_
#define __UTIL_H_

#include <sys/types.h>
#include <stdint.h>

#define CACHELINE   64 
#define MAX_CLIENTS 1024
#define MAX_SERVERS 1024

pid_t gettid(void);
void set_affinity(int cpu_id);
double now();

static inline int max(int a, int b) 
{ 
  if (a > b) return a; 
  else return b; 
}

static inline int min(int a, int b) 
{ 
  if (a < b) return a; 
  else return b; 
}

// even though this is defined in SSE2 it is 
// easier and more compatible to do it this way
// and not include sse2 headers and build flags
static inline void _mm_pause() 
{
  __asm __volatile("pause");
}

static inline uint64_t __attribute__((always_inline))
read_tsc(void)
{
  uint32_t a, d;
  __asm __volatile("rdtsc" : "=a" (a), "=d" (d));
  return ((uint64_t) a) | (((uint64_t) d) << 32);
}

#endif
