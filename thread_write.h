#ifndef __THREAD_WRITE_H__
#define __THREAD_WRITE_H__

/* Standard libraries */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

/* Type definitions - thread args */
typedef struct
{
	/* Eventfd used to signal thread to quit */
	int quit_event_fd;

	/* UDP socket to read from */
	int input_fd;

	/* Client address */
	struct sockaddr_in addr;

	/* Enabled channels */
	uint32_t iio_channels;

	/* Timestamping enabled */
	bool timestamping_enabled;

	/* Sample buffer size (in samples) */
	size_t iio_buffer_size;

} THREAD_WRITE_Args_t;

/* Public functions - Thread entrypoint */
void *THREAD_WRITE_Entrypoint(void *args);

#endif
