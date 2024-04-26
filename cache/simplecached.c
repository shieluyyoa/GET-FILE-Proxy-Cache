#include <stdio.h>
#include <unistd.h>
#include <printf.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <mqueue.h>


#include "cache-student.h"
#include "shm_channel.h"
#include "simplecache.h"
#include "gfserver.h"

// CACHE_FAILURE
#if !defined(CACHE_FAILURE)
#define CACHE_FAILURE (-1)
#endif

#define MAX_CACHE_REQUEST_LEN 6200
#define MAX_SIMPLE_CACHE_QUEUE_SIZE 826

unsigned long int cache_delay;

pthread_cond_t cache_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

steque_t *cache_queue;
mqd_t mqdes;
struct timespec timeout = {10, 0};
int exit_flag = 0;

static void *process_cache_request(void *arg)
{
	while (1)
	{	
		ssize_t bytes_sent; 
		size_t file_len;
		struct stat st;
		// printf("thread id : %lu\n", pthread_self());
		pthread_mutex_lock(&cache_mutex);

		while (steque_isempty(cache_queue))
		{
			if (exit_flag)
			{
				pthread_mutex_unlock(&cache_mutex);
				return NULL;
			}
			pthread_cond_wait(&cache_cond, &cache_mutex);
		}

		request_info *req_info = steque_pop(cache_queue);

		pthread_mutex_unlock(&cache_mutex);

		size_t segsize = req_info->segsize;
		
		// acccess segment
		int seg_fd = shm_open(req_info->seg_name, O_RDWR, 0666);
		if (seg_fd == -1)
		{
			perror("shm_open");
			continue;
		}

		// map segment
		void* file_buffer = mmap(NULL, segsize, PROT_WRITE | PROT_READ, MAP_SHARED, seg_fd, 0);

		if (file_buffer == MAP_FAILED)
		{
			perror("mmap");
			exit(1);
		}
		
		// open semaphores
		sem_t *sem1 = sem_open(req_info->sem1_name, O_WRONLY | O_CREAT, 0644);
		sem_t *sem2 = sem_open(req_info->sem2_name, O_WRONLY | O_CREAT, 0644);
		// printf("sem1 name: %s\n", req_info->sem1_name);
		// printf("sem2 name: %s\n", req_info->sem2_name);

		// get cache file descriptor
		int fd = simplecache_get(req_info->path);
		printf("Cache Path : %s\n", req_info->path);

		// set status buffer
		response_info* res_info = (response_info*) file_buffer;

		if (fd < 0)
		{
			printf("File not found\n");
			res_info->file_len = -1;
			// int value;
			// sem_getvalue(sem1, &value);
			// printf("Cache Sem 1 before: %i\n", value);
			sem_post(sem1);
			// sem_getvalue(sem1, &value);
			// printf("Cache Sem 1 after: %i\n", value);
			
			sem_close(sem1);
			sem_close(sem2);
			munmap(file_buffer, segsize);	
			free(req_info);
			close(seg_fd);
			continue;
		}

		if (fstat(fd, &st) < 0)
		{
			res_info->file_len = -1;
			sem_post(sem1);
			sem_close(sem1);
			sem_close(sem2);
			munmap(file_buffer, segsize);	
			free(req_info);
			close(seg_fd);
			continue;
		} 

		// send header
		// printf("Seg name : %s\n", req_info->seg_name);
		file_len = (size_t)st.st_size;
		// printf("File len : %li\n", file_len);
		res_info->file_len = file_len;
		// printf("status : %li\n",status_buffer->file_len);
		
		// int value;
		// sem_getvalue(sem2, &value);
		// printf("Sem 2 before: %i\n", value);
		// Signal proxy to read segment
		sem_post(sem1);
			
		// send file content
		// int value;
		bytes_sent = 0;
		// printf("Bytes sent : %ld\n", bytes_sent);
		while (bytes_sent < file_len)
		{
			// Wait for proxy to signal that it is ready to send file content
			// sem_getvalue(sem2,&value);
			// printf("sem2 before : %i\n", value);
			sem_wait(sem2);
			// sem_timedwait(sem2, &timeout);
			// printf("sem2 after : %i\n", value);
			res_info->content_len = pread(fd, res_info->buffer, MAX_SIMPLE_CACHE_QUEUE_SIZE, bytes_sent);
			// printf("content len: %ld\n", res_info->content_len);
			if (res_info->content_len <= 0)
			{
				printf("Error reading file\n");
				// res_info->content_len = -1;
				sem_post(sem1);
				break;
			}

			bytes_sent += res_info->content_len;

			// Signal proxy to read next chunk of file content
			sem_post(sem1);
		}
		
		printf("bytes sent: %ld\n", bytes_sent);
		printf("Finished Path : %s\n", req_info->path);
		printf("Finished Segment : %s\n", req_info->seg_name);
		sem_close(sem1);
		sem_close(sem2);
		munmap(file_buffer, segsize);	
		free(req_info);
		close(seg_fd);
		// close(fd);

	}


}


