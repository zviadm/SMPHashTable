#include <assert.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xmmintrin.h>

#include "localmem.h"
#include "smphashtable.h"
#include "util.h"

int design          = 2;
int nservers        = 1;
int nclients        = 1;
int batch_size      = 1000;
int nelems          = 100000;
size_t size         = 6400000;
struct hash_table *hash_table;

struct hash_value {
  int size;
  char data[0];
};

struct thread_args {
  int cid;
  int core;
  volatile int socket;
};

struct thread_args args[100];
volatile int active_clients = 0;
volatile double start_time, end_time;

// Forward Declarations
void run_server();
void * tcpserver(void *xarg);
void * tcpgo(void *xarg);
void doqueries(int cid, int nqueries, struct hash_query *queries, void **values);

int main(int argc, char *argv[])
{
  int opt_char;

  while((opt_char = getopt(argc, argv, "s:c:n:t:d:b:")) != -1) {
    switch (opt_char) {
      case 's':
        nservers = atoi(optarg);
        break;
      case 'c':
        nclients = atoi(optarg);
        break;
      case 'n':
        nelems = atoi(optarg);
        break;
      case 't':
        size = atol(optarg);
        break;
      case 'd':
        design = atoi(optarg);
        break;
      case 'b':
        batch_size = atoi(optarg);
        break;
      default:
        printf("hash server options are: \n"
               "   -d design (1 = naive server/client, 2 = buffering server/client, 3 = locking)\n"
               "   -s number of servers / partitions\n"
               "   -c maximum number of clients\n"
               "   -b batch size\n"
               "   -n max number of elements in cache\n"
               "   -t max size of cache (in bytes)\n");
        exit(-1);
    }
  }

  run_server();
}

void print_stats()
{
  int nhits = stats_get_nhits(hash_table);
  int nlookups = stats_get_nlookups(hash_table);
  int ninserts = stats_get_ninserts(hash_table);

  printf("nhits: %d, nlookups: %d, ninserts: %d, nhits/nlookups: %.3f, ninserts/total:%.3f\n", 
      nhits, nlookups, ninserts, (double)nhits / nlookups, (double)ninserts / (nlookups + ninserts));
}

void run_server() 
{
  hash_table = create_hash_table(size, nelems, nservers);

  if (design == 1 || design == 2) {
    start_hash_table_servers(hash_table, 0);
  }

  // Create all clients 
  assert(nclients <= 100);
  for (int i = 0; i < nclients; i++) {
    args[i].cid = (design == 1 || design == 2) ? create_hash_table_client(hash_table) : 0;
    args[i].core = (design == 1 || design == 2) ? nservers + i : i;
    args[i].socket = -1;
  }

  printf("Starting Hash Server...\n"); 
  printf("design: %d, nservers: %d, nclients: %d, partition: %zu(bytes)\n", 
      design, nservers, nclients, stats_get_overhead(hash_table) / nservers);

  // start thread to listen on tcp port
  pthread_t th;
  int ret = pthread_create(&th, 0, tcpserver, NULL);
  assert(ret == 0);
  pthread_detach(th);

  // start the interactive mode
  char cmd[100];
  while (1) {
    printf("%%> ");
    fflush(stdout);
    char *p = fgets(cmd, 100, stdin);
    if (p != cmd) {
      printf("invalid input\n");
      continue;
    }

    if ((strcmp(cmd, "stats\n") == 0) || (strcmp(cmd, "s\n") == 0)) {
      print_stats();
    } else if ((strcmp(cmd, "reset\n") == 0) || (strcmp(cmd, "r\n") == 0)) {
      print_stats();
      stats_reset(hash_table);
    } else if ((strcmp(cmd, "quit\n") == 0) || (strcmp(cmd, "q\n") == 0)) {
      break;
    } else if (strcmp(cmd, "\n") != 0) {
      printf("unknown command\n");
    }
  }

  if (design == 1 || design == 2) {
    stop_hash_table_servers(hash_table);
  } 
  destroy_hash_table(hash_table);
}

