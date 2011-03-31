#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smphashtable.h"

void assert_hash_value(struct hash_value *value, long val)
{
  long tmp;
  assert(value->size == 8);
  memcpy(&tmp, value->data, 8);
  assert(tmp == val);
}

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
  assert_hash_value(value, 1);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 1234);
  assert_hash_value(value, 2);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 12345);
  assert_hash_value(value, 3);
  release_hash_value(value);

  value = smp_hash_lookup(table, c, 122);
  assert(value == NULL);

  value = smp_hash_lookup(table, c, 123);
  assert_hash_value(value, 1);
  assert(value->ref_count == 2);
  release_hash_value(value);
  assert(value->ref_count == 1);

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 1 Done! -----------\n");
}

void test2() 
{
  printf("----------- Test 2 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(1024, 2);
  int max_count = 1024 / 2 / (2 * CACHELINE);
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
    assert_hash_value(value, i);
    release_hash_value(value);
  }

  printf("Inserting Extra Element...\n");
  long val = max_count;
  smp_hash_insert(table, c, max_count << 1, 8, (char *)&val);
  
  value = smp_hash_lookup(table, c, 0 << 1);
  assert(value == NULL);

  printf("Replacing Element with larger value...\n");
  long large_val[3] = {1, 2, 3};
  smp_hash_insert(table, c, max_count << 1, 8 + 256, (char *)large_val);
  
  value = smp_hash_lookup(table, c, 1 << 1);
  assert(value == NULL);
  value = smp_hash_lookup(table, c, 2 << 1);
  assert(value == NULL);
  value = smp_hash_lookup(table, c, 3 << 1);
  assert_hash_value(value, 3);
  release_hash_value(value);

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
  assert_hash_value(value, 0xDEADBEEF);
  release_hash_value(value);

  value = smp_hash_lookup(table, c1, 1234);
  assert_hash_value(value, 0xFACEDEAD);
  release_hash_value(value);

  printf("Looking up Elements using Client 2...\n");
  value = smp_hash_lookup(table, c2, 123);
  assert_hash_value(value, 0xDEADBEEF);
  release_hash_value(value);

  value = smp_hash_lookup(table, c2, 1234);
  assert_hash_value(value, 0xFACEDEAD);
  release_hash_value(value);
  
  printf("Rewriting Element using Client 2...\n");
  val = 0xACEACEACE;
  smp_hash_insert(table, c2, 123, 8, (char *)&val);

  // make sure hash insert is completed
  value = smp_hash_lookup(table, c2, 123);
  assert_hash_value(value, 0xACEACEACE);
  release_hash_value(value);

  printf("Looking up Element using Client 1...\n");
  value = smp_hash_lookup(table, c2, 123);
  assert_hash_value(value, 0xACEACEACE);
  release_hash_value(value);

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 3 Done! -----------\n");
}

