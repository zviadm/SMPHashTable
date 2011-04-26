#ifndef __ONEWAYBUFFER_H_
#define __ONEWAYBUFFER_H_

#include <stdint.h>
#include "util.h"

#define ONEWAY_BUFFER_SIZE  (2 * (CACHELINE >> 3)) 
#define BUFFER_FLUSH_COUNT  8

struct onewaybuffer {
  volatile uint64_t data[ONEWAY_BUFFER_SIZE];
  volatile unsigned long rd_index;
  volatile char padding0[CACHELINE - sizeof(long)];
  volatile unsigned long wr_index;
  volatile char padding1[CACHELINE - sizeof(long)];
  volatile unsigned long tmp_wr_index;
  volatile char padding2[CACHELINE - sizeof(long)];
} __attribute__ ((aligned (CACHELINE)));

void buffer_write(struct onewaybuffer* buffer, uint64_t data);
void buffer_write_all(struct onewaybuffer* buffer, int write_count, const uint64_t* data, int force_flush);
void buffer_flush(struct onewaybuffer* buffer);
int buffer_read_all(struct onewaybuffer* buffer, int max_read_count, uint64_t* data);

#endif
