#include "proxy-student.h"
#include "gfserver.h"

#define MAX_REQUEST_N 824
#define BUFSIZE (6200)

/*
 * Replace with an implementation of handle_with_curl and
 * any other functions you may need 

 __.__
 */

ssize_t handle_with_curl(gfcontext_t *ctx, const char *path, void* arg){
	(void) ctx;
	(void) arg;
	(void) path;
	/* not implemented */

	errno = ENOSYS;
	return -1;	
}

/*
 * We provide a dummy version of handle_with_file that invokes handle_with_curl
 * as a convenience for linking.  We recommend you simply modify the proxy to
 * call handle_with_curl directly

 __.__
 */
ssize_t handle_with_file(gfcontext_t *ctx, const char *path, void* arg){
	return handle_with_curl(ctx, path, arg);
}	
