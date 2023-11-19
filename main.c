/* Standard / system libraries */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

/* libIIO */
#include <iio.h>

/* Local modules */
#include "sdr_ip_gadget_types.h"
#include "epoll_loop.h"
#include "thread_read.h"
#include "thread_write.h"

/* Macros */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define DEBUG_PRINT(...) if (debug) printf("Main: "__VA_ARGS__)

/* Definitions - UDP port numbers */
#define DIRECT_IP_PORT_CONTROL (30432) // IIOD + 1
#define DIRECT_IP_PORT_DATA (30433) // IIOD + 2

/* Type definitions */
typedef struct
{
	/* Socket file descriptors */
	int sock_control;
	int sock_data;

	/* Eventfds to signal threads */
	int read_thread_event_fd;
	int write_thread_event_fd;

	/* Thread status */
	bool read_started;
	bool write_started;

	/* Thread arguments */
	THREAD_READ_Args_t read_args;
	THREAD_WRITE_Args_t write_args;

	/* Threads */
	pthread_t thread_read;
	pthread_t thread_write;

} state_t;

/* Epoll event handler */
typedef int (*epoll_event_handler)(state_t *state);

/* Global variables */
bool debug;

/* Private function */
static int handle_control(state_t *state);
static bool start_thread(state_t *state, bool tx);
static bool stop_thread(state_t *state, bool tx);
static void signal_handler(int signum);
static void print_usage(const char *program_name, FILE *dest);

/* Private variables */
static volatile sig_atomic_t keep_running = 1;

/* Public functions */
int main(int argc, char *argv[])
{
	state_t state;
	struct sockaddr_in addr;

	/* Reset state */
	memset(&state, 0x00, sizeof(state));

	/* Ensure stdout is line buffered */
	setlinebuf(stdout);

	/* Hello world */
	printf("Welcome!\n");
	printf("--------\n");

	/* Long options array, mapping options to their short equivalents */
	struct option long_options[] = {
		{"debug", no_argument, NULL, 'd'},
		{"version", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0} // Terminate the options array
	};

	/* Basic argument parsing */
	int opt_c;
	bool err = false;
	while ((opt_c = getopt_long(argc, argv, "dhv", long_options, NULL)) != -1)
	{
			switch (opt_c)
			{
				case 'd':
				{
					debug = true;
					break;
				}
				case 'v':
				{
					printf("Version %s\n", PROGRAM_VERSION);
					return 0;
				}
				case 'h':
				{
					print_usage(argv[0], stdout);
					return 0;
				}
				case '?':
				{
					err = true;
					break;
				}
			}
	}
	if (err)
	{
		/* Unrecognised argument */
		fprintf(stderr, "Error: Unrecognised argument\n");
		print_usage(argv[0], stderr);
		return 1;
	}

	/* Register signal handler */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Open sockets */
	state.sock_control = socket(AF_INET, SOCK_DGRAM, 0);
	if (state.sock_control < 0)
	{
		perror("Failed to open control socket");
		return false;
	}
	else
	{
		DEBUG_PRINT("Opened control socket :-)\n");
	}
	state.sock_data = socket(AF_INET, SOCK_DGRAM, 0);
	if (state.sock_data < 0)
	{
		perror("Failed to open data socket");
		return false;
	}
	else
	{
		DEBUG_PRINT("Opened data socket :-)\n");
	}

	/* Place sockets in non-blocking mode */
	if (fcntl(state.sock_control, F_SETFL, fcntl(state.sock_control, F_GETFL, 0) | O_NONBLOCK))
	{
		perror("Failed to set control socket mode to non-blocking");
		return 1;
	}
	if (fcntl(state.sock_data, F_SETFL, fcntl(state.sock_data, F_GETFL, 0) | O_NONBLOCK))
	{
		perror("Failed to set data socket mode to non-blocking");
		return 1;
	}

	/* Bind sockets */
	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(DIRECT_IP_PORT_CONTROL);
	if (bind(state.sock_control, (const struct sockaddr *)&addr, sizeof(addr)))
	{
		perror("Failed to bind control socket");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Bound control socket :-)\n");
	}
	addr.sin_port = htons(DIRECT_IP_PORT_DATA);
	if (bind(state.sock_data, (const struct sockaddr *)&addr, sizeof(addr)))
	{
		perror("Failed to bind data socket");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Bound data socket :-)\n");
	}

	/* Prepare eventfds to notify threads to cancel */
	state.read_thread_event_fd = eventfd(0, 0);
	if (state.read_thread_event_fd < 0)
	{
		perror("Failed to open read eventfd");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Opened read eventfd :-)\n");
	}
	state.write_thread_event_fd = eventfd(0, 0);
	if (state.write_thread_event_fd < 0)
	{
		perror("Failed to open write eventfd");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Opened write eventfd :-)\n");
	}

	/* Prepare read args */
	state.read_args.quit_event_fd = state.read_thread_event_fd;
	state.read_args.output_fd = state.sock_data;

	/* Prepare write args */
	state.write_args.quit_event_fd = state.write_thread_event_fd;
	state.write_args.input_fd = state.sock_data;

	/* Create epoll instance */
	int epoll_fd = epoll_create1(0);
	if (epoll_fd < 0)
	{
		perror("Failed to create epoll instance");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Opened epoll :-)\n");
	}

	struct epoll_event epoll_event;

	/* Register control socket with epoll */
	epoll_event.events = EPOLLIN;
	epoll_event.data.ptr = handle_control;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state.sock_control, &epoll_event) < 0)
	{
		/* Failed to register control socket with epoll */
		perror("Failed to register control socket with epoll");
		return 1;
	}
	else
	{
		DEBUG_PRINT("Registered control socket with epoll :-)\n");
	}

	/* Here we go */
	printf("Ready :-)\n");

	/* Enter main loop */
	DEBUG_PRINT("Enter main loop..\n");
	while (keep_running)
	{
		/* Run epoll until it or one of its handlers fails */
		if (EPOLL_LOOP_Run(epoll_fd, 30000, &state) < 0)
		{
			/* Handler failed...bail */
			break;
		}
	}
	DEBUG_PRINT("Exit main loop :-(\n");

	/* Stop threads */
	stop_thread(&state, false);
	stop_thread(&state, true);

	/* Close files */
	close(epoll_fd);
	close(state.read_thread_event_fd);
	close(state.write_thread_event_fd);
	close(state.sock_control);
	close(state.sock_data);

	/* Goodbye */
	printf("Bye!\n");

	return 0;
}

