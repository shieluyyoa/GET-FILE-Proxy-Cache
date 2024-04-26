#include "gfserver.h"
#include "cache-student.h"
#include <mqueue.h>

#define BUFSIZE (834)
#define QUEUE_NAME "/cache_queue"

extern pthread_mutex_t seg_mutex;
extern pthread_cond_t seg_cleanup_cond;
extern pthread_cond_t seg_cond;
extern steque_t *seg_queue;
extern int exit_flag;

struct timespec timeout = {10, 0};

// ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void *arg)
// {
// 	size_t file_len;
// 	size_t bytes_transferred;
// 	char *data_dir = arg;
// 	ssize_t read_len;
// 	ssize_t write_len;
// 	char buffer[BUFSIZE];
// 	int fildes;
// 	struct stat statbuf;

// 	strncpy(buffer, data_dir, BUFSIZE);
// 	strncat(buffer, path, BUFSIZE);

// 	if (0 > (fildes = open(buffer, O_RDONLY)))
// 	{
// 		if (errno == ENOENT)
// 			// If the file just wasn't found, then send FILE_NOT_FOUND code
// 			return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
// 		else
// 			// Otherwise, it must have been a server error. gfserver library will handle
// 			return SERVER_FAILURE;
// 	}

// 	// Calculating the file size
// 	if (fstat(fildes, &statbuf) < 0)
// 	{
// 		return SERVER_FAILURE;
// 	}
// 	file_len = (size_t)statbuf.st_size;
// 	///

// 	gfs_sendheader(ctx, GF_OK, file_len);

// 	// Sending the file contents chunk by chunk

// 	bytes_transferred = 0;
// 	while (bytes_transferred < file_len)
// 	{
// 		read_len = read(fildes, buffer, BUFSIZE);
// 		if (read_len <= 0)
// 		{
// 			fprintf(stderr, "handle_with_file read error, %zd, %zu, %zu", read_len, bytes_transferred, file_len);
// 			return SERVER_FAILURE;
// 		}
// 		write_len = gfs_send(ctx, buffer, read_len);
// 		if (write_len != read_len)
// 		{
// 			fprintf(stderr, "handle_with_file write error");
// 			return SERVER_FAILURE;
// 		}
// 		bytes_transferred += write_len;
// 	}

// 	return bytes_transferred;
// }

ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void *arg)
{
	int file_len;
	size_t bytes_sent;
	request_info req_info;
	mqd_t mqdes;
	seg_info *seg;
	

	// initialize message queue
	mqdes = mq_open(QUEUE_NAME, O_WRONLY);
	// printf("thread id : %lu\n", pthread_self());
	// dynmically check if queue exists in case proxy server is restarted
	while (mqdes == (mqd_t)-1)
	{
		// printf("Reconnecting to message queue\n");
		mqdes = mq_open(QUEUE_NAME, O_WRONLY);
	}
	
	printf("New Message Queue\n");

	// acquire lock and pop seg info from queue
	pthread_mutex_lock(&seg_mutex);
	while (steque_isempty(seg_queue))
	{
		if (exit_flag)
        {
          pthread_mutex_unlock(&seg_mutex);
          pthread_cond_signal(&seg_cleanup_cond);
          return 0;
        }
		pthread_cond_wait(&seg_cond, &seg_mutex);
	}
	seg = steque_pop(seg_queue);
	pthread_mutex_unlock(&seg_mutex);

	strcpy(req_info.path, path);
	strcpy(req_info.seg_name, seg->seg_name);
	strcpy(req_info.sem1_name, seg->sem1_name);
	strcpy(req_info.sem2_name, seg->sem2_name);
	req_info.segsize = seg->segsize;

	// void *seg_map = mmap(NULL, seg->segsize, PROT_READ, MAP_SHARED, seg->seg_fd, 0);

    // if (seg == MAP_FAILED)
    // {
    //   perror("mmap");
    //   exit(1);
    // }

	if ((seg->sem1 = sem_open(seg->sem1_name, O_CREAT, 0644, 0)) == SEM_FAILED)
    {
      perror("sem_open");
      exit(1);
    }

    if ((seg->sem2 = sem_open(seg->sem2_name, O_CREAT, 0644, 1)) == SEM_FAILED)
    {
      perror("sem_open");
      exit(1);
    }
	
	printf("message sent : %s\n", req_info.seg_name);
	printf("Sending Path : %s\n", req_info.path);
	mq_send(mqdes, (const char *)&req_info, sizeof(req_info), 0);

	// Wait for signal to read segment
	// int value;
	// sem_getvalue(seg->sem1, &value);
	// printf("Proxy Sem 1 before: %i\n", value);
	sem_wait(seg->sem1);
	// sem_timedwait(seg->sem1, &timeout);
	// sem_getvalue(seg->sem1, &value);
	// printf("Proxy Sem 1 after: %i\n", value);

	// Get file len (status)
	response_info *file_buffer = (response_info*) seg->seg;
	file_len = file_buffer->file_len;
	// printf("Seg name : %s\n", seg->seg_name);
	// printf("seg before : %p\n", seg->seg);
	printf("File length %li\n", file_buffer->file_len);

	// Send header
	if (file_len < 0)
	{
		printf("FILE NOT FOUND\n");
		gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
		// cleanup
		mq_close(mqdes);
		sem_close(seg->sem1);
		sem_close(seg->sem2);
		sem_unlink(seg->sem1_name);
		sem_unlink(seg->sem2_name);
		// munmap(seg_map, seg->segsize);


		pthread_mutex_lock(&seg_mutex);
		steque_enqueue(seg_queue, seg);
		pthread_mutex_unlock(&seg_mutex);
		pthread_cond_signal(&seg_cond);

		return 0;
	}

	ctx->file_len = file_len;
	gfs_sendheader(ctx, GF_OK, file_len);

	// Get File Content
	bytes_sent = 0;

	while (bytes_sent < file_len)
	{	
		// Signal cache to write first chunk of file content
		// Wait for cache to signal that it is ready to read file content
		sem_wait(seg->sem1);
;
		if (file_buffer->content_len <= 0)
		{
			printf("Error reading file\n");
			// gfs_send(ctx, file_buffer->buffer, 0);
			break;
		}

		bytes_sent += gfs_send(ctx, file_buffer->buffer, file_buffer->content_len);

		// Signal cache to read next chunk of file content
		sem_post(seg->sem2);

	}

	printf("Bytes sent: %ld\n", bytes_sent);
	printf("Finished Path : %s\n", req_info.path);
	printf("Finished Segment : %s\n", req_info.seg_name);


	ctx->bytes_transferred = bytes_sent;
	
	// cleanup
	mq_close(mqdes);
	sem_close(seg->sem1);
	sem_close(seg->sem2);
	sem_unlink(seg->sem1_name);
	sem_unlink(seg->sem2_name);
	// munmap(seg_map, seg->segsize);

	// recycle segment by adding it back to queue
	pthread_mutex_lock(&seg_mutex);
	steque_enqueue(seg_queue, seg);
	pthread_mutex_unlock(&seg_mutex);
	pthread_cond_signal(&seg_cond);

	return bytes_sent;
}