void test4() 
{
  printf("----------- Test 4 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(256000, 2);
  printf("Starting Servers...\n");
  start_hash_table_servers(table, 0);

  // Do some stuff
  printf("Creating Client...\n");
  int c = create_hash_table_client(table);
 
  const int nqueries = 1000;
  struct hash_query queries[2 * nqueries];
  long data[nqueries];

  for (int i = 0; i < nqueries; i++) {
    data[i] = i;
  }

  for (int i = 0; i < nqueries; i++) {
    queries[i].optype = 1;
    queries[i].key = i;
    queries[i].size = 8;
    queries[i].data = (char*)&data[i];

    queries[nqueries + i].optype = 0;
    queries[nqueries + i].key = i;
  }

  printf("Performing All Queries...\n");
  struct hash_value * values[2 * nqueries];
  smp_hash_doall(table, c, 2 * nqueries, queries, values);

  printf("Checking All Values...\n");
  for (int i = 0; i < nqueries; i++) {
    assert(values[i] == NULL);
    assert_hash_value(values[nqueries + i], data[i]);
    release_hash_value(values[nqueries + i]);
  }

  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 4 Done! -----------\n");
}

void test5() 
{
  printf("----------- Test 5 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(1024, 2);

  // Do some stuff
  printf("Inserting Elements...\n");
  long val;
  val = 1;
  locking_hash_insert(table, 123, 8, (char *)&val);
  val = 2;
  locking_hash_insert(table, 1234, 8, (char *)&val);
  val = 3;
  locking_hash_insert(table, 12345, 8, (char *)&val);

  printf("Looking up Elements...\n");
  struct hash_value *value;
  value = locking_hash_lookup(table, 123);
  assert_hash_value(value, 1);
  release_hash_value(value);

  value = locking_hash_lookup(table, 1234);
  assert_hash_value(value, 2);
  release_hash_value(value);

  value = locking_hash_lookup(table, 12345);
  assert_hash_value(value, 3);
  release_hash_value(value);

  value = locking_hash_lookup(table, 122);
  assert(value == NULL);

  value = locking_hash_lookup(table, 123);
  assert_hash_value(value, 1);
  assert(value->ref_count == 2);
  release_hash_value(value);
  assert(value->ref_count == 1);

  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);


  printf("----------- Test 5 Done! -----------\n");
}

void test6() 
{
  printf("----------- Test 6 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(1024, 2);
  int max_count = 1024 / 2 / (2 * CACHELINE);

  // Do some stuff
  printf("Inserting Elements...\n");
  for (long i = 0; i < max_count; i++) {
    locking_hash_insert(table, i << 1, 8, (char *)&i);
  }

  printf("Looking up Elements...\n");
  struct hash_value *value;
  for (long i = 0; i < max_count; i++) {
    value = locking_hash_lookup(table, i << 1);
    assert_hash_value(value, i);
    release_hash_value(value);
  }

  printf("Inserting Extra Element...\n");
  long val = max_count;
  locking_hash_insert(table, max_count << 1, 8, (char *)&val);
  
  value = locking_hash_lookup(table, 0 << 1);
  assert(value == NULL);

  printf("Replacing Element with larger value...\n");
  long large_val[3] = {1, 2, 3};
  locking_hash_insert(table, max_count << 1, 8 + 256, (char *)large_val);
  
  value = locking_hash_lookup(table, 1 << 1);
  assert(value == NULL);
  value = locking_hash_lookup(table, 2 << 1);
  assert(value == NULL);
  value = locking_hash_lookup(table, 3 << 1);
  assert_hash_value(value, 3);
  release_hash_value(value);

  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 6 Done! -----------\n");
}

void test7() 
{
  printf("----------- Test 7 Start -----------\n");
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(16384, 2);

  // Do some stuff
  printf("Inserting Elements using Client 1...\n");
  long val;
  val = 0xDEADBEEF;
  locking_hash_insert(table, 123, 8, (char *)&val);
  val = 0xFACEDEAD;
  locking_hash_insert(table, 1234, 8, (char *)&val);

  // make sure hash insert is completed
  struct hash_value *value;
  value = locking_hash_lookup(table, 123);
  assert_hash_value(value, 0xDEADBEEF);
  release_hash_value(value);

  value = locking_hash_lookup(table, 1234);
  assert_hash_value(value, 0xFACEDEAD);
  release_hash_value(value);

  printf("Looking up Elements using Client 2...\n");
  value = locking_hash_lookup(table, 123);
  assert_hash_value(value, 0xDEADBEEF);
  release_hash_value(value);

  value = locking_hash_lookup(table, 1234);
  assert_hash_value(value, 0xFACEDEAD);
  release_hash_value(value);
  
  printf("Rewriting Element using Client 2...\n");
  val = 0xACEACEACE;
  locking_hash_insert(table, 123, 8, (char *)&val);

  // make sure hash insert is completed
  value = locking_hash_lookup(table, 123);
  assert_hash_value(value, 0xACEACEACE);
  release_hash_value(value);

  printf("Looking up Element using Client 1...\n");
  value = locking_hash_lookup(table, 123);
  assert_hash_value(value, 0xACEACEACE);
  release_hash_value(value);

  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("----------- Test 7 Done! -----------\n");
}
int main(int argc, char *argv[])
{
  test1();
  test2();
  test3();
  test4();
  test5();
  test6();
  test7();
  return 0;
}
