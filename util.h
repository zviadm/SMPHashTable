#ifndef __UTIL_H_
#define __UTIL_H_

#include <sys/types.h>

#define CACHELINE 64

pid_t gettid(void);
void set_affinity(int cpu_id);
double now();

#endif
