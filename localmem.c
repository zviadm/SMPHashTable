#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "localmem.h"
#include "util.h"

//#define MM_CHECK // uncomment for runtime checking (super slow)

/* All blocks must be allocated on double-word boundaries. */
#define ALIGNMENT CACHELINE

/* Rounds up to the nearest multiple of ALIGNMENT. */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

/* Block Header Size */
#define BLOCK_HEADER_SIZE (5 * sizeof(size_t))

/* Smallest Block Size */
#define MIN_BLOCK_SIZE (2 * CACHELINE) 

/* Maximum through how many blocks will i brute force search */
#define MAX_BRUTE_SEARCH 10

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
  block[0] = (size << 1) + (block[0] & 1);
  block[size / sizeof(size_t) - 1] = size;
}

inline size_t get_block_size(mem_block_t block) {
  return (block[0] >> 1);
}

inline int is_block_used(mem_block_t block)
{
  return (block[0] & 1);
}

inline void set_block_used(mem_block_t block, int used)
{
  block[0] = ((block[0] >> 1) << 1) + (used & 1);
}

inline void set_link_next(mem_block_t block, mem_block_t next) {
  block[1] = (size_t)next;
}

inline void set_link_prev(mem_block_t block, mem_block_t prev) {
  block[2] = (size_t)prev;
}

inline mem_block_t get_link_next(mem_block_t block) {
  return (mem_block_t)block[1];
}

inline mem_block_t get_link_prev(mem_block_t block) {
  return (mem_block_t)block[2];
}

inline void set_async_free_list_next(mem_block_t block, mem_block_t next) {
  block[3] = (size_t)next;
}

inline mem_block_t get_async_free_list_next(mem_block_t block) {
  return (mem_block_t)block[3];
}

inline void* get_data_ptr(mem_block_t block) {
  return (void*)(&block[4]);
}

inline size_t get_data_size(mem_block_t block) {
  return get_block_size(block) - BLOCK_HEADER_SIZE;
}

inline mem_block_t get_block_from_data_ptr(void* ptr)
{
  return (mem_block_t)(((size_t*)ptr - 4));
}

void alloc_block(struct localmem *mem, mem_block_t block, size_t size)
{
  set_block_size(block, size);
  set_block_used(block, 0);
  set_link_prev(block, 0);
  int bin_index = get_bin_from_size(size);
  mem_block_t next_block = mem->bins[bin_index];
  set_link_next(block, next_block);
  if (next_block)
  {
    set_link_prev(next_block, block);
  }
  set_async_free_list_next(block, 0);
  mem->bins[bin_index] = block;
}

void unlink_block(struct localmem *mem, mem_block_t block)
{
  if (get_link_prev(block))
  {
    set_link_next(get_link_prev(block), get_link_next(block));
  }else {
    mem->bins[get_bin_from_size(get_block_size(block))] = get_link_next(block);
  }

  if (get_link_next(block))
  {
    set_link_prev(get_link_next(block), get_link_prev(block));
  }
}

mem_block_t get_adjacent_prev(struct localmem *mem, mem_block_t block)
{
  if ((void*)block == mem->startaddr) return 0;
 
  size_t prev_block_size = *((size_t*)(block - 1));
  return (mem_block_t)((uint8_t*)block - prev_block_size);
}

mem_block_t get_adjacent_next(struct localmem *mem, mem_block_t block)
{
  void *next_block = (((uint8_t*)block) + get_block_size(block));
  if (next_block == mem->endaddr) return 0;
  return (mem_block_t)next_block;
}

/*
 * Local Memory Allocator Functions
 */

// Forward Declarations
void localmem_free_all(struct localmem *mem);
int mm_check(struct localmem *mem);

void localmem_init(struct localmem *mem, size_t size) 
{
  size = ALIGN(size);

  mem->startaddr = memalign(CACHELINE, size);
  mem->endaddr = mem->startaddr + size;
  mem->async_free_list = 0;
  
  memset(mem->bins, 0, NUM_BINS * sizeof(mem_block_t));
  //memset(mem->startaddr, 0, size);

  alloc_block(mem, (mem_block_t)mem->startaddr, size);
}

void localmem_destroy(struct localmem *mem) 
{
  localmem_free_all(mem);
#ifdef MM_CHECK
  // make sure all blocks are free at the end
  if ((get_block_size((mem_block_t)mem->startaddr) != (mem->endaddr - mem->startaddr)) ||
      (is_block_used((mem_block_t)mem->startaddr))) {
    printf("LOCALMEM ERROR: not all blocks were freed\n");
    printf("Async free list: %p\n", mem->async_free_list);
    mem_block_t block = (mem_block_t)(mem->startaddr);
    while (block != 0) {
      printf("%p, %zu, %d\n", block, get_block_size(block), is_block_used(block)); 
      block = get_adjacent_next(mem, block);
    }
    printf("%zu - %zu\n", get_block_size((mem_block_t)mem->startaddr), (mem->endaddr - mem->startaddr));
  }
#endif
  free(mem->startaddr);
}

