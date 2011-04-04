#ifndef __LOCALMEM_H_
#define __LOCALMEM_H_

/* Total number of bins */
#define NUM_BINS 20

/* memory block type which consists of:
 *  block_size << 32 + ref_count
 *  next block
 *  prev block
 *  unused
 *  ... block data ...
 *  block_size
 * */
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
void * localmem_alloc(struct localmem *mem, size_t size);

/*
 * localmem_retain - Retain block allocated in local memory
 */
void localmem_retain(void *ptr);

/*
 * localmem_release - Releases and if necessary frees block allocated in local memory
 */
void localmem_release(void *ptr);

/*
 * localmem_async_release - Releases and if necessary asynchrnously schedules block to be 
 * freed. Scheduled blocks will be freed before next allocation.
 */
void localmem_async_release(void *ptr);

void localmem_mark_ready(void *ptr);
int localmem_is_ready(void *ptr);

#endif
