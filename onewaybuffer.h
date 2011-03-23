#ifndef __ONEWAYBUFFER_H_
#define __ONEWAYBUFFER_H_

#define CACHELINE             64  // size of cache line in bytes
#define ONEWAY_BUFFER_SIZE    (CACHELINE >> 3) 
#define BUFFER_FLUSH_COUNT    4

struct onewaybuffer {
  volatile unsigned long data[CACHELINE >> 3];
  volatile long rd_index;
  volatile char padding0[CACHELINE - sizeof(long)];
  volatile long wr_index;
  volatile char padding1[CACHELINE - sizeof(long)];
  volatile long tmp_wr_index;
  volatile char padding2[CACHELINE - sizeof(long)];
} __attribute__ ((aligned (CACHELINE)));

void buffer_write(struct onewaybuffer* buffer, unsigned long data);
void buffer_write_all(struct onewaybuffer* buffer, int write_count, const unsigned long* data);
void buffer_flush(struct onewaybuffer* buffer);
int buffer_read_all(struct onewaybuffer* buffer, int max_read_count, unsigned long* data);

#endif
