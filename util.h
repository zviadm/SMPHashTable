#ifndef __UTIL_H_
#define __UTIL_H_

#include <sys/types.h>

#define CACHELINE 64 

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


#endif
