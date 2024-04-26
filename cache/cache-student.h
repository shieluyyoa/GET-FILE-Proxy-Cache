/*
 *  To be used by students
 */
#ifndef __CACHE_STUDENT_H__842

#define __CACHE_STUDENT_H__842

#include "steque.h"
#include <semaphore.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */

#define BUFSIZE (834)
#define QUEUE_NAME "/cache_queue"

#define MAX_CACHE_REQUEST_LEN 6200
#define MAX_SIMPLE_CACHE_QUEUE_SIZE 826


typedef struct seg_info
{
  void* seg;
  // int seg_fd;
  char seg_name[10];
  char sem1_name[10];
  char sem2_name[10];
  sem_t *sem1;
  sem_t *sem2;
  size_t segsize;

} seg_info;

typedef struct request_info
{
  char path[BUFSIZE];
  char seg_name[10];
  char sem1_name[10];
  char sem2_name[10];
  size_t segsize;
} request_info;

typedef struct response_info
{
  size_t file_len;
  ssize_t content_len;
  char buffer[BUFSIZE];
} response_info;


#endif // __CACHE_STUDENT_H__842