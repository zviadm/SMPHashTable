#ifndef __ONEWAYBUFFER_H_
#define __ONEWAYBUFFER_H_

#include <stdint.h>
#include "util.h"

// NOTE: all this constants must be powers of 2
#define INPB_SIZE  (CACHELINE >> 3)        // INPB_SIZE * sizeof(uint64_t) == CACHELINE
#define INPB_COUNT 4                       // adjusts size of input buffer
#define OUTB_SIZE  ((CACHELINE >> 3) << 3) // adjusts size of output buffer

struct inputbuffer {
  volatile uint64_t data[INPB_COUNT][INPB_SIZE];
  unsigned long data_rd;
  char padding0[CACHELINE - sizeof(unsigned long)];
  uint64_t local_data[INPB_SIZE];
  unsigned long local_index;
  unsigned long data_wr;
  volatile unsigned long local_waitcnt;
} __attribute__ ((aligned (CACHELINE)));

struct outputbuffer {
  volatile uint64_t data[OUTB_SIZE];
  volatile unsigned long wr_index;
  char padding0[CACHELINE - sizeof(unsigned long)];
  unsigned long local_rd_index;
  unsigned long local_wr_index;
  volatile unsigned long local_waitcnt;
} __attribute__ ((aligned (CACHELINE)));

void inpb_prefetch(struct inputbuffer *buffer);
int inpb_read(struct inputbuffer *buffer, uint64_t *data);
void inpb_write(struct inputbuffer *buffer, int write_count, const uint64_t *data);
void inpb_flush(struct inputbuffer *buffer);

int outb_read(struct outputbuffer *buffer, uint64_t *data);
uint64_t outb_blocking_read(struct outputbuffer *buffer);
void outb_write(struct outputbuffer *buffer, int write_count, const uint64_t *data);
void outb_prefetch(struct outputbuffer *buffer);

#endif
