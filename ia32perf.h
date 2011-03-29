#ifndef __IA32PERF_H__
#define __IA32PERF_H__

#include <inttypes.h>
#include <unistd.h>

// This module provides support for using performance counters
// for Intel IA32 (x64) processors

int StartCounter(int cpu, int pmc_index, uint64_t event_select_value);
int ReadCounter(int cpu, int pmc_index, uint64_t *value);

#endif
