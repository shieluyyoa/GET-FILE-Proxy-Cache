#include "proxy-student.h"
#include "gfserver.h"

#define MAX_REQUEST_N 824
#define BUFSIZE (6200)

static size_t writecb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	gfcontext_t *ctx = (gfcontext_t *)userdata;
	int numbytes = size * nmemb;
	gfs_send(ctx, ptr, numbytes);
	return numbytes;
}

// static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
// {
// 	int content_len;
// 	int status;
// 	gfcontext_t *ctx = (gfcontext_t *)userdata;
// 	int numbytes = size * nitems;
// 	// printf("header : %.*s\n", numbytes, buffer);
// 	if (sscanf(buffer, "HTTP/2 %d", &status) == 1)
// 	{
// 		printf("status: %d\n", status);
// 		if (status == 404)
// 		{
// 			gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
// 			return numbytes;
// 		}

// 		if (status >= 400 && status != 404)
// 		{
// 			gfs_sendheader(ctx, GF_ERROR, 0);
// 			return numbytes;
// 		}
// 	}

// 	if (sscanf(buffer, "content-length: %d", &content_len) == 1)
// 	{
// 		printf("Content-Length: %d\n", content_len);
// 		ctx->file_len = content_len;
// 		gfs_sendheader(ctx, GF_OK, content_len);
// 		return numbytes;
// 	}
// 	return numbytes;
// }

ssize_t handle_with_curl(gfcontext_t *ctx, const char *path, void *arg)
{
	(void)ctx;
	(void)arg;
	(void)path;

	char *server = (char *)arg;
	char url[BUFSIZE];
	strcpy(url, server);
	strcat(url, path);
	printf("url %s\n", url);

	ctx->bytes_transferred = 0;
	CURL *eh = curl_easy_init();
	if (eh)
	{
		curl_easy_setopt(eh, CURLOPT_URL, url);
		// curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, header_callback);
		// curl_easy_setopt(eh, CURLOPT_HEADERDATA, ctx);
		// curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, writecb);
		// curl_easy_setopt(eh, CURLOPT_WRITEDATA, ctx);

		curl_easy_setopt(eh, CURLOPT_NOBODY, 1L);

		CURLcode res;
		res = curl_easy_perform(eh);

		if (res != CURLE_OK)
		{
			fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
			curl_easy_cleanup(eh);
			return SERVER_FAILURE;
		}

		long response_code;
		res = curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &response_code);
		printf("Response code: %ld\n", response_code);
		printf("res %d\n", res);

		if (res != CURLE_OK)
		{
			fprintf(stderr, "curl_easy_getinfo() failed: %s\n", curl_easy_strerror(res));
			curl_easy_cleanup(eh);
			return SERVER_FAILURE;
		}

		if (response_code >= 400)
		{
			gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
			curl_easy_cleanup(eh);
			return SERVER_FAILURE;
		}

		curl_off_t cl;
		res = curl_easy_getinfo(eh, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);

		if (res != CURLE_OK)
		{
			fprintf(stderr, "curl_easy_getinfo() failed: %s\n", curl_easy_strerror(res));
			curl_easy_cleanup(eh);
			return SERVER_FAILURE;
		}

		printf("Download size: %" CURL_FORMAT_CURL_OFF_T "\n", cl);
		gfs_sendheader(ctx, GF_OK, cl);
		curl_easy_reset(eh);
		curl_easy_setopt(eh, CURLOPT_URL, url);
		curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, writecb);
		curl_easy_setopt(eh, CURLOPT_WRITEDATA, ctx);
		res = curl_easy_perform(eh);
		if (res != CURLE_OK)
		{
			fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
			curl_easy_cleanup(eh);
			return SERVER_FAILURE;
		}

		// if (res == CURLE_HTTP_RETURNED_ERROR)
		// {
		// 	fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		// 	gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
		// 	return -1;
		// }

		// if (res != CURLE_OK)
		// {
		// 	fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		// 	gfs_sendheader(ctx, GF_ERROR, 0);
		// 	curl_easy_cleanup(eh);
		// 	return SERVER_FAILURE;
		// }

		// long response_code;
		// curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &response_code);
		// if (response_code == 404)
		// {
		// 	printf("404\n");
		// 	gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
		// 	return -1;
		// }

		curl_easy_cleanup(eh);
	}

	printf("file len %zu bytes transferred %ld\n", ctx->file_len, ctx->bytes_transferred);

	return ctx->bytes_transferred;
}

/*
 * We provide a dummy version of handle_with_file that invokes handle_with_curl
 * as a convenience for linking.  We recommend you simply modify the proxy to
 * call handle_with_curl directly

 __.__
 */
ssize_t handle_with_file(gfcontext_t *ctx, const char *path, void *arg)
{
	return handle_with_curl(ctx, path, arg);
}
