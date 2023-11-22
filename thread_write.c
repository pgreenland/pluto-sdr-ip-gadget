/* Use non portable functions */
#define _GNU_SOURCE

/* Public header */
#include "thread_write.h"

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
#include <syscall.h>
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
#define DEBUG_PRINT(...) if (debug) printf("Write: "__VA_ARGS__)

/* Type definitions */
typedef struct
{
	/* Thread args */
	THREAD_WRITE_Args_t *thread_args;

	/* Keep running */
	bool keep_running;

	/* IIO sample buffer */
	struct iio_buffer *iio_tx_buffer;

	/* Sample size */
	size_t sample_size;

	/* Expected IIO buffer size (bytes) */
	size_t iio_buffer_size;

	/* Buffer size in (samples, excluding timestamp) */
	size_t buffer_size_samples;

	/* Current block index / count */
	uint8_t block_index;
	uint8_t block_count;

	/* Current sequence number / timestamp */
	uint64_t seqno;

	/* Current amount of IIO buffer space used (bytes) */
	size_t iio_buffer_used;

	#if GENERATE_STATS
	/* Stats reporting timer */
	int stats_timerfd;

	/* Drop count (due to bad seq no) */
	uint32_t dropped;

	/* Partial buffer pushes (due to out of order seq no) */
	uint32_t outoforder;

	/* Overflow count */
	uint32_t overflows;

	/* Write period timer */
	UTILS_TimeStats_t write_period;

	/* Write duration timer */
	UTILS_TimeStats_t write_dur;
	#endif

} state_t;

/* Epoll event handler */
typedef int (*epoll_event_handler)(state_t *state);

/* Global variables */
extern bool debug;

/* Private functions */
static int handle_eventfd_thread(state_t *state);
static int handle_socket(state_t *state);
#if GENERATE_STATS
static int handle_stats_timer(state_t *state);
#endif

