#ifndef __SMPHASHTABLE_H_
#define __SMPHASHTABLE_H_

#include "hashprotocol.h"
#include "util.h"

/**
 * struct hash_table
 */
struct hash_table;

/**
 * create_hash_table - Create new smp hash table
 * @max_size: maximum size in bytes that hash table can occupy
 * @nservers: number of servers that serve hash content
 * @return: pointer to the created hash table
 */
struct hash_table *create_hash_table(size_t max_size, int nservers); 

/**
 * destroy_hash_table - Destroy smp hash table
 * @hash_table: pointer to the hash table structure
 */
void destroy_hash_table(struct hash_table *hash_table);

/*
 * start_hash_table_servers - Start up hash table server threads
 * @hash_table: pointer to the hash table structure
 * @first_core: specifies what cores to run servers on [first_core..firt_core+nservers-1]
 *
 * start_hash_table_servers and stop_hash_table_servers must be called from the
 * same thread.
 */
void start_hash_table_servers(struct hash_table *hash_table, int first_core);

/*
 * stop_hash_table_servers - Stop hash table server threads
 * @hash_table: pointer to the hash table structure
 */
void stop_hash_table_servers(struct hash_table *hash_table);

/*
 * create_hash_table_client - Create client to perform hash table operations
 * @hash_table: pointer to the hash table structure
 * @return: created client id
 */
int create_hash_table_client(struct hash_table *hash_table);

/**
 * smp_hash_lookup: Lookup key/value pair in hash table
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @key: hash key to lookup value for
 * @return: 1 for success, 0 on failure when the queue of pending requests is full
 */ 
int smp_hash_lookup(struct hash_table *hash_table, int client_id, hash_key key);

/**
 * smp_hash_insert: Insert key/value pair in hash table
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @key: hash key
 * @size: size of data to insert
 * @return: 1 for success, 0 on failure when the queue of pending requests is full
 */
int smp_hash_insert(struct hash_table *hash_table, int client_id, hash_key key, int size);

/**
 * smp_try_get_next: Try to get next value from scheduled pending requests
 * Returns 1 on success, 0 if there are no available pending requests.
 */
int smp_try_get_next(struct hash_table *hash_table, int client_id, void **value);

/**
 * smp_get_next: Forces to get next value from schedule pending requests
 * Returns 1 on success, 0 if there are no more pending requests.
 */
int smp_get_next(struct hash_table *hash_table, int client_id, void **value);

/**
 * smp_hash_doall: Perform batch hash table queries
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @nqueries: number of queries
 * @queries: hash table quries
 * @values: array to return results of queries
 */
void smp_hash_doall(struct hash_table *hash_table, int client_id, int nqueries, struct hash_query *queries, void **values);

/**
 * locking_hash_lookup: Lookup key/value pair in hash table
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @key: hash key to lookup value for
 * @return: pointer to hash_value structure holding value, or NULL if there
 * was no entry in hash table with given key
 *
 * locking_hash_lookup does look up in a hash without using hash servers or clients,
 * but instead using locks on partitions.
 * This function must not be called when hash servers are running.
 */
void * locking_hash_lookup(struct hash_table *hash_table, hash_key key);

/**
 * locking_hash_insert: Insert key/value pair in hash table
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @key: hash key
 * @size: size of data to insert
 * @return: pointer to newly allocated space of given size which client should
 * fill up with data
 *
 * locking_hash_insert does insert in a hash without using hash servers or clients,
 * but instead using locks on partitions.
 * This function must not be called when hash servers are running.
 */
void * locking_hash_insert(struct hash_table *hash_table, hash_key key, int size);

/**
 * mp_release_value, mp_mark_ready: Release given value pointer or mark it ready
 * using message passing. Only works in server/client version
 */
void mp_release_value(struct hash_table *hash_table, int client_id, void *ptr);
void mp_mark_ready(struct hash_table *hash_table, int client_id, void *ptr);

/**
 * atomic_release_value, atomic_mark_ready: Release given value pointer or mark it ready
 * using atomic operations. Only works in locking implementation.
 */
void atomic_release_value(void *ptr);
void atomic_mark_ready(void *ptr);

/**
 * Stats functions
 */
void stats_reset(struct hash_table *hash_table);
int stats_get_nhits(struct hash_table *hash_table);
int stats_get_nlookups(struct hash_table *hash_table);
int stats_get_ninserts(struct hash_table *hash_table);
size_t stats_get_overhead(struct hash_table *hash_table);
void stats_get_buckets(struct hash_table *hash_table, int server, double *avg, double *stddev);
void stats_get_mem(struct hash_table *hash_table, size_t *used, size_t *total, double *util);

#endif
