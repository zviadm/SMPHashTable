#ifndef __LOCALMEM_H_
#define __LOCALMEM_H_

/* 
 * Total number of bins. 
 * 1 << (NUM_BINS) should be used as upper limit for allocation size
 */
#define NUM_BINS 20

/* 
 * mem_block_t: memory block
 */
typedef struct mem_block* mem_block_t;

/*
 * struct localmem - Local Memory structure
 */
struct localmem {
  mem_block_t bins[NUM_BINS];
  volatile mem_block_t async_free_list;
  void *startaddr;
  void *endaddr;
  size_t memused;
};

/*
 * localmem_init - Initializes local memory. 
 * Must be called once before any other calls are made.
 */
void localmem_init(struct localmem *mem, size_t size);

/*
 * localmem_destroy - Destroys and frees up space used by local memory
*/
void localmem_destroy(struct localmem *mem);

/* 
 * localmem_alloc - Allocates the space of "size" in local memory
 */
void * localmem_alloc(struct localmem *mem, size_t size);

void localmem_free(void *ptr);
void localmem_async_free(void *ptr);

/**
 * localmem_used - returns number of bytes that are allocated in
 * local memory 
 */
static inline size_t localmem_used(struct localmem *mem) 
{
  return mem->memused;
}

#endif