void * tcpserver(void *xarg) 
{
  struct sockaddr_in sin;
  int yes = 1;
  int ret;

  bzero(&sin, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(2117);

  // TCP socket and threads
  int s = socket(AF_INET, SOCK_STREAM, 0);
  assert(s >= 0);
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  ret = bind(s, (struct sockaddr *) &sin, sizeof(sin));
  if(ret < 0){
    perror("bind");
    exit(1);
  }

  listen(s, 100);
  sigset(SIGPIPE, SIG_IGN);

  while (1) {
    long s1;
    struct sockaddr_in sin1;
    socklen_t sinlen = sizeof(sin1);

    bzero(&sin1, sizeof(sin1));
    s1 = accept(s, (struct sockaddr *) &sin1, &sinlen);
    assert(s1 >= 0);
    setsockopt(s1, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    int i = 0;
    while (args[i].socket != -1) {
      i = (i + 1) % nclients;
    }
    args[i].socket = s1;

    pthread_t tcpth;
    ret = pthread_create(&tcpth, 0, tcpgo, (void *)&args[i]);
    assert(ret == 0);
    pthread_detach(tcpth);
  }
}

// serve a client tcp socket, in a dedicated thread
void * tcpgo(void *xarg)
{
  
  size_t r;
  int cid = ((struct thread_args *)xarg)->cid;
  int s = ((struct thread_args *)xarg)->socket;
  set_affinity(((struct thread_args *)xarg)->core);

  int clients = __sync_add_and_fetch(&active_clients, 1);
  if (clients == 1) {
    printf("\n");
    start_time = now();
  }
  printf("Client connected cid: %d, socket: %d, total clients %d\n", cid, s, clients);
  
  FILE *fin = fdopen(s, "r");
  FILE *fout = fdopen(s, "w");
  if (fin == NULL || fout == NULL) {
    perror("fdopen");
    return NULL;
  }
    
  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  void **values = (void **)memalign(CACHELINE, batch_size * sizeof(void *));

  while (1) {
    int nqueries;
    r = fread(&nqueries, sizeof(int), 1, fin);
    if (feof(fin) || ferror(fin)) break;

    if (r != 1) {
      printf("ERROR: client cid: %d failed reading nqueries\n", cid);
      break; 
    }
    assert(nqueries <= batch_size);

    r = fread(queries, sizeof(struct hash_query), nqueries, fin);
    if (r != nqueries) {
      printf("ERROR: client cid: %d failed reading queries\n", cid);
      break; 
    }

    doqueries(cid, nqueries, queries, values);  

    for (int k = 0; k < nqueries; k++) {
      struct hash_value *val = values[k];

      if (queries[k].optype == OPTYPE_LOOKUP) {
        int size = (val == NULL) ? 0 : val->size;
        r = fwrite(&size, sizeof(int), 1, fout);

        if (size != 0) {
          r = fwrite(val->data, 1, size, fout);
          localmem_release(val, 1);     
        } 
      } else if (queries[k].optype == OPTYPE_INSERT) {
        assert(val != NULL);
        val->size = queries[k].size - sizeof(int);
        r = fread(val->data, 1, val->size, fin);
        localmem_mark_ready(val);
      } else {
        assert(0);
      }
    }
    fflush(fout);
  }

  free(queries);
  free(values);
  fclose(fin);
  fclose(fout);
  close(s);

  clients = __sync_sub_and_fetch(&active_clients, 1);
  printf("Client disconnected cid: %d, socket: %d, total clients %d\n", cid, s, clients);
  if (clients == 0) {
    end_time = now();
    printf("TIME: %.3f\n", end_time - start_time);
    printf("%%> ");
    fflush(stdout);
  }
  
  ((struct thread_args *)xarg)->socket = -1;
  return NULL;
}

void doqueries(int cid, int nqueries, struct hash_query *queries, void **values)
{
  // increase size of each value by sizeof(int) since we will store
  // size of value + value in hash table
  for (int k = 0; k < nqueries; k++) {
    queries[k].size += sizeof(int);
  }

  if (design == 1) {
    for (int k = 0; k < nqueries; k++) {
      if (queries[k].optype == OPTYPE_LOOKUP) {
        values[k] = smp_hash_lookup(hash_table, cid, queries[k].key);
      } else {
        values[k] = smp_hash_insert(hash_table, cid, queries[k].key, queries[k].size);
      }
    }
  } else if (design == 2) {
    smp_hash_doall(hash_table, cid, nqueries, queries, values);
  } else if (design == 3) {
    for (int k = 0; k < nqueries; k++) {
      if (queries[k].optype == OPTYPE_LOOKUP) {
        values[k] = locking_hash_lookup(hash_table, queries[k].key);
      } else {
        values[k] = locking_hash_insert(hash_table, queries[k].key, queries[k].size);
      }
    }
  }
}