/* Private functions */
static int handle_control(state_t *state)
{
	socklen_t len;
	struct sockaddr_in addr;
	cmd_ip_t cmd;
	int ret;

	/* Read datagram from socket */
    len = sizeof(addr);
    ret = recvfrom(state->sock_control, &cmd, sizeof(cmd), 0, (struct sockaddr*)&addr, &len);
	if (ret < sizeof(cmd_ip_header_t))
	{
		perror("Failed to read cmd from control socket");
		return -1;
	}

	/* Check magic */
	if (SDR_IP_GADGET_MAGIC != cmd.hdr.magic)
	{
		perror("Bad command magic");
		return -1;
	}

	/* Print event summary */
	printf("Handle control socket command: %"PRIu32"\n", cmd.hdr.cmd);

	/* Act on command */
	switch (cmd.hdr.cmd)
	{
		case SDR_IP_GADGET_COMMAND_START_TX:
		{
			/* Check request size */
			if (ret != sizeof(cmd_ip_tx_start_req_t))
			{
				printf("Bad TX start request, incorrect data size\n");
				break;
			}

			/* Ensure thread stopped */
			stop_thread(state, true);

			/* Prepare args */
			DEBUG_PRINT("Start TX with chans: %08X, timestamp: %u, buffsize: %zu\n",
						cmd.start_tx.enabled_channels,
						cmd.start_tx.timestamping_enabled,
						cmd.start_tx.buffer_size);
			state->write_args.iio_channels = cmd.start_tx.enabled_channels;
			state->write_args.timestamping_enabled = cmd.start_tx.timestamping_enabled;
			state->write_args.iio_buffer_size = cmd.start_tx.buffer_size;

			/* Start thread */
			start_thread(state, true);
			break;
		}
		case SDR_IP_GADGET_COMMAND_START_RX:
		{
			/* Check request size */
			if (ret != sizeof(cmd_ip_rx_start_req_t))
			{
				printf("Bad RX start request, incorrect data size\n");
				break;
			}

			/* Ensure thread stopped */
			stop_thread(state, false);

			/* Prepare args */
			char addr_str[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET, &(addr.sin_addr), addr_str, INET_ADDRSTRLEN) == NULL) {
				perror("Error converting address to string");
				addr_str[0] = '\0';
			}
			DEBUG_PRINT("Start RX with chans: %08X, timestamp: %u, buffsize: %zu, pktsize: %zu, dest: %s:%u\n",
						cmd.start_rx.enabled_channels,
						cmd.start_rx.timestamping_enabled,
						cmd.start_rx.buffer_size,
						cmd.start_rx.packet_size,
						addr_str, ntohs(cmd.start_rx.data_port));
			state->read_args.addr.sin_family = AF_INET;
			state->read_args.addr.sin_addr = addr.sin_addr;
			state->read_args.addr.sin_port = htons(cmd.start_rx.data_port);
			state->read_args.iio_channels = cmd.start_rx.enabled_channels;
			state->read_args.timestamping_enabled = cmd.start_rx.timestamping_enabled;
			state->read_args.iio_buffer_size = cmd.start_rx.buffer_size;
			state->read_args.udp_packet_size = cmd.start_rx.packet_size;

			/* Start thread */
			start_thread(state, false);
			break;
		}
		case SDR_IP_GADGET_COMMAND_STOP_TX:
		case SDR_IP_GADGET_COMMAND_STOP_RX:
		{
			/* Decide on TX vs RX thread */
			bool tx = (SDR_IP_GADGET_COMMAND_STOP_TX == cmd.hdr.cmd);

			DEBUG_PRINT("Stop %s\n", tx ? "TX" : "RX");

			/* Stop thread */
			stop_thread(state, tx);
			break;
		}
		default:
		{
			/* Ignore unknown requests */
			break;
		}
	}

	return 0;
}

