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
#include <time.h>
#include <unistd.h>

#include "smphashtable.h"
#include "util.h"

#define MAX_SOCKETS       128             // per client thread
#define MAX_VALUE_SIZE    (1024 * 1024)
#define VALUE_BUFFER_SIZE (2 * MAX_VALUE_SIZE)

int design          = 2;
int nservers        = 1;
int nclients        = 1;
int batch_size      = 1000;
int nelems          = 100000;
size_t size         = 6400000;
int port            = 2117;
int verbose         = 1;

struct hash_value {
  uint32_t size;
  char data[0];
};

struct client_data {
  int cid;
  int core;
  volatile int nsockets;
  volatile int sockets[MAX_SOCKETS];
  volatile FILE *fin[MAX_SOCKETS];
  int writeiov_cnt[MAX_SOCKETS];
  struct iovec *writeiov[MAX_SOCKETS]; // TODO: dynamically allocate this dude
};

struct client_data cdata[MAX_CLIENTS];

// Hash Table Data Structure
struct hash_table *hash_table;

// Constant value to return when no value is found in key/value store
const uint32_t NULL_VALUE = 0;

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
  hash_table = create_hash_table(size, nservers);

  if (design == 1 || design == 2) {
    start_hash_table_servers(hash_table, 0);
  }

  // Create all clients 
  for (int i = 0; i < nclients; i++) {
    cdata[i].cid = (design == 1 || design == 2) ? create_hash_table_client(hash_table) : 0;
    cdata[i].core = (design == 1 || design == 2) ? nservers + i : i;
    cdata[i].nsockets = 0;
    for (int k = 0; k < MAX_SOCKETS; k++) {
      cdata[i].sockets[k] = -1;
    }

    for (int k = 0; k < MAX_SOCKETS; k++) {
      cdata[i].writeiov[k] = (struct iovec *)memalign(CACHELINE, batch_size * sizeof(struct iovec));
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

  for (int i = 0; i < nclients; i++) {
    for (int k = 0; k < MAX_SOCKETS; k++) {
      free(cdata[i].writeiov[k]);
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
    } else if (verbose > 0) {
      printf("ERROR: client %d failed to open socket %d\n", cid, s1);
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
      if (cd->fin[i] == NULL) return 0;

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
    if (stream_error(f)) return 2; 
    bread += r;
    // TODO: some sort of timeout
  }

  assert(bread == size);
  return 0;
}

int write_iovs(int sock, struct iovec *iov, int iovcnt) 
{
  int ret = 0;
  ssize_t r;
  int ciov = 0;
  void *ciov_base = iov[0].iov_base;

  while (ciov < iovcnt) {
    errno = 0;
    r = writev(sock, &iov[ciov], iovcnt - ciov);
    if (errno != 0 && errno != EAGAIN) {
      ret = 2;
      break;
    }

    // progress ciov
    while (r > 0) {
      if (r >= iov[ciov].iov_len) {
        r -= iov[ciov].iov_len;
        ciov++;
        if (ciov == iovcnt) break;

        if (ciov_base != &NULL_VALUE) value_release(ciov_base);
        ciov_base = iov[ciov].iov_base;
      } else {
        iov[ciov].iov_len -= r;
        iov[ciov].iov_base += r;
        r = 0;
      }
    }
  }

  if (ciov_base != &NULL_VALUE) value_release(ciov_base);
  for (int i = ciov + 1; i < iovcnt; i++) {
    if (iov[i].iov_base != &NULL_VALUE) value_release(iov[i].iov_base);
  }

  return ret;
}

/**
 * write_object blocking write full object to stream. no partial writing.
 * return values:
 * 0 - success
 * 2 - stream error
 */
int write_object(char *buf, size_t size, int sock)
{
  ssize_t r;
  size_t bwritten = 0;

  while (bwritten < size) {
    errno = 0;
    r = write(sock, &buf[bwritten], size - bwritten);    
    if (errno != 0 && errno != EAGAIN) return 2;
    else if (r > 0) bwritten += r;
  }

  assert(bwritten == size);
  return 0;
}

// client thread
void * clientgo(void *xarg)
{
  int r = 0;
  int last_socket = 0;

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
  long totalnqueries = 0;
  long totalclock = 0;
  long docnt = 0;

  // initialize writeiov counts
  for (int k = 0; k < MAX_SOCKETS; k++) {
    cd->writeiov_cnt[k] = 0;
  }

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
                // increase size of each query by sizeof(uint32_t) since we will store
                // size of value + actual value in hash table
                queries[nqueries].size += sizeof(uint32_t);
                nqueries++;
              }
              break;
            default:
              if (verbose > 0) {
                printf("ERROR: client %d received invalid query from socket: %d\n", cid, cd->sockets[i]);
              }
              r = 2;
              break;
          }
        }

        if (r == 2) {
          close_socket(cd, i);
          int acs = __sync_sub_and_fetch(&active_clients, 1);

          // debug and stats
          if ((verbose > 1) && (cd->nsockets == 0)) {
            printf("nqueries: %ld, totalclock: %ld, docnt: %ld, avg nqueries: %.3f, avg clock: %.3f\n", 
                totalnqueries, totalclock, docnt, (double)totalnqueries / docnt, (double)totalclock / totalnqueries);
            docnt = 0;
            totalnqueries = 0;
            totalclock = 0;
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

    if (verbose > 1) {
      // debug and stats
      totalnqueries += nqueries;
      docnt++;

      // perform all queries
      struct timespec tmpst, tmpet;
      clock_gettime(CLOCK_MONOTONIC, /*CLOCK_THREAD_CPUTIME_ID,*/ &tmpst);
      doqueries(cid, nqueries, queries, values);  
      clock_gettime(CLOCK_MONOTONIC, /*CLOCK_THREAD_CPUTIME_ID,*/ &tmpet);
      totalclock += (tmpet.tv_sec - tmpst.tv_sec) * 1000000000 + (tmpet.tv_nsec - tmpst.tv_nsec);
    } else {
      doqueries(cid, nqueries, queries, values);  
    }

    // handle values returned after completeing all the queries
    // this loop must go through every returned value, even if
    // sockets are broken, otherwise there will be memory leaks
    // for any value that is not released or marked ready
    for (int k = 0; k < nqueries; k++) {
      struct hash_value *val = values[k];

      switch (queries[k].optype) {
        case OPTYPE_LOOKUP: 
          {
          int sid = query_sid[k];
          if (cd->sockets[sid] == query_socket[k]) {
            // if the socket has not been destroyed yet write out result
            struct iovec *iov = &cd->writeiov[sid][cd->writeiov_cnt[sid]];
            if (val == NULL) {
              iov->iov_base = (char *)&NULL_VALUE;
              iov->iov_len = sizeof(uint32_t);
            } else {
              iov->iov_base = val;
              iov->iov_len = sizeof(uint32_t) + val->size;
            }
            cd->writeiov_cnt[sid]++;
          }
          }
          break;
        case OPTYPE_INSERT:
          if (val != NULL) {
            val->size = queries[k].size - sizeof(uint32_t);
            memcpy(val->data, valueoffset[k], val->size);
            value_mark_ready(val);
          } else {
            assert(0);
          }
          break;
        default:
          assert(0);
          break;
      }
    }
    
    // Write all prepared iovs to appropriate sockets
    for (int k = 0; k < MAX_SOCKETS; k++) {
      if (cd->writeiov_cnt[k] > 0) {
        r = write_iovs(cd->sockets[k], cd->writeiov[k], cd->writeiov_cnt[k]);
        cd->writeiov_cnt[k] = 0;

        if (verbose > 0 && r == 2) {
          printf("ERROR: client %d failed to write to socket: %d\n", cid, cd->sockets[k]);
        }
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
  switch (design) {
    case 1:
      for (int k = 0; k < nqueries; k++) {
        if (queries[k].optype == OPTYPE_LOOKUP) {
          values[k] = smp_hash_lookup(hash_table, cid, queries[k].key);
        } else {
          values[k] = smp_hash_insert(hash_table, cid, queries[k].key, queries[k].size);
        }
      }
      break;
    case 2:
      smp_hash_doall(hash_table, cid, nqueries, queries, values);
      break;
    case 3:
      for (int k = 0; k < nqueries; k++) {
        if (queries[k].optype == OPTYPE_LOOKUP) {
          values[k] = locking_hash_lookup(hash_table, queries[k].key);
        } else {
          values[k] = locking_hash_insert(hash_table, queries[k].key, queries[k].size);
        }
      }
      break;
    default:
      assert(0);
  }
}
