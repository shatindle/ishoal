#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <unistd.h>

#include "ishoal.h"

int exitcode;

char *progname;
char *iface;
int ifindex;

volatile sig_atomic_t stop_sig_received;
int stop_sig_eventfd;

static void sig_handler(int sig_num)
{
	stop_sig_received = 1;

	uint64_t event_data = 1;
	if (write(stop_sig_eventfd, &event_data, sizeof(event_data)) !=
	    sizeof(event_data))
		perror_exit("write(eventfd)");
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s [interface]\n", argv[0]);
		exit(1);
	}

	progname = argv[0];
	iface = argv[1];
	ifindex = if_nametoindex(iface);
	if (!ifindex) {
		perror(iface);
		exit(1);
	};

	ifinfo_init();
	start_endpoint();
	load_conf();

	struct rlimit unlimited = { RLIM_INFINITY, RLIM_INFINITY };
	if (setrlimit(RLIMIT_MEMLOCK, &unlimited))
		perror_exit("setrlimit(RLIMIT_MEMLOCK)");

	stop_sig_eventfd = eventfd(0, EFD_CLOEXEC);
	if (stop_sig_eventfd < 0)
		perror_exit("eventfd");

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	thread_start(bpf_load_thread, NULL, "bpf");
	thread_start(python_thread, NULL, "python");
	thread_start(tui_thread, NULL, "tui");

	while (!thread_should_stop(current) && !stop_sig_received) {
		struct pollfd fds[2] = {
			{thread_stop_eventfd(current), POLLIN},
			{stop_sig_eventfd, POLLIN},
		};
		poll(fds, 2, -1);
	}

	thread_all_stop();
	thread_join_rest();

	return exitcode;
}
