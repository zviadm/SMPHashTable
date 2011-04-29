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

#define MAX_SOCKETS       128             // per client thread
#define MAX_VALUE_SIZE    (1024 * 1024)
#define VALUE_BUFFER_SIZE (8 * MAX_VALUE_SIZE)

int design          = 2;
int nservers        = 1;
int nclients        = 1;
int batch_size      = 1000;
int nelems          = 100000;
size_t size         = 6400000;
struct hash_table *hash_table;
int port            = 2117;
int verbose         = 1;

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

// variables used for stats and debugging
volatile int active_clients = 0;
volatile double start_time, end_time;

// Forward Declarations
void run_server();
void * tcpserver(void *xarg);
void * clientgo(void *xarg);
int open_socket(struct client_data *cd, int sock);
void close_socket(struct client_data *cd, int sid);
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
    int s1;
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
      // TODO: some sort of timeout 
    }

    if (open_socket(&cdata[cid], s1)) {
      int acs = __sync_add_and_fetch(&active_clients, 1);
      if ((verbose > 0) && (acs == 1)) {
        printf("...Running!...\n");
        start_time = now();
      }
    } else {
      printf("failed to open socket %d %d\n", cid, s1);
    }
  }
}

int open_socket(struct client_data *cd, int sock)
{
  // find unused socket in client's socket list
  int i = 0;
  while (1) {
    if (cd->sockets[i] == -1) {        
      cd->fin[i] = fdopen(sock, "r");
      cd->fout[i] = fdopen(sock, "w");
      if (cd->fin[i] == NULL || cd->fout[i] == NULL) return 0;

      __sync_fetch_and_add(&cd->nsockets, 1);
      cd->sockets[i] = sock;
      return 1; 
    }
    i = (i + 1) & (MAX_SOCKETS - 1);
  }
}

void close_socket(struct client_data *cd, int sid)
{
  fclose((FILE *)cd->fin[sid]);
  fclose((FILE *)cd->fout[sid]);
  close(cd->sockets[sid]);
  __sync_fetch_and_sub(&cd->nsockets, 1); // also serves as memory barrier
  cd->sockets[sid] = -1;
}

/**
 * stream_error: return true if stream is finished or failed
 */
int stream_error(FILE *f) {
  return ((ferror(f) != 0 && errno != EAGAIN) || feof(f));
}

/**
 * read_object read full object from stream. no partial reading.
 * return values:
 * 0 - success
 * 1 - retry later
 * 2 - stream error
 */
int read_object(char *buf, size_t size, FILE *f)
{
  size_t r;

  errno = 0;
  clearerr(f);
  r = fread(buf, 1, size, f);
  if (r == 0) {
    // if socket has failed without EAGAIN or eof was asserted than socket must be closed
    // otherwise it can be still be retried later on
    return stream_error(f) ? 2 : 1;
  } 

  size_t bread = r;
  while (bread < size) {
    errno = 0;
    clearerr(f);
    r = fread(&buf[bread], 1, size - bread, f);
    if (stream_error(f)) { return 2; }
    bread += r;
    // TODO: some sort of timeout
  }

  assert(bread == size);
  return 0;
}

