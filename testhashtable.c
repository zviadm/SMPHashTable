#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "smphashtable.h"

void test1() 
{
  printf("----------- Test 1 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(1024, 2);
  printf("Starting Servers...\n");
  start_hash_table_servers(table, 0);

  // Do some stuff
  printf("Creating Client...\n");
  int c = create_hash_table_client(table);
  
  printf("Inserting Elements...\n");
  long val;
  val = 1;
  smp_hash_insert(table, c, 123, 8, (char *)&val);
  val = 2;
  smp_hash_insert(table, c, 1234, 8, (char *)&val);
  val = 3;
  smp_hash_insert(table, c, 12345, 8, (char *)&val);

  printf("Looking up Elements...\n");
  struct hash_value *value;
  value = smp_hash_lookup(table, c, 123);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 1);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 1234);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 2);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 12345);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 3);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 122);
  assert(value == NULL);

  value = smp_hash_lookup(table, c, 123);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 1);
  assert(value->ref_count == 2);

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  assert(value->ref_count == 1);
  release_hash_value(value);

  printf("----------- Test 1 Done! -----------\n");
}

void test2() 
{
  printf("----------- Test 2 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(16384, 2);
  int max_count = 16384 / 2 / (sizeof(hash_key) + sizeof(struct hash_value) + 8);
  printf("Starting Servers...\n");
  start_hash_table_servers(table, 0);

  // Do some stuff
  printf("Creating Client...\n");
  int c = create_hash_table_client(table);
  
  printf("Inserting Elements...\n");
  for (long i = 0; i < max_count; i++) {
    smp_hash_insert(table, c, i << 1, 8, (char *)&i);
  }

  printf("Looking up Elements...\n");
  struct hash_value *value;
  for (long i = 0; i < max_count; i++) {
    value = smp_hash_lookup(table, c, i << 1);
    assert(value->size == 8);
    assert(*(long *)(value->data) == i);
    release_hash_value(value);
  }

  printf("Inserting Extra Element...\n");
  long val = max_count;
  smp_hash_insert(table, c, val << 1, 8, (char *)&val);
  
  value = smp_hash_lookup(table, c, 0 << 1);
  assert(value == NULL);

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 2 Done! -----------\n");
}

void test3() 
{
  printf("----------- Test 3 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(16384, 2);
  printf("Starting Servers...\n");
  start_hash_table_servers(table, 0);

  // Do some stuff
  printf("Creating Client 1...\n");
  int c1 = create_hash_table_client(table);
  printf("Creating Client 2...\n");
  int c2 = create_hash_table_client(table);
  
  printf("Inserting Elements using Client 1...\n");
  long val;
  val = 0xDEADBEEF;
  smp_hash_insert(table, c1, 123, 8, (char *)&val);
  val = 0xFACEDEAD;
  smp_hash_insert(table, c1, 1234, 8, (char *)&val);

  // make sure hash insert is completed
  struct hash_value *value;
  value = smp_hash_lookup(table, c1, 123);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 0xDEADBEEF);
  release_hash_value(value);

  value = smp_hash_lookup(table, c1, 1234);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 0xFACEDEAD);
  release_hash_value(value);

  printf("Looking up Elements using Client 2...\n");
  value = smp_hash_lookup(table, c2, 123);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 0xDEADBEEF);
  release_hash_value(value);

  value = smp_hash_lookup(table, c2, 1234);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 0xFACEDEAD);
  release_hash_value(value);
  
  printf("Rewriting Element using Client 2...\n");
  val = 0xACEACEACE;
  smp_hash_insert(table, c2, 123, 8, (char *)&val);

  // make sure hash insert is completed
  value = smp_hash_lookup(table, c2, 123);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 0xACEACEACE);
  release_hash_value(value);

  printf("Looking up Element using Client 1...\n");
  value = smp_hash_lookup(table, c2, 123);
  assert(value->size == 8);
  assert(*(long *)(value->data) == 0xACEACEACE);
  release_hash_value(value);

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 3 Done! -----------\n");
}

int main(int argc, char *argv[])
{
  test1();
  test2();
  test3();
  return 0;
}
