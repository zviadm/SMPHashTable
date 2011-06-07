#include <assert.h>
#include <stdlib.h>

#include "mpbuffers.h"
#include "util.h"

void inpb_write(struct inputbuffer *buffer, int write_count, const uint64_t *data)
{
  assert(write_count <= INPB_SIZE);
  if (buffer->local_index + write_count > INPB_SIZE) {
    inpb_flush(buffer);
  }

  for (int i = 0; i < write_count; i++) {
    buffer->local_data[buffer->local_index + i] = data[i];
  }
  buffer->local_index += write_count;
}

void inpb_flush(struct inputbuffer *buffer)
{
  if (buffer->local_index == 0) return;

  while (buffer->data[0] != 0) {
    buffer->local_waitcnt++;
    _mm_pause();
  }

  for (int i = 1; i < buffer->local_index; i++) {
    buffer->data[i] = buffer->local_data[i];
  }
  if (buffer->local_index < INPB_SIZE) {
    buffer->data[buffer->local_index] = 0;
  }

   __sync_synchronize(); 
  buffer->data[0] = buffer->local_data[0];
  buffer->local_index = 0;
}

void inpb_prefetch(struct inputbuffer *buffer)
{
  __builtin_prefetch((const void *)buffer->data, 1, 3);
}

int inpb_read(struct inputbuffer *buffer, uint64_t *data)
{
  int count = 0;
  while ((count < INPB_SIZE) && (buffer->data[count] != 0)) {
    data[count] = buffer->data[count];
    count++;
  }    
  if (count != 0) buffer->data[0] = 0;
  return count;
}

void outb_write(struct outputbuffer *buffer, int write_count, const uint64_t *data) 
{
  assert(write_count <= OUTB_SIZE);
  for (int i = 0; i < write_count; i++) {
    buffer->data[(buffer->wr_index + i) & (OUTB_SIZE - 1)] = data[i];
  }
  __sync_synchronize(); 
  buffer->wr_index += write_count;
}

void outb_prefetch(struct outputbuffer *buffer) {
  //__builtin_prefetch((const void *)&buffer->data[buffer->wr_index & (OUTB_SIZE - 1)], 1, 3);
  //__builtin_prefetch((const void *)&buffer->data[(buffer->wr_index + INPB_SIZE) & (OUTB_SIZE - 1)], 1, 3);
}

int outb_read(struct outputbuffer *buffer, uint64_t *data)
{
  if (buffer->local_rd_index == buffer->local_wr_index) {
    buffer->local_wr_index = buffer->wr_index;
  }
  
  if (buffer->local_rd_index == buffer->local_wr_index) {
    return 0;
  } else {
    *data = buffer->data[buffer->local_rd_index & (OUTB_SIZE - 1)];
    buffer->local_rd_index++;
    return 1;
  }
}

uint64_t outb_blocking_read(struct outputbuffer *buffer)
{
  uint64_t data;
  while (outb_read(buffer, &data) == 0) {
    buffer->local_waitcnt++;
    _mm_pause();
  }
  return data;
}
