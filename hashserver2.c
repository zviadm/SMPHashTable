#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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

#include "localmem.h"
#include "smphashtable.h"
#include "util.h"

#define MAX_SOCKETS       128
#define VALUE_BUFFER_SIZE (8 * (1024 * 1024))
#define MAX_VALUE_SIZE    (1024 * 1024)

int design          = 2;
int nservers        = 1;
int nclients        = 1;
int batch_size      = 1000;
int nelems          = 100000;
size_t size         = 6400000;
struct hash_table *hash_table;
int port            = 2117;

struct hash_value {
  int size;
  char data[0];
};

struct client_data {
  int cid;
  int core;
  volatile int nsockets;
  volatile int sockets[MAX_SOCKETS];
  volatile FILE * fin[MAX_SOCKETS];
  volatile FILE * fout[MAX_SOCKETS];
};

struct client_data cdata[MAX_CLIENTS];
volatile int active_clients = 0;
volatile double start_time, end_time;

// Forward Declarations
void run_server();
void * tcpserver(void *xarg);
void * clientgo(void *xarg);
void doqueries(int cid, int nqueries, struct hash_query *queries, void **values);

int main(int argc, char *argv[])
{
  int opt_char;

  while((opt_char = getopt(argc, argv, "s:c:n:t:d:b:")) != -1) {
    switch (opt_char) {
      case 's':
        nservers = atoi(optarg);
        break;
      case 'p':
        port = atoi(optarg);
        break;
      case 'c':
        nclients = atoi(optarg);
        assert(nclients <= MAX_CLIENTS);
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
               "   -p port\n"
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

  size_t used, total;
  double util;
  stats_get_mem(hash_table, &used, &total, &util);
  printf("memory used: %zu (%.2f%%), total: %zu, utilization: %.2f%%\n", 
      used, (double) used / total * 100.0, total, util * 100.0);
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
    cdata[i].cid = (design == 1 || design == 2) ? create_hash_table_client(hash_table) : 0;
    cdata[i].core = (design == 1 || design == 2) ? nservers + i : i;
    cdata[i].nsockets = 0;
    for (int k = 0; k < MAX_SOCKETS; k++) {
      cdata[i].sockets[k] = -1;
    }
  }

  printf("Starting Hash Server...\n"); 
  printf("design: %d, nservers: %d, nclients: %d, partition: %zu(bytes)\n", 
      design, nservers, nclients, stats_get_overhead(hash_table) / nservers);

  // start client threads
  int ret;
  for (int i = 0; i < nclients; i++) {
    pthread_t tcpth;
    ret = pthread_create(&tcpth, 0, clientgo, (void *)&cdata[i]);
    assert(ret == 0);
    pthread_detach(tcpth);
  }

  // start thread to listen on tcp port
  tcpserver(NULL);

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
  sin.sin_port = htons(port);

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

  listen(s, nclients * MAX_SOCKETS);
  sigset(SIGPIPE, SIG_IGN);

  while (1) {
    long s1;
    struct sockaddr_in sin1;
    socklen_t sinlen = sizeof(sin1);

    bzero(&sin1, sizeof(sin1));
    s1 = accept(s, (struct sockaddr *) &sin1, &sinlen);
    assert(s1 >= 0);
    setsockopt(s1, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    fcntl(s1, F_SETFL, fcntl(s1, F_GETFL, 0) | O_NONBLOCK);

    // find client with minimum number of connections    
    int cid = 0;
    for (int i = 1; i < nclients; i++) {
      if (cdata[i].nsockets < cdata[cid].nsockets) cid = i;
    }
    while (cdata[cid].nsockets >= MAX_SOCKETS) {
      _mm_pause();
    }

    // find unused socket in client's socket list
    int i = 0;
    while (1) {
      if (cdata[cid].sockets[i] == -1) {        
        cdata[cid].fin[i] = fdopen(s1, "r");
        cdata[cid].fout[i] = fdopen(s1, "w");
        __sync_fetch_and_add(&cdata[cid].nsockets, 1);
        cdata[cid].sockets[i] = s1;

        if (__sync_add_and_fetch(&active_clients, 1) == 1) {
          printf("...Running!...\n");
          start_time = now();
        }
        //printf("client connected assigned to cid: %d, socketid it: %d\n", cid, i);
        break;
      }
      i = (i + 1) & (MAX_SOCKETS - 1);
    }
  }
}

// client thread
void * clientgo(void *xarg)
{
  size_t r;
  int socketerr = 0;
  int socketretry = 0;

  struct client_data *cd = (struct client_data *)xarg;
  int cid = cd->cid;
  set_affinity(cd->core);

  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  void **valueoffset = (void **)memalign(CACHELINE, batch_size * sizeof(void *));
  char *valuebuf = (char *)memalign(CACHELINE, VALUE_BUFFER_SIZE);
  
  int *query_sid = (int *)memalign(CACHELINE, batch_size * sizeof(int));
  int *query_socket = (int *)memalign(CACHELINE, batch_size * sizeof(int));

  void **values = (void **)memalign(CACHELINE, batch_size * sizeof(void *));

  int last_socket = 0;
  int doflush[MAX_SOCKETS] = { 0 };

  int totalnqueries = 0;
  int docnt = 0;

  double last_dotime;

  while (1) {
    int nqueries = 0;
    int bufoffset = 0;
    
    int i = last_socket;
    last_dotime = now();
    while (1) {      
      if ((nqueries >= batch_size) || (bufoffset + MAX_VALUE_SIZE >= VALUE_BUFFER_SIZE)) {
        break;
      }

      if (cd->sockets[i] != -1) {
        errno = 0;
        clearerr((FILE *)cd->fin[i]);
        char *query = (char *)&queries[nqueries];
        r = fread(query, 1, sizeof(struct hash_query), (FILE *)cd->fin[i]);
        if (r > 0) {
          size_t bread = r;
          while (bread < sizeof(struct hash_query)) {
            errno = 0;
            clearerr((FILE *)cd->fin[i]);
            r = fread(&query[bread], 1, (sizeof(struct hash_query) - bread), (FILE *)cd->fin[i]);
            if (((ferror((FILE *)cd->fin[i]) != 0) && (errno != EAGAIN)) || feof((FILE *)cd->fin[i])) {
              socketerr = 1;
              socketretry = 0;
              break;
            }
            bread += r;
          }

          if (bread == sizeof(struct hash_query)) {
            socketretry = 0;
            socketerr = 0;
          }
        } else {
          socketretry = (errno == EAGAIN);
          socketerr = ((ferror((FILE *)cd->fin[i]) != 0 && !socketretry) || feof((FILE *)cd->fin[i]));
        }

        //printf("%d, err: %d, retry: %d, othererr: %d, r: %zu\n", i, socketerr, socketretry, ferror((FILE *)cd->fin[i]), r);
        if (!socketerr && !socketretry) {
          assert(queries[nqueries].optype == OPTYPE_INSERT || queries[nqueries].optype == OPTYPE_LOOKUP);
          //printf("successful read, optype: %d, nqueries: %d\n", queries[nqueries].optype, nqueries);
          last_socket = i;

          // read the query succesfully
          if (queries[nqueries].optype == OPTYPE_INSERT) {
            // need to read value too
            size_t bread = 0;
            while (bread < queries[nqueries].size) {
              errno = 0;
              clearerr((FILE *)cd->fin[i]);
              r = fread(&valuebuf[bufoffset + bread], 1, (queries[nqueries].size - bread), (FILE *)cd->fin[i]);
              if (((ferror((FILE *)cd->fin[i]) != 0) && (errno != EAGAIN)) || feof((FILE *)cd->fin[i])) {
                socketerr = 1;
                socketretry = 0;
                break;
              }
              bread += r;
            }

            if (bread == queries[nqueries].size) {
              // successfully read value
              valueoffset[nqueries] = &valuebuf[bufoffset];
              bufoffset += bread;
              nqueries++;
            }
          } else {
            query_sid[nqueries] = i;
            query_socket[nqueries] = cd->sockets[i];
            nqueries++;
          }
        }

        if (socketerr) {
          // this socket has been closed
          fclose((FILE *)cd->fin[i]);
          fclose((FILE *)cd->fout[i]);
          close(cd->sockets[i]);
          __sync_fetch_and_sub(&cd->nsockets, 1);
          cd->sockets[i] = -1;

          if (cd->nsockets == 0) {
            //debug and stats
            printf("avg nqueries: %.3f\n", (double)totalnqueries / docnt);
            docnt = 0;
            totalnqueries = 0;
          }
          if (__sync_sub_and_fetch(&active_clients, 1) == 0) {
            end_time = now();
            printf("TIME: %.3f\n", end_time - start_time);
            print_stats();
            stats_reset(hash_table);
          }
          //printf("client disconnected cid: %d, socketid it: %d\n", cid, i);
          continue;
        } 
      }

      if ((cd->sockets[i] == -1) || socketretry) {
        // socket is not ready try other ones
        i = (i + 1) & (MAX_SOCKETS - 1);
        if ((nqueries > 0) && (i == last_socket)) {
          break;
        }
      }
    }

    //printf("cid: %d, nqueries: %d\n", cid, nqueries);
    totalnqueries += nqueries;
    docnt++;
    doqueries(cid, nqueries, queries, values);  

    // handle values returned after completeing all the queries
    // this loop must go through every returned value, even if
    // fin and fout are broken, otherwise there will be memory leaks
    // for any value that is not releaesd or marked ready
    for (int k = 0; k < nqueries; k++) {
      struct hash_value *val = values[k];

      if (queries[k].optype == OPTYPE_LOOKUP) {        
        if (cd->sockets[query_sid[k]] == query_socket[k]) {
          // if the socket has not been destroyed yet write out result
          int size = (val == NULL) ? 0 : val->size;
          r = fwrite(&size, sizeof(int), 1, (FILE *)cd->fout[query_sid[k]]);

          if (size != 0) {
            r = fwrite(val->data, 1, size, (FILE *)cd->fout[query_sid[k]]);
          } 
          doflush[query_sid[k]] = 1;
        }

        if (val != NULL) localmem_release(val, 1);
      } else if (queries[k].optype == OPTYPE_INSERT) {
        assert(val != NULL);
        val->size = queries[k].size - sizeof(int);
        memcpy(val->data, valueoffset[k], val->size);
        localmem_mark_ready(val);
      } else {
        assert(0);
      }
    }

    for (int i = 0; i < MAX_SOCKETS; i++) {
      if (doflush[i] == 1) {
        fflush((FILE *)cd->fout[i]);
        doflush[i] = 0;
      }
    }
  }

  free(queries);
  free(valueoffset);
  free(valuebuf);
  free(query_sid);
  free(query_socket);
  free(values);
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
