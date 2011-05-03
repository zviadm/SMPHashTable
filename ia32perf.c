#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ia32msr.h"
#include "ia32perf.h"

#ifndef AMD64
  // Intel Core I7 (default)
  #define NPMC        3     // number of available counter MSRs,
                            // we can get this number using CPUID too but it is too much work just set manually
  #define PMC0        0xC1  // start address of coutner MSR
  #define PERFEVTSEL0 0x186 // start address of event select MSR
#else
  // AMD64 Opteron and Such
  #define NPMC        4
  #define PMC0        0xC0010004
  #define PERFEVTSEL0 0xC0010000
#endif

int
StartCounter(int cpu, int pmc_index, uint64_t event_select_value) 
{
  int r;
  if (pmc_index >= NPMC) {
    return -1;
  }

  // Stop Counter
  if ((r = WriteMSR(cpu, PERFEVTSEL0 + pmc_index, 0)) < 0) {
    return r;
  }

  // Reset Counter
  if ((r = WriteMSR(cpu, PMC0 + pmc_index, 0)) < 0) {
    return r;
  }

  // Restart Counter with new event select value
  if ((r = WriteMSR(cpu, PERFEVTSEL0 + pmc_index, event_select_value)) < 0) {
    return r;
  }

  return 0;
}

int
ReadCounter(int cpu, int pmc_index, uint64_t *value)
{
  int r;
  if (pmc_index >= NPMC) {
    return -1;
  }

  // Read Counter
  if ((r = ReadMSR(cpu, PMC0 + pmc_index, value)) < 0) {
    return r;
  }

  return 0;
}