/* Public functions */
void *THREAD_WRITE_Entrypoint(void *args)
{
	THREAD_WRITE_Args_t *thread_args = (THREAD_WRITE_Args_t*)args;

	/* Enter */
	DEBUG_PRINT("Write thread enter (tid: %ld)\n", syscall(SYS_gettid));

	/* Set name, priority and CPU affinity */
	pthread_setname_np(pthread_self(), "IP_SDR_GAD_WR");
	UTILS_SetThreadRealtimePriority();
	UTILS_SetThreadAffinity(1);

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

	/* Retrieve TX streaming device */
	struct iio_device *iio_dev_tx = iio_context_find_device(iio_ctx, "cf-ad9361-dds-core-lpc");
	if (!iio_dev_tx)
	{
		fprintf(stderr, "Failed to open iio tx dev\n");
		return NULL;
	}

	/* Disable all channels */
	unsigned int nb_channels = iio_device_get_channels_count(iio_dev_tx);
	for (unsigned int i = 0; i < nb_channels; i++)
	{
		iio_channel_disable(iio_device_get_channel(iio_dev_tx, i));
	}

	/* Enable required channels */
	for (unsigned int i = 0; i < 32; i++)
	{
		/* Enable channel if required */
		if (thread_args->iio_channels & (1U << i))
		{
			/* Retrieve channel */
			struct iio_channel *channel = iio_device_get_channel(iio_dev_tx, i);
			if (!channel)
			{
				fprintf(stderr, "Failed to find iio rx chan %u\n", i);
				return NULL;
			}

			/* Enable channels */
			iio_channel_enable(channel);
		}
	}

	/* Create non-cyclic buffer */
	state.iio_tx_buffer = iio_device_create_buffer(iio_dev_tx, thread_args->iio_buffer_size, false);
	if (!state.iio_tx_buffer)
	{
		fprintf(stderr, "Failed to create tx buffer for %zu samples\n", thread_args->iio_buffer_size);
		return NULL;
	}

	/* Retrieve number of bytes between two samples of the same channel (aka size of one sample of all enabled channels) */
	state.sample_size = iio_buffer_step(state.iio_tx_buffer);

	/* Calculate expected buffer size */
	state.iio_buffer_size = state.sample_size * thread_args->iio_buffer_size;

	/* Calculate buffer size, excluding timestamp */
	state.buffer_size_samples = thread_args->iio_buffer_size;
	if (thread_args->timestamping_enabled)
	{
		state.buffer_size_samples -= (sizeof(uint64_t) / state.sample_size);
	}

	/* Summarize info */
	DEBUG_PRINT("TX sample count: %zu, iio sample size: %zu\n",
				thread_args->iio_buffer_size,
				state.sample_size);

	/* Register data socket with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_socket;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state.thread_args->input_fd, &epoll_event) < 0)
	{
		perror("Failed to register data socket readable with epoll");
		return NULL;
	}
	else
	{
		DEBUG_PRINT("Registered data socket readable with epoll :-)\n");
	}

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

	/* Init timers */
	UTILS_ResetTimeStats(&state.write_period);
	UTILS_ResetTimeStats(&state.write_dur);
	#endif

	/* Enter main loop */
	DEBUG_PRINT("Enter write loop..\n");
	state.keep_running = true;
	while (state.keep_running)
	{
		if (EPOLL_LOOP_Run(epoll_fd, 30000, &state) < 0)
		{
			/* Epoll failed...bail */
			break;
		}
	}
	DEBUG_PRINT("Exit write loop..\n");

	/* Close / destroy everything */
	#if GENERATE_STATS
	close(state.stats_timerfd);
	#endif
	iio_buffer_destroy(state.iio_tx_buffer);
	iio_context_destroy(iio_ctx);
	close(epoll_fd);

	/* Exit */
	DEBUG_PRINT("Write thread exit\n");

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

static int handle_socket(state_t *state)
{
	/* Prepare scatter/gather structures */
	struct iovec iov[2];
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;

	/* Prepare data packet header */
	data_ip_hdr_t pkt_hdr;
	iov[0].iov_base = &pkt_hdr;
	iov[0].iov_len = sizeof(pkt_hdr);

	/* Retrieve buffer address */
	uint8_t *buffer = iio_buffer_start(state->iio_tx_buffer);

	/* Read until socket exhausted (hoping to receive enough packets to fill the buffer) */
	for (;;)
	{
		size_t buffer_offset = state->iio_buffer_used;

		if (	(0 == state->iio_buffer_used)
			 && (state->thread_args->timestamping_enabled)
		   )
		{
			/* Reserve space at head of buffer for timestamp */
			buffer_offset += sizeof(uint64_t);
		}

		/* Prepare buffer pointer */
		iov[1].iov_base = &buffer[buffer_offset];
		iov[1].iov_len = state->iio_buffer_size - buffer_offset;

		/* Receive into buffers */
		int rc = recvmsg(state->thread_args->input_fd, &msg, 0);
		if (-1 == rc)
		{
			/* Receive failed, check for EAGAIN, which is fine, we ran out of data */
			if ((EWOULDBLOCK != errno) && (EAGAIN != errno))
			{
				/* Oh dear, a "bad" error */
				perror("Receive failed");
				return 1;
			}
			break;
		}

		/* Receive succeeded, what did we win? Check magic */
		if (	((size_t)rc < sizeof(data_ip_hdr_t))
			 || (SDR_IP_GADGET_MAGIC != pkt_hdr.magic)
		   )
		{
			/* Wrong header size or bad magic, possibly a naughty network application or an honest mistake */
			continue;
		}

		/* Remove packet header length from data remaining */
		rc -= sizeof(data_ip_hdr_t);

		/*
		** Check packet sequence number / timestamp, discarding any out of order packets
		** Note this is fragile against time warps
		*/
		if (pkt_hdr.seqno < state->seqno)
		{
			DEBUG_PRINT("Drop seq\n");
			#if GENERATE_STATS
			/* Count dropped datagram */
			state->dropped++;
			#endif
			continue;
		}

		if (0 == state->iio_buffer_used)
		{
			/* Check packet starts sequence */
			if (0 != pkt_hdr.block_index)
			{
				DEBUG_PRINT("Drop index\n");
				#if GENERATE_STATS
				/* Count dropped datagram */
				state->dropped++;
				#endif

				/* Drop packet, waiting for sequence start */
				continue;
			}

			/* Reset index and store total */
			state->block_index = 0;
			state->block_count = pkt_hdr.block_count;

			/* Is timestamping enabled? */
			if (state->thread_args->timestamping_enabled)
			{
				/* Yes, copy timestamp from header to working data and start of buffer */
				state->seqno = pkt_hdr.seqno;
				*((uint64_t*)buffer) = state->seqno;
			}
		}
		else
		{
			/* Check index, total and timestamp match */
			if (	(state->block_index != pkt_hdr.block_index)
				 || (state->block_count != pkt_hdr.block_count)
				 || (state->seqno != pkt_hdr.seqno)
			   )
			{
				DEBUG_PRINT("Drop OOO\n");

				if (state->block_index != pkt_hdr.block_index) DEBUG_PRINT("OOO: index, exp: %u, got: %u\n", (unsigned int)state->block_index, (unsigned int)pkt_hdr.block_index);
				if (state->block_count != pkt_hdr.block_count) DEBUG_PRINT("OOO: count, exp: %u, got: %u\n", (unsigned int)state->block_count, (unsigned int)pkt_hdr.block_count);
				if (state->seqno != pkt_hdr.seqno) DEBUG_PRINT("OOO: seq, exp: %"PRIu64", got: %"PRIu64"\n", state->seqno, pkt_hdr.seqno);

				/* Either an out of order, or duplicate block */
				#if GENERATE_STATS
				/* Count out-of-order datagram */
				state->outoforder++;
				#endif

				/* Reset buffer */
				state->iio_buffer_used = 0;

				/* Drop packet */
				continue;
			}
		}

		/* Update buffer used */
		if (	(0 == state->iio_buffer_used)
			 && (state->thread_args->timestamping_enabled)
		   )
		{
			state->iio_buffer_used += sizeof(uint64_t);
		}
		state->iio_buffer_used += (size_t)rc;

		/* Advance index */
		state->block_index++;

		/* Is buffer full? */
		if (state->iio_buffer_size == state->iio_buffer_used)
		{
			/* Yep, get ready to send it */
			#if GENERATE_STATS
			/* Capture write period */
			UTILS_UpdateTimeStats(&state->write_period);

			/* Record write start time */
			UTILS_StartTimeStats(&state->write_dur);
			#endif

			/* Perform blocking write */
			ssize_t nbytes = iio_buffer_push(state->iio_tx_buffer);
			if (nbytes != (ssize_t)state->iio_buffer_size)
			{
				#if GENERATE_STATS
				/* Count overflow */
				state->overflows++;
				#endif
			}

			#if GENERATE_STATS
			/* Capture write end time */
			UTILS_UpdateTimeStats(&state->write_dur);

			/* Record period start time (to subtract write time above) */
			UTILS_StartTimeStats(&state->write_period);
			#endif

			/* Reset buffer used */
			state->iio_buffer_used = 0;

			/* Advance sequence number */
			state->seqno += state->buffer_size_samples;

			/* Break to main loop having handled an entire iio buffer */
			break;
		}
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

	/* Report min/max/average write period */
	printf("Write period: min: %"PRIu64", max: %"PRIu64", avg: %"PRIu64" (uS)\n",
		   state->write_period.min,
		   state->write_period.max,
		   UTILS_CalcAverageTimeStats(&state->write_period)
	);

	/* Report min/max/average write duration */
	printf("Write dur: min: %"PRIu64", max: %"PRIu64", avg: %"PRIu64" (uS)\n",
		   state->write_dur.min,
		   state->write_dur.max,
		   UTILS_CalcAverageTimeStats(&state->write_dur)
	);

	/* Check for overflows */
	if (state->overflows > 0)
	{
		printf("Write overflows: %u in last 5s period\n", state->overflows);
	}

	/* Check for dropped */
	if (state->dropped > 0)
	{
		printf("Write dropped: %u in last 5s period\n", state->dropped);
	}

	/* Check for out of order */
	if (state->outoforder > 0)
	{
		printf("Write outoforder: %u in last 5s period\n", state->outoforder);
	}

	/* Reset stats */
	UTILS_ResetTimeStats(&state->write_period);
	UTILS_ResetTimeStats(&state->write_dur);
	state->overflows = 0;
	state->dropped = 0;
	state->outoforder = 0;

	return 0;
}
#endif
