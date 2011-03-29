#ifndef __IA32MSR_H__
#define __IA32MSR_H__

#include <unistd.h>
#include <inttypes.h>

// This module provides support for reading and writing
// Intel IA32 (x64) machine specific registers (MSRs)

int ReadMSR(int cpu, uint32_t reg, uint64_t* value);
int WriteMSR(int cpu, uint32_t reg, uint64_t value);
 
#endif