void* localmem_alloc(struct localmem *mem, size_t size) 
{
#ifdef MM_CHECK
  mm_check(mem);
#endif

  // perform all pending asynchrnous free operations
  if (mem->async_free_list != NULL) localmem_free_all(mem);

  size_t aligned_size = ALIGN(size + BLOCK_HEADER_SIZE);
  int bin_index = get_bin_from_size(aligned_size);

  // First try to find a block in the bin where it is not clear
  // if aligned_size block exists or not
  int cnt = 1;
  mem_block_t dst = mem->bins[bin_index];
  while (dst != 0 && aligned_size > get_block_size(dst)) {
    if (cnt >= MAX_BRUTE_SEARCH) {
      // we cant go ahead and brute search whole list, stop
      // trying after we have tried MAX_BRUTE_SEARCH items in the
      // list
      dst = 0;
      break;
    }

    cnt++;
    dst = get_link_next(dst);
  }

  if (dst == 0) {
    // we could not find a block thus try bigger bins
    do {
      bin_index++;
      if (bin_index == NUM_BINS) {
        // there is no more space to allocate anything
        return NULL;
      }
    } while (mem->bins[bin_index] == 0);
    dst = mem->bins[bin_index]; // destination block
  }

  // first unlink current block.
  unlink_block(mem, dst);

  // split current block into allocated block and leftover block
  size_t left_over_size = get_block_size(dst) - aligned_size;
  if (left_over_size >= MIN_BLOCK_SIZE) {
    // only split it if left over block can actually be a block
    // and at least hold the header
    set_block_size(dst, aligned_size);
    alloc_block(mem, get_adjacent_next(mem, dst), left_over_size);
  }
  set_block_used(dst, 1); // put the block in use
#ifdef MM_CHECK
  mm_check(mem);
#endif
  return get_data_ptr(dst);
}

void localmem_free(struct localmem *mem, void *ptr) 
{
#ifdef MM_CHECK
  mm_check(mem);
#endif

  // Get Block, and both adjacent blocks
  mem_block_t block = get_block_from_data_ptr(ptr);
  mem_block_t prev_block = get_adjacent_prev(mem, block);
  mem_block_t next_block = get_adjacent_next(mem, block);

  // This will be the merged block
  mem_block_t merged_free_block = block;
  size_t merged_free_block_size = get_block_size(block);

  if (prev_block && !is_block_used(prev_block))
  {
    // if prev_block is free then merge it
    merged_free_block = prev_block;
    merged_free_block_size += get_block_size(prev_block);
    unlink_block(mem, prev_block);
  }

  if (next_block && !is_block_used(next_block))
  {
    // if the next block is free then merge it
    merged_free_block_size += get_block_size(next_block);
    unlink_block(mem, next_block);
  }
  alloc_block(mem, merged_free_block, merged_free_block_size);

#ifdef MM_CHECK
  mm_check(mem);
#endif
}

void localmem_async_free(struct localmem *mem, void *ptr)
{
  mem_block_t block = get_block_from_data_ptr(ptr);
  mem_block_t next;
  
  do {
    next = mem->async_free_list;
    set_async_free_list_next(block, next);
  } while (__sync_bool_compare_and_swap(&mem->async_free_list, next, block) == 0);
}

void localmem_free_all(struct localmem *mem)
{
#ifdef MM_CHECK
  mm_check(mem);
#endif

  mem_block_t block = __sync_fetch_and_and(&mem->async_free_list, 0);
  while (block != NULL) {
    mem_block_t next = get_async_free_list_next(block);
    localmem_free(mem, get_data_ptr(block));
    block = next;
  }

#ifdef MM_CHECK
  mm_check(mem);
#endif
}

/*
 * mm_check() section
 * This checking functions are unoptimized and largely for testing
 * purposes. The operation should be self-explanatory.
 */

// Util function to print mm_check error messages
inline void mm_check_err_msg(size_t* block_ptr, char* str) {
  printf("LOCALMEM ERROR [%p]: %s\n", block_ptr, str); 
}

// Check the list for consistency with the memory
int mm_check_free_list(struct localmem *mem) {
  int bin_index;
  for (bin_index = 0; bin_index < NUM_BINS; ++bin_index) {
    size_t* block_ptr = mem->bins[bin_index];
    while (block_ptr != 0) {
      if (block_ptr < (size_t*)mem->startaddr || block_ptr >= (size_t*)mem->endaddr) { // Check for a pointer that points off the heap
        mm_check_err_msg(block_ptr, "Block pointer off of the heap.");
        return 0;    
      }
      if (is_block_used(block_ptr)) { // Check for things on the free list that aren't marked free
        mm_check_err_msg(block_ptr, "Block is in the free list but not marked as free.");
        return 0;
      }
      block_ptr = get_link_next(block_ptr);
    }
  }
  return 1;
}

// Check the memory for self-consistency and consistency with the free lists
int mm_check_heap(struct localmem *mem) {
  size_t* block_ptr = (size_t*)mem->startaddr;
  size_t* end_ptr = (size_t*)(mem->endaddr);
  size_t cur_size;
  int last_block_free = 0;
  while (block_ptr < end_ptr) {
    if ((unsigned long)block_ptr & (CACHELINE - 1)) {
      mm_check_err_msg(block_ptr, "Block not aligned to CACHELINE.");
      return 0;
    }
    int cur_block_free = !is_block_used(block_ptr);
    if (last_block_free && cur_block_free) { // we have two contiguous free blocks
      mm_check_err_msg(block_ptr, "Two consecutive free blocks (this and the previous block)");
      return 0;
    }
    cur_size = get_block_size(block_ptr);
    int msb = get_bin_from_size(cur_size);
    // check if this thing is in the free list
    size_t* free_list_ptr = mem->bins[msb];
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
      free_list_ptr = get_link_next(free_list_ptr);
    }
    if (!found && cur_block_free) {
      mm_check_err_msg(block_ptr, "Block is marked as free but not in the free list");
      return 0;
    }
    last_block_free = cur_block_free;
    block_ptr += cur_size / sizeof(size_t);
  }

  if (block_ptr != end_ptr) {
    mm_check_err_msg(block_ptr, "Overlapping blocks, missing blocks, or corrupted size fields");
    return 0;
  }

  return 1;
}

int mm_check(struct localmem *mem) {
  if (!mm_check_free_list(mem) || !mm_check_heap(mem)) {
    return 0;
  }
  /* printf("SUCCESS: Heap passes all tests.\n"); */
  return 1;
}
