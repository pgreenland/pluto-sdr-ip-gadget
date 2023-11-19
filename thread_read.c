/* Use non portable functions */
#define _GNU_SOURCE

/* Public header */
#include "thread_read.h"

/* Standard / system libraries */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* libIIO */
#include <iio.h>

/* Local modules */
#include "sdr_ip_gadget_types.h"
#include "epoll_loop.h"
#include "utils.h"

/* Set the following to periodically report statistics */
#ifndef GENERATE_STATS
#define GENERATE_STATS (0)
#endif

/* Set stats period */
#ifndef STATS_PERIOD_SECS
#define STATS_PERIOD_SECS (5)
#endif

/* Macros */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define DEBUG_PRINT(...) if (debug) printf("Read: "__VA_ARGS__)

/* Type definitions */
typedef struct
{
	/* Thread args */
	THREAD_READ_Args_t *thread_args;

	/* Keep running */
	bool keep_running;

	/* IIO sample buffer */
	struct iio_buffer *iio_rx_buffer;

	/* Sample size */
	size_t sample_size;

	/* Expected IIO buffer size (bytes) */
	size_t iio_buffer_size;

	/* Current sequence number / timestamp */
	uint64_t seqno;

	#if GENERATE_STATS
	/* Stats reporting timer */
	int stats_timerfd;

	/* Overflow count */
	uint32_t overflows;

	/* Read period timer */
	UTILS_TimeStats_t read_period;

	/* Read duration timer */
	UTILS_TimeStats_t read_dur;
	#endif

} state_t;

/* Epoll event handler */
typedef int (*epoll_event_handler)(state_t *state);

/* Global variables */
extern bool debug;

/* Private functions */
static int handle_eventfd_thread(state_t *state);
static int handle_iio_buffer(state_t *state);
#if GENERATE_STATS
static int handle_stats_timer(state_t *state);
#endif

