#ifndef __LOCALMEM_H_
#define __LOCALMEM_H_

/* Total number of bins */
#define NUM_BINS 20

/* memory block type which consists of:
 *  block_size << 1 + in_use
 *  next block
 *  prev block
 *  unused
 *  ... block data ...
 *  block_size
 * */
typedef size_t* mem_block_t;

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
 * localmem_init - Initializes the smpalloc package. 
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
void* localmem_alloc(struct localmem *mem, size_t size);

/*
 * localmem_free - Frees block allocated in local memory
 */
void localmem_free(struct localmem *mem, void *ptr);

/*
 * localmem_async_free - Asynchrnously schedules block to be freed.
 * Scheduled blocks will be freed before next allocation
 */
void localmem_async_free(struct localmem *mem, void *ptr);

#endif
