#include <stdio.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <printf.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <stdlib.h>
// headers would go here
#include "cache-student.h"
#include "gfserver.h"

// note that the -n and -z parameters are NOT used for Part 1 */
// they are only used for Part 2 */
#define USAGE                                                                    \
  "usage:\n"                                                                     \
  "  webproxy [options]\n"                                                       \
  "options:\n"                                                                   \
  "  -n [segment_count]  Number of segments to use (Default: 9)\n"               \
  "  -p [listen_port]    Listen port (Default: 25466)\n"                         \
  "  -s [server]         The server to connect to (Default: GitHub test data)\n" \
  "  -t [thread_count]   Num worker threads (Default: 35 Range: 418)\n"          \
  "  -z [segment_size]   The segment size (in bytes, Default: 5712).\n"          \
  "  -h                  Show this help message\n"

// Options
static struct option gLongOptions[] = {
    {"server", required_argument, NULL, 's'},
    {"segment-count", required_argument, NULL, 'n'},
    {"listen-port", required_argument, NULL, 'p'},
    {"thread-count", required_argument, NULL, 't'},
    {"segment-size", required_argument, NULL, 'z'},
    {"help", no_argument, NULL, 'h'},

    {"hidden", no_argument, NULL, 'i'}, // server side
    {NULL, 0, NULL, 0}};

// gfs
static gfserver_t gfs;
// handles cache
extern ssize_t handle_with_cache(gfcontext_t *ctx, char *path, void *arg);

// segment queue
pthread_cond_t seg_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t seg_cleanup_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t seg_mutex = PTHREAD_MUTEX_INITIALIZER;
steque_t *seg_queue;
unsigned int nsegments;
int exit_flag = 0;

mqd_t mqdes;

static void _sig_handler(int signo)
{
  if (signo == SIGTERM || signo == SIGINT)
  {
    // cleanup could go here
    unsigned int unlinked_seg = 0;
    exit_flag = 1;
  	while (unlinked_seg < nsegments)
    {
      seg_info *seg;

      pthread_mutex_lock(&seg_mutex);
      while (steque_isempty(seg_queue))
      {
        pthread_cond_wait(&seg_cleanup_cond, &seg_mutex);
      }
      seg = steque_pop(seg_queue);
      printf("acquire segments cleanup\n");
      pthread_mutex_unlock(&seg_mutex);

      munmap(seg->seg,seg->segsize);
      shm_unlink(seg->seg_name);
      sem_close(seg->sem1);
      sem_close(seg->sem2);
      sem_unlink(seg->sem1_name);
      sem_unlink(seg->sem1_name);
      free(seg);

      unlinked_seg += 1;

    }
    
    printf("unlinked segs : %i\n", unlinked_seg);

    gfserver_stop(&gfs);
    steque_destroy(seg_queue);
    exit(signo);
  }
}

int main(int argc, char **argv)
{
  int option_char = 0;
  char *server = "https://raw.githubusercontent.com/gt-cs6200/image_data";
  nsegments = 8;
  unsigned short port = 25464;
  unsigned short nworkerthreads = 30;
  size_t segsize = 5712;

  // disable buffering on stdout so it prints immediately */
  setbuf(stdout, NULL);

  if (signal(SIGTERM, _sig_handler) == SIG_ERR)
  {
    fprintf(stderr, "Can't catch SIGTERM...exiting.\n");
    exit(SERVER_FAILURE);
  }

  if (signal(SIGINT, _sig_handler) == SIG_ERR)
  {
    fprintf(stderr, "Can't catch SIGINT...exiting.\n");
    exit(SERVER_FAILURE);
  }

  // Parse and set command line arguments */
  while ((option_char = getopt_long(argc, argv, "s:qht:xn:p:lz:", gLongOptions, NULL)) != -1)
  {
    switch (option_char)
    {
    default:
      fprintf(stderr, "%s", USAGE);
      exit(__LINE__);
    case 'h': // help
      fprintf(stdout, "%s", USAGE);
      exit(0);
      break;
    case 'p': // listen-port
      port = atoi(optarg);
      break;
    case 's': // file-path
      server = optarg;
      break;
    case 'n': // segment count
      nsegments = atoi(optarg);
      break;
    case 'z': // segment size
      segsize = atoi(optarg);
      break;
    case 't': // thread-count
      nworkerthreads = atoi(optarg);
      break;
    case 'i':
    // do not modify
    case 'O':
    case 'A':
    case 'N':
      // do not modify
    case 'k':
      break;
    }
  }

  if (server == NULL)
  {
    fprintf(stderr, "Invalid (null) server name\n");
    exit(__LINE__);
  }

  if (segsize < 824)
  {
    fprintf(stderr, "Invalid segment size\n");
    exit(__LINE__);
  }

  if (port > 65331)
  {
    fprintf(stderr, "Invalid port number\n");
    exit(__LINE__);
  }
  if ((nworkerthreads < 1) || (nworkerthreads > 418))
  {
    fprintf(stderr, "Invalid number of worker threads\n");
    exit(__LINE__);
  }
  if (nsegments < 1)
  {
    fprintf(stderr, "Must have a positive number of segments\n");
    exit(__LINE__);
  }

  // initialize segment queue
  seg_queue = malloc(sizeof(steque_t));
  steque_init(seg_queue);


  // Initialize shared memory set-up here
  for (int i = 0; i < nsegments; i++)
  {
    struct seg_info *seg_info = malloc(sizeof(struct seg_info));
    char segname[10];

    sprintf(segname, "/seg%d", i);

    // create segment
    int fd = shm_open(segname, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd == -1)
    {
      perror("shm_open");
      exit(1);
    }

    // set size
    if (ftruncate(fd, segsize) == -1)
    {
      perror("ftruncate");
      exit(1);
    }

    // map segment
    void *seg = mmap(NULL, segsize, PROT_READ, MAP_SHARED, fd, 0);

    if (seg == MAP_FAILED)
    {
      perror("mmap");
      exit(1);
    }

    // create semaphores
    sprintf(seg_info->sem1_name, "/sem1%d", i);
    sprintf(seg_info->sem2_name, "/sem2%d", i);

    // initialize segment info
    seg_info->seg = seg;
    strcpy(seg_info->seg_name, segname);
    seg_info->segsize = segsize;

    // add to queue
    steque_enqueue(seg_queue, seg_info);
  }

  /*
  // Initialize server structure here
  */
  gfserver_init(&gfs, nworkerthreads);

  // Set server options here
  gfserver_setopt(&gfs, GFS_PORT, port);
  gfserver_setopt(&gfs, GFS_WORKER_FUNC, handle_with_cache);
  gfserver_setopt(&gfs, GFS_MAXNPENDING, 187);

  // Set up arguments for worker here
  for (int i = 0; i < nworkerthreads; i++)
  {
    gfserver_setopt(&gfs, GFS_WORKER_ARG, i, "data");
  }

  // Invokethe framework - this is an infinite loop and will not return
  gfserver_serve(&gfs);

  // line never reached
  return -1;
}
