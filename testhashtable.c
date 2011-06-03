#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smphashtable.h"

static inline void insert(struct hash_table *table, int use_locking, int c, hash_key key, long val)
{
  void * value;
  if (use_locking == 0) {
    int r = smp_hash_insert(table, c, key, 8);
    assert(r == 1);
    r = smp_get_next(table, c, &value);
    assert(r == 1);
  } else {
    value = locking_hash_insert(table, key, 8);
  }
  assert(value != NULL);
  *(long *)value = val;
  if (use_locking == 0) {
    mp_release_value(table, c, value);
  } else {
    atomic_mark_ready(value);
  }
}

static inline long lookup(struct hash_table *table, int use_locking, int c, hash_key key)
{
  void * value;
  if (use_locking == 0) {
    int r = smp_hash_lookup(table, c, key);
    assert(r == 1);
    r = smp_get_next(table, c, &value);
    assert(r == 1);
  } else {
    value = locking_hash_lookup(table, key);
  }
  if (value == NULL) { 
    return 0;
  } else {
    long val = *(long *)value;
    if (use_locking == 0) {
      mp_release_value(table, c, value);
    } else {
      atomic_release_value(value);
    }
    return val;
  }
}

void test1(int use_locking) 
{
  printf("----------- Test 1 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(65536, 2);
  int c = 0;
  if (use_locking == 0) {
    printf("Starting Servers...\n");
    start_hash_table_servers(table, 0);
  
    printf("Creating Client...\n");
    c = create_hash_table_client(table);
  }
  
  printf("Inserting Elements...\n");
  insert(table, use_locking, c, 123, 1);
  insert(table, use_locking, c, 1234, 2);
  insert(table, use_locking, c, 12345, 3);

  printf("Looking up Elements...\n");
  long value;
  value = lookup(table, use_locking, c, 123);
  assert(value == 1);

  value = lookup(table, use_locking, c, 1234);
  assert(value == 2);

  value = lookup(table, use_locking, c, 12345);
  assert(value == 3);

  value = lookup(table, use_locking, c, 122);
  assert(value == 0);

  value = lookup(table, use_locking, c, 123);
  assert(value == 1);

  if (use_locking == 0) {
    printf("Stopping Servers...\n");
    stop_hash_table_servers(table);
  }
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 1 Done! -----------\n");
}

void test2(int use_locking) 
{
  printf("----------- Test 2 Start -----------\n");
  printf("Creating Hash Table...\n");
  long max_count = 1024;
  struct hash_table *table = create_hash_table(2 * max_count * 128, 2);
  int c = 0;
  if (use_locking == 0) {
    printf("Starting Servers...\n");
    start_hash_table_servers(table, 0);

    printf("Creating Client...\n");
    c = create_hash_table_client(table);
  }
  
  printf("Inserting Elements...\n");
  for (long i = 0; i < max_count; i++) {
    insert(table, use_locking, c, i << 1, i);
  }

  printf("Looking up Elements...\n");
  long value;
  for (long i = 0; i < max_count; i++) {
    value = lookup(table, use_locking, c, i << 1);
    assert(value == i);
  }

  printf("Inserting Extra Element...\n");
  insert(table, use_locking, c, max_count << 1, max_count);
  
  value = lookup(table, use_locking, c, 0 << 1);
  assert(value == 0);

  for (long i = 1; i <= max_count; i++) {
    value = lookup(table, use_locking, c, i << 1);
    assert(value == i);
  }

  if (use_locking == 0) {
    printf("Stopping Servers...\n");
    stop_hash_table_servers(table);
  }
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 2 Done! -----------\n");
}

void test3(int use_locking) 
{
  printf("----------- Test 3 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(65536, 2);
  int c1 = 0;
  int c2 = 0;
  if (use_locking == 0) {
    printf("Starting Servers...\n");
    start_hash_table_servers(table, 0);
  
    printf("Creating Client 1...\n");
    c1 = create_hash_table_client(table);
    printf("Creating Client 2...\n");
    c2 = create_hash_table_client(table);
  }

  printf("Inserting Elements using Client 1...\n");
  insert(table, use_locking, c1, 123, 0xDEADBEEF);
  insert(table, use_locking, c1, 1234, 0xFACEDEAD);
  if (use_locking == 0) {
    mp_flush_releases(table, c1);
    usleep(1000000);
  }

  printf("Looking up Elements using Client 2...\n");
  long value;
  value = lookup(table, use_locking, c2, 123);
  assert(value == 0xDEADBEEF);

  value = lookup(table, use_locking, c2, 1234);
  assert(value == 0xFACEDEAD);
  
  printf("Rewriting Element using Client 2...\n");
  insert(table, use_locking, c2, 123, 0xACEACEACE);
  if (use_locking == 0) {
    mp_flush_releases(table, c2);
    usleep(1000000);
  }

  printf("Looking up Element using Client 1...\n");
  value = lookup(table, use_locking, c2, 123);
  assert(value == 0xACEACEACE);

  if (use_locking == 0) {
    printf("Stopping Servers...\n");
    stop_hash_table_servers(table);
  }
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 3 Done! -----------\n");
}

void test4() 
{
  printf("----------- Test 4 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(131072, 2);
  printf("Starting Servers...\n");
  start_hash_table_servers(table, 0);

  printf("Creating Client...\n");
  int c = create_hash_table_client(table);
 
  const long nqueries = 1000;
  struct hash_query queries[2 * nqueries];

  for (long i = 0; i < nqueries; i++) {
    queries[i].optype = OPTYPE_INSERT;
    queries[i].key = i;
    queries[i].size = 8;

    queries[nqueries + i].optype = OPTYPE_LOOKUP;
    queries[nqueries + i].key = i;
  }

  printf("Performing All Queries...\n");
  void * values[nqueries];
  smp_hash_doall(table, c, nqueries, queries, values);

  // write all values
  for (int i = 0; i < nqueries; i++) {
    assert(values[i] != NULL);
    *(long *)values[i] = i;
    mp_release_value(table, c, values[i]);
  }

  smp_hash_doall(table, c, nqueries, &queries[nqueries], values);

  printf("Checking All Values...\n");
  for (int i = 0; i < nqueries; i++) {
    assert(values[i] != NULL);
    assert(*(long*)values[i] == i);
    mp_release_value(table, c, values[i]);
  }

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 4 Done! -----------\n");
}

int main(int argc, char *argv[])
{
  test1(0);
  test1(1);
  test2(0);
  test2(1);
  test3(0);
  test3(1);
  test4();
  return 0;
}
