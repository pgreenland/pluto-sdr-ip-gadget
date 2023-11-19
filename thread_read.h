#ifndef __THREAD_READ_H__
#define __THREAD_READ_H__

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

	/* UDP socket to write to */
	int output_fd;

	/* Client address */
	struct sockaddr_in addr;

	/* Enabled channels */
	uint32_t iio_channels;

	/* Timestamping enabled */
	bool timestamping_enabled;

	/* Sample buffer size (in samples) */
	size_t iio_buffer_size;

	/* UDP packet size (in bytes) */
	size_t udp_packet_size;

} THREAD_READ_Args_t;

/* Public functions - Thread entrypoint */
void *THREAD_READ_Entrypoint(void *args);

#endif
