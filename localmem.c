#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "localmem.h"
#include "util.h"

/* All blocks must be allocated on double-word boundaries. */
#define ALIGNMENT CACHELINE

/* Rounds up to the nearest multiple of ALIGNMENT. */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

/* Allocated Block Header Size */
#define BLOCK_HEADER_SIZE (2 * sizeof(size_t) + sizeof(unsigned long) + sizeof(struct localmem *))

/* Smallest Block Size */
#define MIN_BLOCK_SIZE ALIGN(BLOCK_HEADER_SIZE) 

/* Maximum through how many blocks will i brute force search */
#define MAX_BRUTE_SEARCH 10

/* Until first write, newly allocated block is not ready to be read */
#define USED_BLOCK_REFCOUNT   0x8000000000000000
#define DATA_READY_MASK       0x4000000000000000

struct mem_block {
  volatile size_t size;
  volatile uint64_t ref_count;
  union {
    struct {
      volatile mem_block_t list_next;
      volatile mem_block_t list_prev;
      volatile mem_block_t async_list_next;
    };
    struct {
      struct localmem *mem;
      char data_ptr[0];
    };
  };
};

/* precomputed array of msbs for numbers from 0 to 255 */
const char precomputed_bin_from_size[256] = {
  -1, 0,
  1, 1,
  2, 2, 2, 2,  
  3, 3, 3, 3, 3, 3, 3, 3, 
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 
  5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 
  7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 
  7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 
  7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 
  7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7 
};

/*
 * Return bin that block of "size" should be placed into.
 * Calculates MSB using precalculated table to speed up performance.
 */
inline int get_bin_from_size(size_t size) {
  if (size >= (1 << NUM_BINS)) return NUM_BINS - 1;

  int bin_index = 0;
  while (size > 0xFF)
  {
    size >>= 8;
    bin_index += 8;
  }
  bin_index += precomputed_bin_from_size[size];
  return bin_index;
}

/*
 * Memory Block Functions 
 */
inline void set_block_size(mem_block_t block, size_t size)
{
  block->size = size;
  *(size_t *)((unsigned long)block + size - sizeof(size_t)) = size;
}

inline mem_block_t get_block_from_data_ptr(void* ptr)
{
  return 
    (mem_block_t)((unsigned long)ptr - sizeof(struct localmem *) - sizeof(unsigned long) - sizeof(size_t));
}

void alloc_block(struct localmem *mem, mem_block_t block, size_t size)
{
  block->ref_count = 0;
  set_block_size(block, size);
  block->list_prev = NULL;
  block->async_list_next = NULL;

  int bin_index = get_bin_from_size(size);
  mem_block_t next_block = mem->bins[bin_index];
  mem->bins[bin_index] = block;

  block->list_next = next_block;
  if (next_block != NULL)
  {
    next_block->list_prev = block;
  }
}

void unlink_block(struct localmem *mem, mem_block_t block)
{
  if (block->list_prev != NULL)
  {
    block->list_prev->list_next = block->list_next;
  }else {
    mem->bins[get_bin_from_size(block->size)] = block->list_next;
  }

  if (block->list_next != NULL)
  {
    block->list_next->list_prev = block->list_prev;
  }
}

mem_block_t get_adjacent_prev(struct localmem *mem, mem_block_t block)
{
  if ((void*)block == mem->startaddr) return NULL;
  
  size_t prev_block_size = *(size_t *)((unsigned long)block - sizeof(size_t));
  return (mem_block_t)((unsigned long)block - prev_block_size);
}

mem_block_t get_adjacent_next(struct localmem *mem, mem_block_t block)
{
  void *next_block = (void *)((unsigned long)block + block->size);
  if (next_block == mem->endaddr) return 0;
  return (mem_block_t)next_block;
}

/*
 * Local Memory Allocator Functions
 */

// Forward Declarations
void localmem_free_all(struct localmem *mem);
static inline void mm_check(struct localmem *mem);

void localmem_init(struct localmem *mem, size_t size) 
{
  size = ALIGN(size);

  mem->startaddr = memalign(ALIGNMENT, size);
  mem->endaddr = mem->startaddr + size;
  mem->async_free_list = NULL;
  
  memset(mem->bins, 0, NUM_BINS * sizeof(mem_block_t));

  alloc_block(mem, (mem_block_t)mem->startaddr, size);
}

