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

/*
 * localmem_retain - Retain block allocated in local memory
 */
void localmem_retain(void *ptr);

/*
 * localmem_release - Releases and if necessary frees block allocated in local memory
 */
void localmem_release(void *ptr, int async_free);

/*
 * localmem_mark_ready - mark allocated memory block as ready to use
 */
void localmem_mark_ready(void *ptr);

/*
 * localmem_is_ready - returns 1 if memory block is ready to use
 * 0 otherwise
 */
int localmem_is_ready(void *ptr);

#endif
