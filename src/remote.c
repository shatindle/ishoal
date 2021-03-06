#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "ishoal.h"
#include "list.h"

uint16_t vpn_port;
uint16_t public_vpn_port;

static int endpoint_fd;

struct remote_switch {
	struct list_head list;
	ipaddr_t local;
	struct remote_addr remote;
};

static pthread_mutex_t remotes_lock;
static LIST_HEAD(remotes);

int remotes_fd;
static FILE *remotes_log;

__attribute__((constructor))
static void remote_init(void)
{
	pthread_mutex_init(&remotes_lock, NULL);
}

void start_endpoint(void)
{
	remotes_fd = open(".", O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR);
	if (remotes_fd < 0)
		perror_exit("open(O_TMPFILE)");

	remotes_log = fdopen(remotes_fd, "a");
	if (!remotes_log)
		perror_exit("fdopen");

	setvbuf(remotes_log, NULL, _IONBF, 0);

	endpoint_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (endpoint_fd < 0)
		perror_exit("socket(AF_INET, SOCK_DGRAM, 0)");

	struct ifreq ifr;

	ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name, iface, IFNAMSIZ-1);

	if (setsockopt(endpoint_fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0)
		perror_exit("setsockopt");

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = 0,
		.sin_addr = { public_host_ip },
	};
	socklen_t addrlen = sizeof(addr);
	if (bind(endpoint_fd, (struct sockaddr *)&addr, addrlen) < 0)
		perror_exit("bind");

	if (getsockname(endpoint_fd, (struct sockaddr *)&addr, &addrlen) == -1)
		perror_exit("bind");

	vpn_port = ntohs(addr.sin_port);
	assert(vpn_port);

	ipaddr_t public_ip;
	do_stun(endpoint_fd, &public_ip, &public_vpn_port);

	if (public_vpn_port == vpn_port)
		fprintf(remotes_log, "Endpoint UDP port: %d\n", vpn_port);
	else
		fprintf(remotes_log, "Endpoint UDP port: %d, STUN resolved to: %d\n", vpn_port, public_vpn_port);
}

void set_remote_addr(ipaddr_t local_ip, ipaddr_t remote_ip, uint16_t remote_port)
{
	if (local_ip == switch_ip)
		return;

	struct remote_switch *remote;
	pthread_mutex_lock(&remotes_lock);
	list_for_each_entry(remote, &remotes, list) {
		if (remote->local == local_ip) {
			remote->remote.ip = remote_ip;
			remote->remote.port = remote_port;
			goto next;
		}
	}

next:
	remote = calloc(1, sizeof(*remote));
	if (!remote)
		perror_exit("calloc");
	remote->local = local_ip;
	remote->remote.ip = remote_ip;
	remote->remote.port = remote_port;

	list_add(&remote->list, &remotes);
	pthread_mutex_unlock(&remotes_lock);

	fprintf(remotes_log, "+ Remote IP %s\n", ip_str(local_ip));

	bpf_set_remote_addr(local_ip, &remote->remote);
}

void delete_remote_addr(ipaddr_t local_ip)
{
	if (local_ip == switch_ip)
		return;

	struct remote_switch *remote;
	pthread_mutex_lock(&remotes_lock);
	list_for_each_entry(remote, &remotes, list) {
		if (remote->local == local_ip) {
			list_del(&remote->list);
			goto found;
		}
	}
	pthread_mutex_unlock(&remotes_lock);
	return;

found:
	pthread_mutex_unlock(&remotes_lock);

	fprintf(remotes_log, "- Remote IP %s\n", ip_str(local_ip));

	bpf_delete_remote_addr(local_ip);
}

void broadcast_all_remotes(void *buf, size_t len)
{
	struct remote_switch *remote;
	pthread_mutex_lock(&remotes_lock);
	list_for_each_entry(remote, &remotes, list) {
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(remote->remote.port),
			.sin_addr = { remote->remote.ip },
		};
		sendto(endpoint_fd, buf, len, 0,
		       (struct sockaddr *)&addr, sizeof(addr));
	}
	pthread_mutex_unlock(&remotes_lock);
}
