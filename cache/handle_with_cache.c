#include "gfserver.h"
#include "cache-student.h"

#define BUFSIZE (834)

/*
 __.__
Replace with your implementation
 __.__
*/
ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void* arg){
	size_t file_len;
    size_t bytes_transferred;
	char *data_dir = arg;
	ssize_t read_len;
    ssize_t write_len;
	char buffer[BUFSIZE];
	int fildes;
	struct stat statbuf;

	strncpy(buffer,data_dir, BUFSIZE);
	strncat(buffer,path, BUFSIZE);

	if( 0 > (fildes = open(buffer, O_RDONLY))){
		if (errno == ENOENT)
			//If the file just wasn't found, then send FILE_NOT_FOUND code
			return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
		else
			//Otherwise, it must have been a server error. gfserver library will handle
			return SERVER_FAILURE;
	}

	//Calculating the file size
	if (fstat(fildes, &statbuf) < 0) {
		return SERVER_FAILURE;
	}
	file_len = (size_t) statbuf.st_size;
	///

	gfs_sendheader(ctx, GF_OK, file_len);

	//Sending the file contents chunk by chunk

	bytes_transferred = 0;
	while(bytes_transferred < file_len){
		read_len = read(fildes, buffer, BUFSIZE);
		if (read_len <= 0){
			fprintf(stderr, "handle_with_file read error, %zd, %zu, %zu", read_len, bytes_transferred, file_len );
			return SERVER_FAILURE;
		}
		write_len = gfs_send(ctx, buffer, read_len);
		if (write_len != read_len){
			fprintf(stderr, "handle_with_file write error");
			return SERVER_FAILURE;
		}
		bytes_transferred += write_len;
	}

	return bytes_transferred;


}

