#ifndef __SMPHASHTABLE_H_
#define __SMPHASHTABLE_H_

/**
 * hash_key - Hash table key type
 */
typedef long hash_key;

/**
 * struct hash_value - Hash table value type
 * @ref_count: reference count
 * @size: size of data
 * @data: object data
 */
struct hash_value {
  int ref_count;
  size_t size;
  char data[0];
};

/**
 * struct hash_table
 */
struct hash_table;

/**
 * create_hash_table - Create new smp hash table
 * @max_size: maximum size of hash table in bytes
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
 *
 * after done using value, release_hash_value must be called to release
 * hash_value object
 */ 
struct hash_value * smp_hash_lookup(struct hash_table *hash_table, int client_id, hash_key key);

/**
 * smp_hash_insert: Insert key/value pair in hash table
 * @hash_table: pointer to the hash table structure
 * @client_id: client id to use to communicate with hash table servers
 * @key: hash key
 * @size: size of data
 * @data: pointer to data
 */
void smp_hash_insert(struct hash_table *hash_table, int client_id, hash_key key, size_t size, char *data);

struct hash_value * alloc_hash_value(size_t size, char *data);
void retain_hash_value(struct hash_value *value);
void release_hash_value(struct hash_value *value);

#endif
