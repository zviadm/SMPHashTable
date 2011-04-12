#ifndef __SMPHASHTABLE_H_
#define __SMPHASHTABLE_H_

#include "util.h"

/**
 * hash_key - Hash table key type
 */
typedef unsigned long hash_key;

enum optype {
  OPTYPE_LOOKUP = 0,
  OPTYPE_INSERT = 1
};

/**
 * struct hash_query - Hash table query
 * @optype: 0 - lookup, 1 - insert
 * @size: size of data to insert
 * @key: key to lookup or insert
 */
struct hash_query {
  enum optype optype;
  int size;
  hash_key key;
};

/**
 * struct hash_table
 */
struct hash_table;

/**
 * create_hash_table - Create new smp hash table
 * @max_size: maximum size in bytes that hash table can occupy
 * @nelems: maximum number of elements in hash table
 * @nservers: number of servers that serve hash content
 * @return: pointer to the created hash table
 */
struct hash_table *create_hash_table(size_t max_size, int nelems, int nservers); 

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
 * start_hash_table_servers and stop_hash_table_servers must be called from
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
 * @return: pointer to hash_value structure holding value, or NULL if there
 * was no entry in hash table with given key
 */ 
void * smp_hash_lookup(struct hash_table *hash_table, int client_id, hash_key key);

/**
 * smp_hash_insert: Insert key/value pair in hash table
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @key: hash key
 * @size: size of data to insert
 * @return: pointer to newly allocated space of given size which client should
 * fill up with data
 */
void * smp_hash_insert(struct hash_table *hash_table, int client_id, hash_key key, int size);

/**
 * smp_hash_doall: Perform batch hash table queries
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @nqueries: number of queries
 * @queries: hash table quries
 * @values: array to return results of queries
 *
 * Performing queries in batch is much faster then doing them one by one using
 * functions smp_hash_lookup and smp_hash_insert
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
 * stats_get_nhits: Get total number of hash requests that were hits
 */
int stats_get_nhits(struct hash_table *hash_table);

/**
 * stats_get_overhead: Returns total memory in bytes that is used to
 * store hash table structures
 */
size_t stats_get_overhead(struct hash_table *hash_table);

void stats_get_extreme_buckets(struct hash_table *hash_table, int server, double *avg, double *stddev);

#endif