// client thread
void * clientgo(void *xarg)
{
  int r = 0;
  int last_socket = 0;
  int doflush[MAX_SOCKETS] = { 0 };

  struct client_data *cd = (struct client_data *)xarg;
  int cid = cd->cid;
  set_affinity(cd->core);

  // Buffer for queries and values read from sockets
  struct hash_query *queries = (struct hash_query *)memalign(CACHELINE, batch_size * sizeof(struct hash_query));
  void **valueoffset = (void **)memalign(CACHELINE, batch_size * sizeof(void *));
  char *valuebuf = (char *)memalign(CACHELINE, VALUE_BUFFER_SIZE);
  
  // For queries read we keep information which socket to send data back to
  int *query_sid = (int *)memalign(CACHELINE, batch_size * sizeof(int));
  int *query_socket = (int *)memalign(CACHELINE, batch_size * sizeof(int));

  // Array of value pointers returned after completing queries
  void **values = (void **)memalign(CACHELINE, batch_size * sizeof(void *));

  // for stats/debugging
  int totalnqueries = 0;
  int docnt = 0;

  while (1) {
    int nqueries = 0;
    int bufoffset = 0;
    
    int i = last_socket;
    while (1) {      
      if ((nqueries >= batch_size) || (bufoffset + MAX_VALUE_SIZE >= VALUE_BUFFER_SIZE)) {
        break;
      }

      if (cd->sockets[i] != -1) {
        r = read_object((char *)&queries[nqueries], sizeof(struct hash_query), (FILE *)cd->fin[i]);

        if (r == 0) {
          last_socket = i;

          switch (queries[nqueries].optype) {
            case OPTYPE_LOOKUP:
              query_sid[nqueries] = i;
              query_socket[nqueries] = cd->sockets[i];
              nqueries++;
              break;
            case OPTYPE_INSERT:
              do {
                r = read_object(&valuebuf[bufoffset], queries[nqueries].size, (FILE *)cd->fin[i]);
                // TODO: some sort of timeout
              } while (r == 1);

              if (r == 0) {
                valueoffset[nqueries] = &valuebuf[bufoffset];
                bufoffset += queries[nqueries].size;
                // increase size of each query by sizeof(int) since we will store
                // size of value + actual value in hash table
                queries[nqueries].size += sizeof(int);
                nqueries++;
              }
              break;
            default:
              assert(0);
              break;
          }
        }

        if (r == 2) {
          close_socket(cd, i);
          int acs = __sync_sub_and_fetch(&active_clients, 1);

          // debug and stats
          if ((verbose > 0) && (cd->nsockets == 0)) {
            printf("avg nqueries: %.3f\n", (double)totalnqueries / docnt);
            docnt = 0;
            totalnqueries = 0;
          }

          if ((verbose > 0) && (acs == 0)) {
            end_time = now();
            printf("TIME: %.3f\n", end_time - start_time);
            print_stats();
            stats_reset(hash_table);
          }
        }
      }

      if ((cd->sockets[i] == -1) || (r == 1)) {
        // socket is not ready try other ones
        i = (i + 1) & (MAX_SOCKETS - 1);
        if ((nqueries > 0) && (i == last_socket)) {
          break;
        }
      }
    }

    // debug and stats
    totalnqueries += nqueries;
    docnt++;

    // perform all queries
    doqueries(cid, nqueries, queries, values);  

    // handle values returned after completeing all the queries
    // this loop must go through every returned value, even if
    // fin and fout are broken, otherwise there will be memory leaks
    // for any value that is not releaesd or marked ready
    for (int k = 0; k < nqueries; k++) {
      struct hash_value *val = values[k];

      switch (queries[k].optype) {
        case OPTYPE_LOOKUP:
          if (cd->sockets[query_sid[k]] == query_socket[k]) {
            // if the socket has not been destroyed yet write out result
            uint32_t size = (val == NULL) ? 0 : val->size;
            r = fwrite(&size, sizeof(uint32_t), 1, (FILE *)cd->fout[query_sid[k]]);

            if (size != 0) {
              r = fwrite(val->data, 1, size, (FILE *)cd->fout[query_sid[k]]);
            } 
            doflush[query_sid[k]] = 1;
          }
          if (val != NULL) localmem_release(val, 1);
          break;
        case OPTYPE_INSERT:
          assert(val != NULL);
          val->size = queries[k].size - sizeof(int);
          memcpy(val->data, valueoffset[k], val->size);
          localmem_mark_ready(val);
          break;
        default:
          assert(0);
          break;
      }
    }

    // Flush all sockets where values have been returned
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