/* Public functions */
void *THREAD_READ_Entrypoint(void *args)
{
	THREAD_READ_Args_t *thread_args = (THREAD_READ_Args_t*)args;

	/* Enter */
	DEBUG_PRINT("Read thread enter\n");

	/* Set name, priority and CPU affinity */
	pthread_setname_np(pthread_self(), "IP_SDR_GAD_RD");
	UTILS_SetThreadRealtimePriority();
	UTILS_SetThreadAffinity(0);

	/* Reset state */
	state_t state;
	memset(&state, 0x00, sizeof(state));

	/* Store args */
	state.thread_args = thread_args;

	/* Create epoll instance */
	int epoll_fd = epoll_create1(0);
	if (epoll_fd < 0)
	{
		perror("Failed to create epoll instance");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Opened epoll :-)\n");
	}

	struct epoll_event epoll_event;

	/* Register thread quit eventfd with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_eventfd_thread;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, thread_args->quit_event_fd, &epoll_event) < 0)
	{
		perror("Failed to register thread quit eventfd with epoll");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Registered thread quit eventfd with with epoll :-)\n");
	}

	/* Create IIO context */
	struct iio_context *iio_ctx = iio_create_local_context();
	if (!iio_ctx)
	{
		fprintf(stderr, "Failed to open iio\n");
		return NULL;
	}

	/* Retrieve RX streaming device */
	struct iio_device *iio_dev_rx = iio_context_find_device(iio_ctx, "cf-ad9361-lpc");
	if (!iio_dev_rx)
	{
		fprintf(stderr, "Failed to open iio rx dev\n");
		return NULL;
	}

	/* Disable all channels */
	unsigned int nb_channels = iio_device_get_channels_count(iio_dev_rx);
	for (unsigned int i = 0; i < nb_channels; i++)
	{
		iio_channel_disable(iio_device_get_channel(iio_dev_rx, i));
	}

	/* Enable required channels */
	for (unsigned int i = 0; i < 32; i++)
	{
		/* Enable channel if required */
		if (thread_args->iio_channels & (1U << i))
		{
			/* Retrieve channel */
			struct iio_channel *channel = iio_device_get_channel(iio_dev_rx, i);
			if (!channel)
			{
				fprintf(stderr, "Failed to find iio rx chan %u\n", i);
				return false;
			}

			/* Enable channels */
			iio_channel_enable(channel);
		}
	}

	/* Create non-cyclic buffer */
	state.iio_rx_buffer = iio_device_create_buffer(iio_dev_rx, thread_args->iio_buffer_size, false);
	if (!state.iio_rx_buffer)
	{
		fprintf(stderr, "Failed to create rx buffer for %zu samples\n", thread_args->iio_buffer_size);
		return NULL;
	}

	/* Register buffer with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_iio_buffer;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, iio_buffer_get_poll_fd(state.iio_rx_buffer), &epoll_event) < 0)
	{
		/* Failed to register IIO buffer with epoll */
		perror("Failed to register IIO buffer with epoll");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Registered IIO buffer with with epoll :-)\n");
	}

	/* Retrieve number of bytes between two samples of the same channel (aka size of one sample of all enabled channels) */
	state.sample_size = iio_buffer_step(state.iio_rx_buffer);

	/* Calculate expected buffer size */
	state.iio_buffer_size = state.sample_size * thread_args->iio_buffer_size;

	/* Summarize info */
	DEBUG_PRINT("RX sample count: %zu, iio sample size: %zu, UDP packet size: %zu\n",
				thread_args->iio_buffer_size,
				state.sample_size,
				thread_args->udp_packet_size);

	#if GENERATE_STATS
	/* Create stats reporting timer */
	state.stats_timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (state.stats_timerfd < 0)
	{
		perror("Failed to open timerfd");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Opened timerfd :-)\n");
	}
	struct itimerspec timer_period =
	{
		.it_value = { .tv_sec = STATS_PERIOD_SECS, .tv_nsec = 0 },
		.it_interval = { .tv_sec = STATS_PERIOD_SECS, .tv_nsec = 0 }
	};
	if (timerfd_settime(state.stats_timerfd, 0, &timer_period, NULL) < 0)
	{
		perror("Failed to set timerfd");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Set timerfd :-)\n");
	}

	/* Register timer with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_stats_timer;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state.stats_timerfd, &epoll_event) < 0)
	{
		/* Failed to register timer with epoll */
		perror("Failed to register timer eventfd with epoll");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Registered timer with with epoll :-)\n");
	}

	/* Init timer */
	UTILS_ResetTimeStats(&state.read_period);
	UTILS_ResetTimeStats(&state.read_dur);
	#endif

	/* Enter main loop */
	DEBUG_PRINT("Enter read loop..\n");
	state.keep_running = true;
	while (state.keep_running)
	{
		if (EPOLL_LOOP_Run(epoll_fd, 30000, &state) < 0)
		{
			/* Epoll failed...bail */
			break;
		}
	}
	DEBUG_PRINT("Exit read loop..\n");

	/* Close / destroy everything */
	#if GENERATE_STATS
	close(state.stats_timerfd);
	#endif
	iio_buffer_destroy(state.iio_rx_buffer);
	iio_context_destroy(iio_ctx);
	close(epoll_fd);

	/* Exit */
	DEBUG_PRINT("Read thread exit\n");

	return NULL;
}

/* Private functions */
static int handle_eventfd_thread(state_t *state)
{
	/* Quit having detected write on eventfd */
	DEBUG_PRINT("Stop request received\n");
	state->keep_running = false;

	return 0;
}

