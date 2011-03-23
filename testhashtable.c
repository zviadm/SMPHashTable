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
  long val;
  printf("Creating Client...\n");
  int c = create_hash_table_client(table);
  
  printf("Inserting Elements...\n");
  val = 1;
  smp_hash_insert(table, c, 123, 8, (char *)&val);
  val = 2;
  smp_hash_insert(table, c, 1234, 8, (char *)&val);
  val = 3;
  smp_hash_insert(table, c, 12345, 8, (char *)&val);

  struct hash_value *value;

  value = smp_hash_lookup(table, c, 123);
  assert(value->size == 8);
  assert((long)(*value->data) == 1);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 1234);
  assert(value->size == 8);
  assert((long)(*value->data) == 2);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 12345);
  assert(value->size == 8);
  assert((long)(*value->data) == 3);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 122);
  assert(value == NULL);

  value = smp_hash_lookup(table, c, 123);
  assert(value->size == 8);
  assert((long)(*value->data) == 1);
  release_hash_value(value);

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 1 Done! -----------\n");
}

int main(int argc, char *argv[])
{
  test1();
  return 0;
}
