// Specifications for the Client
// =============================
// 1. Client initiates the connection.
// 2. Sends a request to get info about server's top "N" cpu consuming
//    processes.
//    This means that if clients asks info about 2 top processes, then
//    server should check 2 processes which are consuming most CPU space.
// 3. Receive the file containing process info, and store it somewhere on
//    the disk.
// 4. Amongst these processes, find the top most consuming process, and
//    return its info to the server.
// 5. Close the connection.
#include <arpa/inet.h>
#include <assert.h>
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

#define SERV_PORT 8787
#define LOG(X) printf("(server): %s\n", strerror(X))

// Represent any process to be later put into the info file.
struct proc_info_t {
  pid_t pid;              /* PID of the process */
  unsigned long pr_utime; /* Process's User time */
  unsigned long pr_stime; /* Process's System/CPU time */
  char pr_name[32];       /* Process name */
};

// Convert an integer to an array of characters but in reverse order. 
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

  if (snprintf(ptr, len + 1, "%d,%s,%lu,%lu\n", arr->pid, arr->pr_name,
               arr->pr_utime, arr->pr_stime) > len + 1) {
    fprintf(stderr, "(client): snprintf returned truncated result.\n");
    return NULL;
  }

  return ptr;
}

// Finds top CPU consuming process.
// @returns: pointer to the structure, NULL otherwise
static struct proc_info_t *find_top_process(struct proc_info_t *top_proc) {
  int slash_proc_fd, stat_fd;
  DIR *dirp;
  struct dirent *nextdir;

  if ((slash_proc_fd = open("/proc/", O_RDONLY | O_DIRECTORY)) == -1) {
    fprintf(stderr, "(client): unable to acquire file descriptor for /proc/\n");
    return NULL;
  }

  if (!(dirp = fdopendir(slash_proc_fd))) {
    fprintf(stderr, "(client): unable to open /proc/\n");
    return NULL;
  }

  unsigned long tot_min = 0; /* total time (user + system) */
  char stat_path[256] = {0}; /* for /[pid]/stat */
  char *curr_stat_info = (char *)malloc(sizeof(char) * 1024);

  struct proc_info_t *curr_proc =
      (struct proc_info_t *)malloc(sizeof(struct proc_info_t));
  // Zero all heap allocations
  memset(curr_proc, 0, sizeof(struct proc_info_t));

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
      if (curr_tot > tot_min) {
        top_proc->pid = curr_proc->pid;
        top_proc->pr_utime = curr_proc->pr_utime;
        top_proc->pr_stime = curr_proc->pr_stime;
        strcpy(top_proc->pr_name, curr_proc->pr_name);
      }

      // Close current /proc/[pid]/stat file
      close(stat_fd);
    }
  }

  return top_proc;
}

// Retrieves the current top CPU consuming process.
// @returns: pointer to proc_info_t, NULL otherwise
char *get_top_proc_info(char *flatten) {
  struct proc_info_t *top_proc =
      (struct proc_info_t *)malloc(sizeof(struct proc_info_t));

  if ((top_proc = find_top_process(top_proc)) == NULL) {
    fprintf(stderr, "(client): unable to find top CPU consuming process\n");
    return NULL;
  }

  if ((flatten = struct2str(top_proc, 1)) == NULL) {
    fprintf(stderr,
            "(client): unable to flatten structure to pointer to characters\n");
    return NULL;
  }

  return flatten;
}

// Send top CPU consuming process's information back to the server.
// @returns: 0 upon success, -1 otherwise
int send_info(int socket_fd) {
  char *top_proc = (char *)malloc(sizeof(char) * 64);

  if ((top_proc = get_top_proc_info(top_proc)) == NULL) {
    fprintf(stderr, "(client): unable to retrieve top CPU consuming process\n");
    return -1;
  }

  // send top process infomation to server
  if (send(socket_fd, top_proc, strlen(top_proc), MSG_DONTWAIT) == -1) {
    fprintf(stderr, "(client): unable to send data over to server\n");
    return -1;
  }

  return 0;
}

// Makes a request to the server.
// @returns: 0 upon success, -1 otherwise
int make_req(int socket_fd) {
  int buf[1];

  fprintf(stdout, "Please enter a number for request: ");
  if (fscanf(stdin, "%d", buf) < 0) {
    fprintf(stderr, "(client): input error, provide a number\n");
    return -1;
  }

  if (buf[0] < 1) {
    fprintf(stderr, "(client): request for less than 1 process is not allowed\n");
    return -1;
  }

  if (write(socket_fd, buf, sizeof(int)) == -1) {
    fprintf(stderr, "(client): unable to send request\n");
    return -1;
  }

  int bytes = 0;
  char data[1024] = {0};
  if ((bytes = recv(socket_fd, data, 64, MSG_WAITFORONE)) == -1) {
    fprintf(stderr, "(client): unable to receive data from server\n");
  }
  data[bytes] = '\0';
  fprintf(stdout, "from server: %s\n", data);
  
  FILE *fptr;
  char file[64] = "data/client/";
  char pid[16] = {0};
  itoa(getpid(), pid);
  strcat(file, pid);

  if ((fptr = fopen(file, "w")) == NULL) {
    fprintf(stderr, "(server): unable to open file\n");
    return -1;
  }

  if (fwrite(data, sizeof(char), strlen(data), fptr) < 1) {
    fprintf(stderr, "(server): unable to write to file\n");
    return -1;
  }

  return 0;
}

// Initializes the client socket.
// @returns: 0 on success, errno on failure.
int init_client() {
  int socket_fd;
  struct sockaddr_in cli_addr;
  socklen_t socket_len = sizeof(cli_addr);

  if ((socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
    fprintf(stderr, "(client): unable to initiate the socket\n");
    return errno;
  }

  bzero(&cli_addr, socket_len);

  // socket parameters setup
  cli_addr.sin_family = AF_INET;
  cli_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  cli_addr.sin_port = htons(SERV_PORT);
  /* cli_addr.sin_addr.s_addr = inet_addr("192.168.1.105"); */
  /* cli_addr.sin_port = htons(SERV_PORT); */

  if (connect(socket_fd, (struct sockaddr *)&cli_addr, socket_len)) {
    fprintf(stderr, "(client): unable to connect to socket\n");
    LOG(errno); // connection refused
    return errno;
  }

  if (make_req(socket_fd)) {
    fprintf(stderr, "(client): unable to make a request\n");
    return -1;
  }

  // Sends own top process's info
  if (send_info(socket_fd)) {
    fprintf(stderr, "(client): unable to send back information to server\n");
    return -1;
  }

  if (close(socket_fd) == -1) {
    fprintf(stderr, "(client): unable to close connection\n");
    return -1;
  }

  fprintf(stdout, "(client): closing the connection...\n");

  return 0;
}

int main(int argc, char **argv) {
  int errno, ret;

  /* if (argc != 2) { */
  /*   fprintf(stderr, "(client): require IP address to connect\n"); */
  /*   exit(1); */
  /* } */

  if ((ret = init_client()) < 0) {
    fprintf(stderr, "(client): Error %d, unable to initialize the client\n",
            ret);
    exit(1);
  }

  return 0;
}
