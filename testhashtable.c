#include <stdio.h>
#include <stdlib.h>
#include "smphashtable.h"

int main(int argc, char *argv[])
{
  printf("Creating Hash Table...\n");
  struct hash_table *table = create_hash_table(1024, 2);
  printf("Starting Servers...\n");
  start_hash_table_servers(table, 0);
  // Do some stuff
  printf("Stopping Servers...\n");
  stop_hash_table_servers(table);
  printf("Destroying Hash Table...\n");
  destroy_hash_table(table);

  printf("Done\n");
  return 0;
}