void localmem_destroy(struct localmem *mem) 
{
  localmem_free_all(mem);

  // make sure all blocks are free at the end
#if 0
  if (((mem_block_t)mem->startaddr)->size != (mem->endaddr - mem->startaddr)) {
    mem_block_t b = (mem_block_t)mem->startaddr;
    while (b != NULL) {
      printf("%zu - %lx, ", b->size, b->ref_count);
      b = get_adjacent_next(mem, b);
    }
    printf("\n");
  }
#endif
  assert(((mem_block_t)mem->startaddr)->size == (mem->endaddr - mem->startaddr));
  assert(((mem_block_t)mem->startaddr)->ref_count == 0);

  free(mem->startaddr);
}

void* localmem_alloc(struct localmem *mem, size_t size) 
{
  mm_check(mem);

  // perform all pending asynchrnous free operations
  if (mem->async_free_list != NULL) localmem_free_all(mem);

  size_t aligned_size = ALIGN(size + BLOCK_HEADER_SIZE);
  int bin_index = get_bin_from_size(aligned_size);

  // First try to find a block in the bin where it is not clear
  // if aligned_size block exists or not
  int cnt = 1;
  mem_block_t dst = mem->bins[bin_index];
  while (dst != NULL && aligned_size > dst->size) {
    if (cnt >= MAX_BRUTE_SEARCH) {
      // we cant go ahead and brute search whole list, stop
      // trying after we have tried MAX_BRUTE_SEARCH items in the
      // list
      dst = NULL;
      break;
    }

    cnt++;
    dst = dst->list_next;
  }

  if (dst == NULL) {
    // we could not find a block thus try bigger bins
    do {
      bin_index++;
      if (bin_index == NUM_BINS) {
        // there is no more space to allocate anything
        return NULL;
      }
    } while (mem->bins[bin_index] == NULL);
    dst = mem->bins[bin_index]; // destination block
  }

  // first unlink current block.
  unlink_block(mem, dst);

  // split current block into allocated block and leftover block
  size_t left_over_size = dst->size - aligned_size;
  if (left_over_size >= MIN_BLOCK_SIZE) {
    // only split it if left over block can actually be a block
    // and at least hold the header
    set_block_size(dst, aligned_size);
    alloc_block(mem, get_adjacent_next(mem, dst), left_over_size);
  }

  dst->mem = mem;
  dst->ref_count = USED_BLOCK_REFCOUNT | DATA_READY_MASK | 1; // put the block in use
  mem->memused += dst->size;

  mm_check(mem);
  return (void *)dst->data_ptr;
}

void localmem_free(struct localmem *mem, void *ptr) 
{
  mm_check(mem);

  // Get Block, and both adjacent blocks
  mem_block_t block = get_block_from_data_ptr(ptr);
  assert(block->ref_count == USED_BLOCK_REFCOUNT);
  mem_block_t prev_block = get_adjacent_prev(mem, block);
  mem_block_t next_block = get_adjacent_next(mem, block);

  // This will be the merged block
  mem_block_t merged_free_block = block;
  size_t merged_free_block_size = block->size;
  mem->memused -= block->size;

  if ((prev_block != NULL) && (prev_block->ref_count == 0))
  {
    // if prev_block is free then merge it
    merged_free_block = prev_block;
    merged_free_block_size += prev_block->size;
    unlink_block(mem, prev_block);
  }

  if ((next_block != NULL) && (next_block->ref_count == 0))
  {
    // if the next block is free then merge it
    merged_free_block_size += next_block->size;
    unlink_block(mem, next_block);
  }
  alloc_block(mem, merged_free_block, merged_free_block_size);

  mm_check(mem);
}

void localmem_free_all(struct localmem *mem)
{
  mm_check(mem);

  mem_block_t block = __sync_fetch_and_and(&mem->async_free_list, 0x0);
  while (block != 0x0) {
    mem_block_t next = block->async_list_next;
    localmem_free(mem, (void *)block->data_ptr);
    block = next;
  }

  mm_check(mem);
}

void localmem_async_free(struct localmem *mem, void *ptr)
{
  mem_block_t block = get_block_from_data_ptr(ptr);
  mem_block_t next;
  
  do {
    next = mem->async_free_list;
    block->async_list_next = next;
  } while (__sync_bool_compare_and_swap(&mem->async_free_list, next, block) == 0);
}

void localmem_retain(void *ptr)
{
  mem_block_t block = get_block_from_data_ptr(ptr);
  __sync_add_and_fetch(&(block->ref_count), 1);
}