static bool start_thread(state_t *state, bool tx)
{
	/* Mask all signals (such that threads will by default not handle them) */
	sigset_t new_mask, old_mask;
	sigfillset(&new_mask);
	if (sigprocmask(SIG_SETMASK, &new_mask, &old_mask) < 0)
	{
		perror("Failed to mask signals");
		return false;
	}

	/* Create appropriate thread */
	if (tx && !state->write_started)
	{
		/* Start thread */
		state->write_started = (0 == pthread_create(&state->thread_write, NULL, &THREAD_WRITE_Entrypoint, &state->write_args));
		if (!state->write_started)
		{
			perror("Failed to start write thread");
			return false;
		}
	}
	else if (!tx && !state->read_started)
	{
		/* Start thread */
		state->read_started = (0 == pthread_create(&state->thread_read, NULL, &THREAD_READ_Entrypoint, &state->read_args));
		if (!state->read_started)
		{
			perror("Failed to start read thread");
			return false;
		}
	}

	/* Return signal mask to old value, such that all signals will be handled by main thread */
	if (sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0)
	{
		perror("Failed to unmask signals");
		return false;
	}

	return true;
}

static bool stop_thread(state_t *state, bool tx)
{
	if (tx && state->write_started)
	{
		/* Write eventfd to signal thread to stop */
		uint64_t eventfd_val = 0x1;
		if (write(state->write_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to write to write thread eventfd");
			return false;
		}

		/* Join with thread */
		pthread_join(state->thread_write, NULL);

		/* Read eventfd now thread has stopped to reset it */
		if (read(state->write_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to read from write thread eventfd");
			return false;
		}

		/* Clear running flag */
		state->write_started = false;
	}
	else if (!tx && state->read_started)
	{
		/* Write eventfd to signal thread to stop */
		uint64_t eventfd_val = 0x1;
		if (write(state->read_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to write to read thread eventfd");
			return false;
		}

		/* Join with thread */
		pthread_join(state->thread_read, NULL);

		/* Read eventfd now thread has stopped to reset it */
		if (read(state->read_thread_event_fd, &eventfd_val, sizeof(eventfd_val)) < 0)
		{
			perror("Failed to read from read thread eventfd");
			return false;
		}

		/* Clear running flag */
		state->read_started = false;
	}

	return true;
}

static void signal_handler(int signum)
{
	(void)signum;

	/* Clear running flag */
	keep_running = 0;
}

static void print_usage(const char *program_name, FILE *dest)
{
	fprintf(dest, "Usage: %s [OPTIONS]\n", program_name);
	fprintf(dest, "OPTIONS:\n");
	fprintf(dest, "  -h, --help\tDisplay this help message\n");
	fprintf(dest, "  -d, --debug\tEnable debug output\n");
	fprintf(dest, "  -v, --version\tDisplay the version of the program\n");
}