void init_threads(size_t nthreads)
{
  static pthread_t *workers;
  workers = malloc(sizeof(pthread_t) * nthreads);
  for (int i = 0; i < nthreads; i++)
  {
    if (pthread_create(&workers[i], NULL, process_cache_request, NULL) != 0)
    {
      fprintf(stderr, "Can't create thread %d\n", i);
      exit(1);
    }

    // printf("Created thread %d\n", i);
  }
}

static void _sig_handler(int signo)
{
	if (signo == SIGTERM || signo == SIGINT)
	{	
		/*you should do IPC cleanup here*/
		exit_flag = 1;

		mq_close(mqdes);

		if (mq_unlink(QUEUE_NAME) == 0)
		{
			printf("unlinked message queue\n");
		}

		// while (!steque_isempty(cache_queue))
		// {
		// 	request_info* req_info = steque_pop(cache_queue);
		// 	free(req_info);

		// }

		printf("exitin\n");		
		steque_destroy(cache_queue);

		exit(signo);
	}
}

#define USAGE                                                                                            \
	"usage:\n"                                                                                           \
	"  simplecached [options]\n"                                                                         \
	"options:\n"                                                                                         \
	"  -c [cachedir]       Path to static files (Default: ./)\n"                                         \
	"  -t [thread_count]   Thread count for work queue (Default is 42, Range is 1-235711)\n"             \
	"  -d [delay]          Delay in simplecache_get (Default is 0, Range is 0-2500000 (microseconds)\n " \
	"  -h                  Show this help message\n"

// OPTIONS
static struct option gLongOptions[] = {
	{"cachedir", required_argument, NULL, 'c'},
	{"nthreads", required_argument, NULL, 't'},
	{"help", no_argument, NULL, 'h'},
	{"hidden", no_argument, NULL, 'i'},		 /* server side */
	{"delay", required_argument, NULL, 'd'}, // delay.
	{NULL, 0, NULL, 0}};

void Usage()
{
	fprintf(stdout, "%s", USAGE);
}

int main(int argc, char **argv)
{
	int nthreads = 10;
	char *cachedir = "locals.txt";
	char option_char;
	
	printf("Simplecached starting\n");
	/* disable buffering to stdout */
	setbuf(stdout, NULL);

	while ((option_char = getopt_long(argc, argv, "d:ic:hlt:x", gLongOptions, NULL)) != -1)
	{
		switch (option_char)
		{
		default:
			Usage();
			exit(1);
		case 't': // thread-count
			nthreads = atoi(optarg);
			break;
		case 'h': // help
			Usage();
			exit(0);
			break;
		case 'c': // cache directory
			cachedir = optarg;
			break;
		case 'd':
			cache_delay = (unsigned long int)atoi(optarg);
			break;
		case 'i': // server side usage
		case 'o': // do not modify
		case 'a': // experimental
			break;
		}
	}

	if (cache_delay > 2500001)
	{
		fprintf(stderr, "Cache delay must be less than 2500001 (us)\n");
		exit(__LINE__);
	}

	if ((nthreads > 211804) || (nthreads < 1))
	{
		fprintf(stderr, "Invalid number of threads must be in between 1-211804\n");
		exit(__LINE__);
	}
	if (SIG_ERR == signal(SIGINT, _sig_handler))
	{
		fprintf(stderr, "Unable to catch SIGINT...exiting.\n");
		exit(CACHE_FAILURE);
	}
	if (SIG_ERR == signal(SIGTERM, _sig_handler))
	{
		fprintf(stderr, "Unable to catch SIGTERM...exiting.\n");
		exit(CACHE_FAILURE);
	}

	printf("Initializing");
	/*Initialize cache*/
	simplecache_init(cachedir);

	struct mq_attr attr;
	attr.mq_flags = 0;
	attr.mq_maxmsg = 10;
	attr.mq_msgsize = MAX_CACHE_REQUEST_LEN;
	attr.mq_curmsgs = 0;
	// initialize cache queue
	cache_queue = malloc(sizeof(steque_t));
	steque_init(cache_queue);

	// initialize workers
	init_threads(nthreads);

	while (1)
	{	
		mqdes = mq_open(QUEUE_NAME, O_RDONLY | O_CREAT, 0777,&attr);
		// dynmically check if queue exists in case proxy server is restarted
		while (mqdes == (mqd_t)-1)
		{
			printf("Recreating message queue\n");
			mqdes = mq_open(QUEUE_NAME, O_RDONLY | O_CREAT, 0777,&attr);
		}
		request_info *req_info = (request_info*)malloc(MAX_CACHE_REQUEST_LEN);

		// printf("receiving message\n");
		int n = mq_receive(mqdes, (char *)req_info, MAX_CACHE_REQUEST_LEN, NULL);
		printf("message received : %s\n", ((request_info *)req_info)->seg_name);

		if (n == -1)
		{
			// printf("seg fd %s", request_info);
			printf("n %i", n);
			perror("mq_receive");
			printf("Error: %d \n ", errno);
			continue;
		}
		
		pthread_mutex_lock(&cache_mutex);
		steque_enqueue(cache_queue, req_info);
		pthread_mutex_unlock(&cache_mutex);
		pthread_cond_signal(&cache_cond);

		// printf("request info: %s\n", ((request_info *)req_info)->seg_name);

	}


	// Line never reached
	return -1;
}
