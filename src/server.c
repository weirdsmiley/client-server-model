// Specifications for the Server
// =============================
// 1. It sets up its socket to listen for client connections.
// 2. Should handle multiple connections
//    a. Spawn thread for each connection
// 3. Upon receiving a req of top N CPU consuming processes, write process
// information, like PID, current CPU utilization, process name, to a file.
// 4. Send the process information file to client.
//
// In order to handle multiple requests, spawn thread upon each req from
// client.
//
// Block diagram for the connection:
//
//    +------------+                        +------------+
//    |  File      |                        |  File      |
//    +------------+                        +------------+
//         ▲                                      ▲
//         │ Write N top                          │ Find top-most
//         │ proc info                            │ proc info
//    +---------------+ req N top processes +----------------+
//    |               |◀————————————————————|                |
//    |               |                     |                |
//    |    SERVER     |    send File        |     CLIENT     |
//    |               |————————————————————▶|                |
//    |               |                     |                |
//    |               |◀————————————————————|                |
//    +---------------+ send top-most proc  +----------------+
//
// For advanced techniques,
// 1. Non-blocking sockets
//    a. poll syscall
//    b. select syscall
// 2. epoll syscall
#include <arpa/inet.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h> /* Only for AF_NET or INET domain */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE 1024
#define PENDING 20
#define SERV_PORT 8787
#define LOG(X) printf("(server): %s\n", strerror(X))

// Represent any process to be later put into the info file.
struct proc_info_t {
  pid_t pid;              /* PID of the process */
  unsigned long pr_utime; /* Process's User time */
  unsigned long pr_stime; /* Process's System/CPU time */
  char pr_name[32];       /* Process name */
};

void dump(struct proc_info_t *arr, int n) {
  struct proc_info_t *iter = arr;

  for (int i = 0; i < n; ++i) {
    printf("pid: %d | %s | utime: %lu | stime: %lu\n", arr->pid, arr->pr_name,
           arr->pr_utime, arr->pr_stime);
    ++iter;
  }
}

// Convert an integer to an array of characters but in reverse order. 
// Its aim is to provide unique s[] (as tid is unique) hence we don't 
// need the reversal of characters.
// @returns: void
void itoa(unsigned long n, char s[]) {
  int i = 0;

  do {
    s[i++] = n % 10 + '0';
  } while ((n /= 10) > 0);
  s[i] = '\0';
}

// Flatten all the given structs into a char*, according to the offset n.
// @returns: pointer to the flattened array, NULL on failure
char *struct2str(struct proc_info_t *arr, int n) {
  /* get lenght of string required to hold struct values */
  size_t len = 0;
  len = snprintf(NULL, len, "%d,%s,%lu,%lu", arr->pid, arr->pr_name,
                 arr->pr_utime, arr->pr_stime);

  /* allocate/validate string to hold all values (+1 to null-terminate) */
  char *ptr = calloc(1, sizeof *ptr * len + 1);
  if (!ptr) {
    fprintf(stderr, "(server): virtual memory allocation failed.\n");
    return NULL;
  }

  /* write/validate struct values to apstr */
  if (snprintf(ptr, len + 1, "%d,%s,%lu,%lu", arr->pid, arr->pr_name,
               arr->pr_utime, arr->pr_stime) > len + 1) {
    fprintf(stderr, "(server): snprintf returned truncated result.\n");
    return NULL;
  }

  return ptr;
}

