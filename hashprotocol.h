#ifndef __HASHPROTOCOL_H_
#define __HASHPROTOCOL_H_

#include <stdint.h>

/**
 * hash_key - Hash table key type
 */
typedef uint64_t hash_key;

/**
 * hash operations
 */
enum optype {
  OPTYPE_LOOKUP = 0,
  OPTYPE_INSERT = 1
};

/**
 * struct hash_query - Hash table query
 * @optype: operation
 * @size: size of data to insert
 * @key: key to lookup or insert
 */
struct hash_query {
  uint32_t optype;
  uint32_t size;
  hash_key key;
};

/**
 * struct client_query - Query for client library
 * in future this can be more different from hash_query,
 * hence the separate structure
 */
struct client_query {
  enum optype optype;
  hash_key key;
  size_t size;
  void *value;
};
#endif
