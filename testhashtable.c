#include <stdlib.h>

#include "smphashtable.h"

int
main(int argc, char *argv[])
{
  struct hash_table *table = create_hash_table(1024, 2);
  start_hash_table_servers(table, 0);
  // Do some stuff
  stop_hash_table_servers(table);
  destroy_hash_table(table);
}