// Finds 'n' top CPU consuming processes and writes their information
// to a file. 'buf' contains one element 'n' as the request for the
// current thread.
// @returns: 0 upon success, -1 on failure.
static int find_top_processes(int conn_fd, int buf[]) {

  if (buf[0] < 1) {
    fprintf(stdout, "(server): Request for 0 top processes is denied\n");
    return -1;
  }

  int slash_proc_fd, stat_fd;
  FILE *fptr;
  DIR *dirp;
  struct dirent *nextdir;

  char file[64] = "data/server/";
  char tid[16] = {0};
  itoa(pthread_self(), tid);
  strcat(file, tid);

  // fptr contains the file handle where computed information will be
  // dumped to send.
  if ((fptr = fopen(file, "w")) == NULL) {
    fprintf(stderr, "(server): unable to open file\n");
    return -1;
  }

  if ((slash_proc_fd = open("/proc/", O_RDONLY | O_DIRECTORY)) == -1) {
    fprintf(stderr, "(server): unable to acquire file descriptor for /proc/\n");
    return -1;
  }

  if (!(dirp = fdopendir(slash_proc_fd))) {
    fprintf(stderr, "(server): unable to open /proc/\n");
    return -1;
  }

  unsigned long tot_min = 0; /* total time (user + system) */
  char stat_path[256] = {0}; /* for /[pid]/stat */
  char *curr_stat_info = (char *)malloc(sizeof(char) * 1024);

  struct proc_info_t *curr_proc =
      (struct proc_info_t *)malloc(sizeof(struct proc_info_t));

  struct proc_info_t *top_procs =
      (struct proc_info_t *)malloc(buf[0] * sizeof(struct proc_info_t));

  // Zero all heap allocations
  memset(curr_proc, 0, sizeof(struct proc_info_t));
  memset(top_procs, 0, buf[0] * sizeof(struct proc_info_t));

  while ((nextdir = readdir(dirp))) {
    if (!strcmp(nextdir->d_name, ".") || !strcmp(nextdir->d_name, "..")) {
      continue;
    } else if (nextdir->d_name[0] >= '0' && nextdir->d_name[0] <= '9') {
      strcpy(stat_path, nextdir->d_name);
      strcat(stat_path, "/stat");

      if ((stat_fd = openat(slash_proc_fd, stat_path, O_RDONLY)) == -1) {
        // Some processes may just have initialized and/or they may not
        // have a /proc/pid/stat file. In this case, we will simply
        // ignore it.
        continue;
      }

      // Read the entire contents of stat_fd to curr_stat_info
      char *iter = curr_stat_info;
      while (read(stat_fd, iter, sizeof(char)) == 1) {
        ++iter;
      }
      *iter = '\0';

      sscanf(curr_stat_info,
             "%d %[^)] %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu",
             &curr_proc->pid, curr_proc->pr_name, &curr_proc->pr_utime,
             &curr_proc->pr_stime);

      unsigned long curr_tot = curr_proc->pr_utime + curr_proc->pr_stime;
      if (curr_tot < tot_min) {
        continue;
      }

      int iter_list = 0;
      // Iterate over the entire list.
      // FIXME: wrong logicals fills up one top most proc
      while (iter_list < buf[0]) {
        if (curr_tot > (top_procs + iter_list)->pr_utime +
                           (top_procs + iter_list)->pr_stime) {

          (top_procs + iter_list)->pid = curr_proc->pid;
          (top_procs + iter_list)->pr_utime = curr_proc->pr_utime;
          (top_procs + iter_list)->pr_stime = curr_proc->pr_stime;
          strcpy((top_procs + iter_list)->pr_name, curr_proc->pr_name);

          // Find the new min
          int iter_min = 0;
          while (iter_min < buf[0]) {
            curr_tot = (top_procs + iter_min)->pr_utime +
                       (top_procs + iter_min)->pr_stime;
            tot_min = curr_tot < tot_min ? curr_tot : tot_min;
            ++iter_min;
          }
        }
        ++iter_list;
      }
      close(stat_fd);
    }
  }

  char *data = struct2str(top_procs, buf[0]);

  // write to fptr
  if (fwrite(data, sizeof(char), strlen(data), fptr) < 1) {
    fprintf(stderr, "(server): unable to write to file\n");
    return -1;
  }

  if (send(conn_fd, data, strlen(data), MSG_DONTWAIT) == -1) {
    fprintf(stderr, "(server): unable to send file over to client");
    return -1;
  }

  // FIXME: Something is wrong in these free() calls!
  /* free(curr_stat_info); */
  /* free(curr_proc); */
  /* free(top_procs); */
  fclose(fptr);
  free(nextdir);
  closedir(dirp);
  close(slash_proc_fd);

  return 0;
}

// Processes the current client's request. This is happening in a newly
// spawned thread.
// @returns: 0 on success, -1 on failure, errno is not set
static int process_req(int conn_fd) {
  // Read N from conn_fd
  int buf[1];

  if (read(conn_fd, buf, sizeof(int)) <= 0) {
    fprintf(stderr, "(server): error while reading from socket\n");
    return -1;
  }

  if (find_top_processes(conn_fd, buf)) {
    fprintf(stderr, "(server): unable to get CPU consuming processes\n");
    return -1;
  }

  char cli_top_proc[64];
  // Receive the client's top CPU consuming process
  int iter = 0;
  while ((iter < 64) && read(conn_fd, &cli_top_proc[iter], sizeof(char)) == 1) {
    ++iter;
  }
  cli_top_proc[iter] = '\0';

  fprintf(stdout, "(server): client responded: %s\n", cli_top_proc);

  return 0;
}

// A wrapper around the request processing routine to handle threads and
// errors.
// @returns: NULL
static void *process_req_wrap(void *args) {
  int conn_fd = *((int *)args);
  free(args);

  if (pthread_detach(pthread_self())) {
    fprintf(stderr,
            "(server): unable to detach the thread handling client request\n");
    return NULL;
  }

  if (process_req(conn_fd)) {
    fprintf(stderr, "(server): unable to process client's request\n");
    return NULL;
  }

  close(conn_fd);

  return NULL;
}

// Initializes the server and sets it up for listening.
// @returns: 0 on success, -1 on failure
int init_server() {
  int socket_fd, *conn_fd;
  struct sockaddr_in srv_addr;

  socklen_t socket_len = sizeof(srv_addr);

  // TODO: handling ERRORS (see respective manpages)
  if ((socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
    fprintf(stderr, "(server): unable to initiate the socket\n");
    return errno;
  }

  bzero(&srv_addr, socket_len);

  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  srv_addr.sin_port = htons(SERV_PORT);
  /* srv_addr.sin_addr.s_addr = inet_addr("192.168.1.105"); */
  /* srv_addr.sin_port = htons(SERV_PORT); */

  if (bind(socket_fd, (struct sockaddr *)&srv_addr, socket_len)) {
    fprintf(stderr, "(server): unable to bind socket\n");
    return errno;
  }

  if (listen(socket_fd, PENDING)) {
    fprintf(stderr, "(server): unable to listen to port\n");
    return errno;
  }
  fprintf(stdout, "(server): listening for connections...\n");

  do {
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);

    // Heap allocation is necessary to prevent sync issues when both
    // threads try to access same file descriptor.
    conn_fd = (int *)malloc(sizeof(int));

    if ((*conn_fd = accept(socket_fd, (struct sockaddr *)&cli_addr, &cli_len)) <
        0) {
      fprintf(stderr, "(server): unable to accept client connection\n");
      return errno;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, &process_req_wrap, conn_fd)) {
      fprintf(stderr, "(server): unable to spawn a new thread\n");
    }

  } while (1);

  return 0;
}

int main(int argc, char **argv) {
  int errno, ret;

  if ((ret = init_server()) < 0) {
    fprintf(stderr, "(server): Error: %d, unable to initialize the server\n",
            ret);
    exit(1);
  }

  return 0;
}
