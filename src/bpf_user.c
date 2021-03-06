#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/xsk.h>

#include "ishoal.h"
#include "list.h"
#include "bpf_kern.skel.h"

static struct bpf_kern *obj;

macaddr_t switch_mac;
ipaddr_t switch_ip;

ipaddr_t fake_gateway_ip;

static int num_xsk;

static void close_obj(void)
{
	bpf_kern__destroy(obj);
}

static void detach_obj(void)
{
	bpf_set_link_xdp_fd(ifindex, -1, 0);
}

static void clear_map(void)
{
	for (int i = 0; i < 64; i++) {
		int key = i;
		bpf_map_delete_elem(bpf_map__fd(obj->maps.xsks_map), &key);
	}
}

void bpf_set_remote_addr(ipaddr_t local_ip, struct remote_addr *remote_addr)
{
	if (bpf_map_update_elem(bpf_map__fd(obj->maps.remote_addrs), &local_ip,
				remote_addr, BPF_ANY))
		perror_exit("bpf_map_update_elem");
}

void bpf_delete_remote_addr(ipaddr_t local_ip)
{
	bpf_map_delete_elem(bpf_map__fd(obj->maps.remote_addrs), &local_ip);
}

struct on_switch_chg_handler {
	struct list_head list;
	void (*fn)(void);
};

static pthread_mutex_t on_switch_chg_handlers_lock;
static LIST_HEAD(on_switch_chg_handlers);

__attribute__((constructor))
static void on_switch_chg_init(void)
{
	pthread_mutex_init(&on_switch_chg_handlers_lock, NULL);
}

void on_switch_change(void (*fn)(void)) {
	struct on_switch_chg_handler *handler = calloc(1, sizeof(*handler));
	if (!handler)
		perror_exit("calloc");

	handler->fn = fn;
	pthread_mutex_lock(&on_switch_chg_handlers_lock);
	list_add(&handler->list, &on_switch_chg_handlers);
	pthread_mutex_unlock(&on_switch_chg_handlers_lock);
}

static void __on_switch_change(void)
{
	struct on_switch_chg_handler *handler;
	pthread_mutex_lock(&on_switch_chg_handlers_lock);
	list_for_each_entry(handler, &on_switch_chg_handlers, list) {
		handler->fn();
	}
	pthread_mutex_unlock(&on_switch_chg_handlers_lock);
}

void bpf_set_switch_ip(ipaddr_t addr)
{
	if (switch_ip == addr)
		return;

	switch_ip = addr;
	obj->bss->switch_ip = addr;
	__on_switch_change();
}

void bpf_set_switch_mac(macaddr_t addr)
{
	if (!memcmp(switch_mac, addr, sizeof(macaddr_t)))
		return;

	memcpy(switch_mac, addr, sizeof(macaddr_t));
	memcpy(obj->bss->switch_mac, addr, sizeof(macaddr_t));
	__on_switch_change();
}

static void update_subnet_mask(void)
{
	if (fake_gateway_ip)
		obj->bss->subnet_mask = htonl(0xFFFFFF00);
	else
		obj->bss->subnet_mask = real_subnet_mask;
}

void bpf_set_fake_gateway_ip(ipaddr_t addr)
{
	if (fake_gateway_ip == addr)
		return;

	fake_gateway_ip = addr;
	obj->bss->fake_gateway_ip = addr;

	update_subnet_mask();
}

static void on_xsk_pkt(void *ptr, size_t length)
{
	tui_on_xsk_pkt();

	if (obj->bss->switch_ip != switch_ip ||
	    memcmp(obj->bss->switch_mac, switch_mac, sizeof(macaddr_t))) {
		switch_ip = obj->bss->switch_ip;
		memcpy(switch_mac, obj->bss->switch_mac, sizeof(macaddr_t));

		__on_switch_change();
	}
	if (length <= sizeof(struct ethhdr))
		return;

	char *buf = ptr;
	buf += sizeof(struct ethhdr);
	length -= sizeof(struct ethhdr);

	broadcast_all_remotes(buf, length);
}

void bpf_load_thread(void *arg)
{
	obj = bpf_kern__open_and_load();
	if (!obj)
		exit(1);

	atexit(close_obj);

	obj->bss->switch_ip = switch_ip;
	memcpy(obj->bss->switch_mac, switch_mac, sizeof(macaddr_t));

	obj->bss->public_host_ip = public_host_ip;
	memcpy(obj->bss->host_mac, host_mac, sizeof(macaddr_t));
	memcpy(obj->bss->gateway_mac, gateway_mac, sizeof(macaddr_t));

	obj->bss->fake_gateway_ip = fake_gateway_ip;
	update_subnet_mask();

	obj->bss->vpn_port = vpn_port;

	if (bpf_set_link_xdp_fd(ifindex, bpf_program__fd(obj->progs.xdp_prog), 0) < 0)
		perror_exit("bpf_set_link_xdp_fd");
	atexit(detach_obj);

	for (int i = 0; i < MAX_XSKS; i++) {
		struct xsk_socket *xsk = xsk_configure_socket(iface, i, on_xsk_pkt);
		if (!xsk) {
			if (i)
				break;
			else
				perror_exit("xsk_configure_socket");
		}

		int fd = xsk_socket__fd(xsk);
		int key = i;
		if (bpf_map_update_elem(bpf_map__fd(obj->maps.xsks_map), &key, &fd, 0))
			perror_exit("bpf_map_update_elem");

		num_xsk++;
	}

	atexit(clear_map);
}