void localmem_release(void *ptr, int async_free)
{
  mem_block_t block = get_block_from_data_ptr(ptr);
  uint64_t ref_count = __sync_sub_and_fetch(&(block->ref_count), 1);
  if (ref_count == USED_BLOCK_REFCOUNT) {
    if (async_free == 0) {
      localmem_free(block->mem, ptr);
    } else {
      localmem_async_free(block->mem, ptr);
    }
  }
}

void localmem_mark_ready(void *ptr)
{
  mem_block_t block = get_block_from_data_ptr(ptr);
  uint64_t ref_count = __sync_and_and_fetch(&(block->ref_count), (USED_BLOCK_REFCOUNT | (DATA_READY_MASK - 1)));
  if (ref_count == USED_BLOCK_REFCOUNT) {
    localmem_async_free(block->mem, ptr);
  }
}

int localmem_is_ready(void *ptr)
{
  mem_block_t block = get_block_from_data_ptr(ptr);
  return (block->ref_count & DATA_READY_MASK) == 0 ? 1 : 0;
}

/*
 * mm_check() section
 * This checking functions are unoptimized and largely for testing
 * purposes. The operation should be self-explanatory.
 */

// Util function to print mm_check error messages
inline void mm_check_err_msg(mem_block_t block_ptr, char* str) {
  printf("LOCALMEM ERROR [%p]: %s\n", block_ptr, str); 
}

// Check the list for consistency with the memory
int mm_check_free_list(struct localmem *mem) {
  int bin_index;
  for (bin_index = 0; bin_index < NUM_BINS; ++bin_index) {
    mem_block_t block_ptr = mem->bins[bin_index];
    while (block_ptr != 0) {
      if ((void *)block_ptr < mem->startaddr || (void *)block_ptr >= mem->endaddr) { // Check for a pointer that points off the heap
        mm_check_err_msg(block_ptr, "Block pointer off of the heap.");
        return 0;    
      }
      if (block_ptr->ref_count != 0) { // Check for things on the free list that aren't marked free
        mm_check_err_msg(block_ptr, "Block is in the free list but not marked as free.");
        return 0;
      }
      block_ptr = block_ptr->list_next;
    }
  }
  return 1;
}

// Check the memory for self-consistency and consistency with the free lists
int mm_check_heap(struct localmem *mem) {
  mem_block_t block_ptr = (mem_block_t)mem->startaddr;
  mem_block_t end_ptr = (mem_block_t)(mem->endaddr);
  //unsigned int cur_size;
  int last_block_free = 0;
  while (block_ptr < end_ptr) {
    if ((unsigned long)block_ptr & (ALIGNMENT - 1)) {
      mm_check_err_msg(block_ptr, "Block not aligned to predefined ALIGNMENT.");
      return 0;
    }

    if (*(size_t *)((unsigned long)block_ptr + block_ptr->size - sizeof(size_t)) != 
        block_ptr->size) {
      mm_check_err_msg(block_ptr, "Block size at the end of block is invalid.");
      return 0;
    }

    int cur_block_free = block_ptr->ref_count == 0;
    if (last_block_free && cur_block_free) { // we have two contiguous free blocks
      mm_check_err_msg(block_ptr, "Two consecutive free blocks (this and the previous block)");
      return 0;
    }
    int msb = get_bin_from_size(block_ptr->size);
    // check if this thing is in the free list
    mem_block_t free_list_ptr = mem->bins[msb];
    int found = 0;
    while (free_list_ptr != 0) {
      if (free_list_ptr == block_ptr) {
        if (cur_block_free) {
          found = 1;
          break;
        } else {
          mm_check_err_msg(block_ptr, "Block not marked as free but on the free list"); // TODO: possibly redundant?
          return 0;
        }
      }
      free_list_ptr = free_list_ptr->list_next;
    }
    if (!found && cur_block_free) {
      mm_check_err_msg(block_ptr, "Block is marked as free but not in the free list");
      return 0;
    }
    last_block_free = cur_block_free;
    block_ptr = (mem_block_t)((unsigned long)block_ptr + block_ptr->size);
  }

  if (block_ptr != end_ptr) {
    mm_check_err_msg(block_ptr, "Overlapping blocks, missing blocks, or corrupted size fields");
    return 0;
  }

  return 1;
}

static inline void mm_check(struct localmem *mem) {
#ifdef MM_CHECK
  int r;
  r = mm_check_free_list(mem);
  assert(r == 1);
  r = mm_check_heap(mem);
  assert(r == 1);
#endif
}