static int handle_iio_buffer(state_t *state)
{
	#if GENERATE_STATS
	/* Capture read period */
	UTILS_UpdateTimeStats(&state->read_period);

	/* Record read start time */
	UTILS_StartTimeStats(&state->read_dur);
	#endif

	/* Refill buffer */
	ssize_t nbytes = iio_buffer_refill(state->iio_rx_buffer);
	if (nbytes != (ssize_t)state->iio_buffer_size)
	{
		fprintf(stderr, "RX buffer read failed, expected %zu, read %zd bytes\n", state->iio_buffer_size, nbytes);
		return -1;
	}

	#if GENERATE_STATS
	/* Capture read end time */
	UTILS_UpdateTimeStats(&state->read_dur);

	/* Record period start time (to subtract read time above) */
	UTILS_StartTimeStats(&state->read_period);
	#endif

	/* Retrieve buffer ptr */
	uint8_t *buffer = iio_buffer_start(state->iio_rx_buffer);
	size_t buffer_remaining = state->iio_buffer_size;

	if (state->thread_args->timestamping_enabled)
	{
		/* Update sequence number from IIO buffer, advance pointer, decrement size */
		state->seqno = *((uint64_t*)buffer);
		buffer += sizeof(uint64_t);
		buffer_remaining -= sizeof(uint64_t);
	}

	/* Prepare scatter/gather structures */
	struct iovec iov[2];
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &state->thread_args->addr;
	msg.msg_namelen = sizeof(state->thread_args->addr);
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;

	/* Prepare data packet header */
	data_ip_hdr_t pkr_hdr;
	pkr_hdr.magic = SDR_IP_GADGET_MAGIC;
	iov[0].iov_base = &pkr_hdr;
	iov[0].iov_len = sizeof(pkr_hdr);

	/* Split buffer into datagrams */
	while (buffer_remaining)
	{
		/* Set packet sequence number */
		pkr_hdr.seqno = state->seqno;

		/* Calculate amount of data to send */
		size_t buffer_to_send = state->thread_args->udp_packet_size - sizeof(data_ip_hdr_t);
		if (buffer_to_send > buffer_remaining)
		{
			/* Limit send size to remaining buffer size */
			buffer_to_send = buffer_remaining;
		}
		else if (0 != (buffer_to_send % state->sample_size))
		{
			/* Do not split samples across buffers, reduce number of samples sent, round down */
			buffer_to_send = (buffer_to_send / state->sample_size) * state->sample_size;
		}

		/* Prepare data section of datagram */
		iov[1].iov_base = buffer;
		iov[1].iov_len = buffer_to_send;

		/* Send datagram */
		if (-1 == sendmsg(state->thread_args->output_fd, &msg, 0))
		{
			/* Send failed */
			#if GENERATE_STATS
			/* Count overflow */
			state->overflows++;
			#endif
		}

		/* Advance pointers and reduce sizes */
		buffer += buffer_to_send;
		buffer_remaining -= buffer_to_send;

		/* Advance sequence number by samples sent */
		state->seqno += (buffer_to_send / state->sample_size);
	}

	return 0;
}

#if GENERATE_STATS
static int handle_stats_timer(state_t *state)
{
	/* Read timer to acknowledge it */
	uint64_t timerfd_val;
	if (read(state->stats_timerfd, &timerfd_val, sizeof(timerfd_val)) < 0)
	{
		perror("Failed to read timerfd");
		return 1;
	}

	/* Report min/max/average read period */
	printf("Read period: min: %"PRIu64", max: %"PRIu64", avg: %"PRIu64" (uS)\n",
		   state->read_period.min,
		   state->read_period.max,
		   UTILS_CalcAverageTimeStats(&state->read_period)
	);

	/* Report min/max/average read duration */
	printf("Read dur: min: %"PRIu64", max: %"PRIu64", avg: %"PRIu64" (uS)\n",
		   state->read_dur.min,
		   state->read_dur.max,
		   UTILS_CalcAverageTimeStats(&state->read_dur)
	);

	/* Check for overflows */
	if (state->overflows > 0)
	{
		printf("Read overflows: %u in last 5s period\n", state->overflows);
	}

	/* Reset stats */
	UTILS_ResetTimeStats(&state->read_period);
	UTILS_ResetTimeStats(&state->read_dur);
	state->overflows = 0;

	return 0;
}
#endif
