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

#endif
