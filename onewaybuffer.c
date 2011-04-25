#include <assert.h>
#include <stdlib.h>

#include "onewaybuffer.h"
#include "util.h"

void buffer_write(struct onewaybuffer* buffer, unsigned long data)
{
  // wait till there is space in buffer
  while (buffer->tmp_wr_index >= buffer->rd_index + ONEWAY_BUFFER_SIZE) {
    _mm_pause();
  }

  buffer->data[buffer->tmp_wr_index & (ONEWAY_BUFFER_SIZE - 1)] = data;
  buffer->tmp_wr_index++;
  if (buffer->tmp_wr_index >= buffer->wr_index + BUFFER_FLUSH_COUNT) {
    buffer_flush(buffer);
  }
}

void buffer_write_all(struct onewaybuffer* buffer, int write_count, const unsigned long* data, int force_flush) 
{
  assert(write_count <= ONEWAY_BUFFER_SIZE);
  // wait till there is space in buffer
  while (buffer->tmp_wr_index + write_count - 1 >= buffer->rd_index + ONEWAY_BUFFER_SIZE) {
    _mm_pause();
  }

  for (int i = 0; i < write_count; i++) {
    buffer->data[(buffer->tmp_wr_index + i) & (ONEWAY_BUFFER_SIZE - 1)] = data[i];
  }
  buffer->tmp_wr_index += write_count;
  if (force_flush || (buffer->tmp_wr_index >= buffer->wr_index + BUFFER_FLUSH_COUNT)) {
    buffer_flush(buffer);
  }
}

void buffer_flush(struct onewaybuffer* buffer)
{
  // for safety do memory barrier to make sure data is written before index
  __sync_synchronize(); 
  buffer->wr_index = buffer->tmp_wr_index;
}

int buffer_read_all(struct onewaybuffer* buffer, int max_read_count, unsigned long* data)
{
  int count = buffer->wr_index - buffer->rd_index;
  if (count == 0) return 0;
  if (max_read_count < count) count = max_read_count;

  for (int i = 0; i < count; i++) {
    data[i] = buffer->data[(buffer->rd_index + i) & (ONEWAY_BUFFER_SIZE - 1)];
  }
  buffer->rd_index += count;
  return count;
}

