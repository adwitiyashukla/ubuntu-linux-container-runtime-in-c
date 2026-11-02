#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lcr.h"

struct nlreq {
	struct nlmsghdr nh;
	char buf[4096 - sizeof(struct nlmsghdr)];
};

static struct rtattr *nla_add(struct nlmsghdr *nh, int type, const void *data, int len)
{
	struct rtattr *rta = (struct rtattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));

	rta->rta_type = (unsigned short)type;
	rta->rta_len = (unsigned short)RTA_LENGTH(len);
	if (len)
		memcpy(RTA_DATA(rta), data, (size_t)len);
	nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
	return rta;
}

static struct rtattr *nla_nest(struct nlmsghdr *nh, int type)
{
	return nla_add(nh, type, NULL, 0);
}

static void nla_nest_end(struct nlmsghdr *nh, struct rtattr *start)
{
	start->rta_len = (unsigned short)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len) - (char *)start);
}

static void nl_init(struct nlreq *req, int type, int flags, size_t hdrlen)
{
	memset(req, 0, sizeof(*req));
	req->nh.nlmsg_len = NLMSG_LENGTH(hdrlen);
	req->nh.nlmsg_type = (unsigned short)type;
	req->nh.nlmsg_flags = (unsigned short)(NLM_F_REQUEST | NLM_F_ACK | flags);
}

static int nl_open(void)
{
	struct sockaddr_nl sa;
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);

	if (fd < 0)
		die("open netlink socket");
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		die("bind netlink socket");
	return fd;
}

static int nl_talk(int fd, struct nlreq *req)
{
	static uint32_t seq = 100;
	char buf[8192];

	req->nh.nlmsg_seq = ++seq;
	if (send(fd, req, req->nh.nlmsg_len, 0) < 0)
		return -1;
	for (;;) {
		ssize_t got = recv(fd, buf, sizeof(buf), 0);
		struct nlmsghdr *r;
		unsigned len;

		if (got < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		len = (unsigned)got;
		for (r = (struct nlmsghdr *)buf; NLMSG_OK(r, len); r = NLMSG_NEXT(r, len)) {
			if (r->nlmsg_seq != req->nh.nlmsg_seq)
				continue;
			if (r->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(r);
				if (e->error) {
					errno = -e->error;
					return -1;
				}
				return 0;
			}
			if (r->nlmsg_type == NLMSG_DONE)
				return 0;
		}
	}
}

static int link_create(int fd, const char *name, const char *kind, const char *peer, int peer_ns_fd)
{
	struct nlreq req;
	struct ifinfomsg *ifi;
	struct rtattr *linkinfo;

	nl_init(&req, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL, sizeof(*ifi));
	ifi = NLMSG_DATA(&req.nh);
	ifi->ifi_family = AF_UNSPEC;
	nla_add(&req.nh, IFLA_IFNAME, name, (int)strlen(name) + 1);
	linkinfo = nla_nest(&req.nh, IFLA_LINKINFO);
	nla_add(&req.nh, IFLA_INFO_KIND, kind, (int)strlen(kind));
	if (peer) {
		struct rtattr *data = nla_nest(&req.nh, IFLA_INFO_DATA);
		struct rtattr *p = nla_nest(&req.nh, VETH_INFO_PEER);
		char *tail = (char *)&req.nh + NLMSG_ALIGN(req.nh.nlmsg_len);

		memset(tail, 0, sizeof(struct ifinfomsg));
		req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + sizeof(struct ifinfomsg);
		nla_add(&req.nh, IFLA_IFNAME, peer, (int)strlen(peer) + 1);
		if (peer_ns_fd >= 0)
			nla_add(&req.nh, IFLA_NET_NS_FD, &peer_ns_fd, sizeof(int));
		nla_nest_end(&req.nh, p);
		nla_nest_end(&req.nh, data);
	}
	nla_nest_end(&req.nh, linkinfo);
	return nl_talk(fd, &req);
}

static int link_set(int fd, unsigned index, int up, unsigned master)
{
	struct nlreq req;
	struct ifinfomsg *ifi;

	nl_init(&req, RTM_NEWLINK, 0, sizeof(*ifi));
	ifi = NLMSG_DATA(&req.nh);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = (int)index;
	if (up) {
		ifi->ifi_flags = IFF_UP;
		ifi->ifi_change = IFF_UP;
	}
	if (master)
		nla_add(&req.nh, IFLA_MASTER, &master, sizeof(master));
	return nl_talk(fd, &req);
}

static int addr_add(int fd, unsigned index, const char *ip, int prefix)
{
	struct nlreq req;
	struct ifaddrmsg *ifa;
	struct in_addr a;
	struct in_addr bcast;

	if (inet_pton(AF_INET, ip, &a) != 1) {
		errno = EINVAL;
		return -1;
	}
	nl_init(&req, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, sizeof(*ifa));
	ifa = NLMSG_DATA(&req.nh);
	ifa->ifa_family = AF_INET;
	ifa->ifa_prefixlen = (unsigned char)prefix;
	ifa->ifa_index = index;
	ifa->ifa_scope = RT_SCOPE_UNIVERSE;
	bcast.s_addr = a.s_addr | htonl(0xffffffffu >> prefix);
	nla_add(&req.nh, IFA_LOCAL, &a, sizeof(a));
	nla_add(&req.nh, IFA_ADDRESS, &a, sizeof(a));
	nla_add(&req.nh, IFA_BROADCAST, &bcast, sizeof(bcast));
	return nl_talk(fd, &req);
}

static int route_add_default(int fd, const char *gateway, unsigned index)
{
	struct nlreq req;
	struct rtmsg *rtm;
	struct in_addr gw;

	if (inet_pton(AF_INET, gateway, &gw) != 1) {
		errno = EINVAL;
		return -1;
	}
	nl_init(&req, RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, sizeof(*rtm));
	rtm = NLMSG_DATA(&req.nh);
	rtm->rtm_family = AF_INET;
	rtm->rtm_table = RT_TABLE_MAIN;
	rtm->rtm_protocol = RTPROT_BOOT;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_type = RTN_UNICAST;
	nla_add(&req.nh, RTA_GATEWAY, &gw, sizeof(gw));
	nla_add(&req.nh, RTA_OIF, &index, sizeof(index));
	return nl_talk(fd, &req);
}

static void ensure_bridge(int fd)
{
	unsigned index = if_nametoindex(BRIDGE_NAME);
	char knob[PATH_MAX];

	if (!index) {
		if (link_create(fd, BRIDGE_NAME, "bridge", NULL, -1) < 0 && errno != EEXIST)
			die("create bridge %s", BRIDGE_NAME);
		index = if_nametoindex(BRIDGE_NAME);
		if (!index)
			fail("bridge %s did not appear", BRIDGE_NAME);
	}
	if (addr_add(fd, index, BRIDGE_ADDR, BRIDGE_PREFIX) < 0 && errno != EEXIST)
		die("assign %s to %s", BRIDGE_ADDR, BRIDGE_NAME);
	if (link_set(fd, index, 1, 0) < 0)
		die("bring up %s", BRIDGE_NAME);
	snprintf(knob, sizeof(knob), "/sys/class/net/%s/bridge/nf_call_iptables", BRIDGE_NAME);
	write_file(knob, "0");
	if (write_file("/proc/sys/net/ipv4/ip_forward", "1") < 0)
		die("enable ip_forward");
}

static void ensure_nat(void)
{
	static const char ruleset[] =
		"table ip " BRIDGE_NAME " {}\n"
		"flush table ip " BRIDGE_NAME "\n"
		"table ip " BRIDGE_NAME " {\n"
		"  chain postrouting {\n"
		"    type nat hook postrouting priority srcnat; policy accept;\n"
		"    ip saddr " BRIDGE_SUBNET " ip daddr != " BRIDGE_SUBNET " masquerade\n"
		"    oifname \"" BRIDGE_NAME "\" fib saddr type local masquerade\n"
		"  }\n"
		"  chain forward {\n"
		"    type filter hook forward priority filter; policy accept;\n"
		"    iifname \"" BRIDGE_NAME "\" accept\n"
		"    oifname \"" BRIDGE_NAME "\" ct state related,established accept\n"
		"  }\n"
		"}\n";
	static const char *const nft[] = { "nft", "-f", "-", NULL };
	static const char *const nft_check[] = { "nft", "list", "table", "ip", BRIDGE_NAME, NULL };
	static const char *const list[] = { "iptables", "-w", "-S", "FORWARD", NULL };
	static const char *const add_in[] = { "iptables", "-w", "-I", "FORWARD", "-i", BRIDGE_NAME, "-j", "ACCEPT", NULL };
	static const char *const add_out[] = { "iptables", "-w", "-I", "FORWARD", "-o", BRIDGE_NAME, "-j", "ACCEPT", NULL };
	char rules[65536];

	if (run_cmd_stdin(nft_check, NULL, 1) < 0 && run_cmd_stdin(nft, ruleset, 0) < 0)
		die("install nftables NAT rules for %s", BRIDGE_SUBNET);
	if (access("/usr/sbin/iptables", X_OK) < 0 && access("/sbin/iptables", X_OK) < 0)
		return;
	if (run_cmd_output(list, rules, sizeof(rules)) < 0 || !strstr(rules, "-P FORWARD DROP"))
		return;
	if (!strstr(rules, "-A FORWARD -i " BRIDGE_NAME " -j ACCEPT"))
		run_cmd_stdin(add_in, NULL, 1);
	if (!strstr(rules, "-A FORWARD -o " BRIDGE_NAME " -j ACCEPT"))
		run_cmd_stdin(add_out, NULL, 1);
}

static void publish_ports(const struct config *cfg)
{
	static const char *const nft[] = { "nft", "-f", "-", NULL };
	char ruleset[8192];
	char knob[PATH_MAX];
	size_t off = 0;
	int i;

	off += (size_t)snprintf(ruleset + off, sizeof(ruleset) - off,
		"table ip lcr_%s {\n  chain prerouting {\n    type nat hook prerouting priority dstnat; policy accept;\n",
		cfg->id);
	for (i = 0; i < cfg->nports; i++)
		off += (size_t)snprintf(ruleset + off, sizeof(ruleset) - off,
			"    tcp dport %d dnat to %s:%d\n", cfg->ports[i].host, cfg->ip, cfg->ports[i].ctr);
	off += (size_t)snprintf(ruleset + off, sizeof(ruleset) - off,
		"  }\n  chain output {\n    type nat hook output priority -100; policy accept;\n");
	for (i = 0; i < cfg->nports; i++)
		off += (size_t)snprintf(ruleset + off, sizeof(ruleset) - off,
			"    fib daddr type local tcp dport %d dnat to %s:%d\n", cfg->ports[i].host, cfg->ip, cfg->ports[i].ctr);
	off += (size_t)snprintf(ruleset + off, sizeof(ruleset) - off, "  }\n}\n");
	if (off >= sizeof(ruleset))
		fail("too many published ports");
	snprintf(knob, sizeof(knob), "/proc/sys/net/ipv4/conf/%s/route_localnet", BRIDGE_NAME);
	write_file(knob, "1");
	if (run_cmd_stdin(nft, ruleset, 0) < 0)
		die("install port forwarding rules");
}

void net_alloc_ip(struct config *cfg)
{
	unsigned char used[256];
	DIR *d;
	struct dirent *e;
	int host;

	memset(used, 0, sizeof(used));
	used[0] = used[1] = used[255] = 1;
	d = opendir(state_root());
	if (d) {
		while ((e = readdir(d)) != NULL) {
			struct state st;
			char dir[PATH_MAX];
			unsigned a, b, c, h;

			if (e->d_name[0] == '.')
				continue;
			join_path(dir, sizeof(dir), state_root(), e->d_name);
			if (state_load(dir, &st) < 0 || !st.ip[0] || !state_alive(&st))
				continue;
			if (sscanf(st.ip, "%u.%u.%u.%u", &a, &b, &c, &h) == 4 && h < 256)
				used[h] = 1;
		}
		closedir(d);
	}
	for (host = 2; host < 255; host++) {
		if (!used[host]) {
			snprintf(cfg->ip, sizeof(cfg->ip), "10.77.0.%d", host);
			return;
		}
	}
	fail("no free address left in %s", BRIDGE_SUBNET);
}

void net_host_setup(struct config *cfg, pid_t pid)
{
	char host_if[IFNAMSIZ];
	char nspath[64];
	int fd, nsfd;
	unsigned index, bridge;

	fd = nl_open();
	ensure_bridge(fd);
	ensure_nat();
	snprintf(host_if, sizeof(host_if), "lcr%.10s", cfg->id);
	snprintf(nspath, sizeof(nspath), "/proc/%d/ns/net", pid);
	nsfd = open(nspath, O_RDONLY | O_CLOEXEC);
	if (nsfd < 0)
		die("open %s", nspath);
	if (link_create(fd, host_if, "veth", "eth0", nsfd) < 0)
		die("create veth pair %s", host_if);
	close(nsfd);
	index = if_nametoindex(host_if);
	bridge = if_nametoindex(BRIDGE_NAME);
	if (!index || !bridge)
		die("look up %s", host_if);
	if (link_set(fd, index, 1, bridge) < 0)
		die("attach %s to %s", host_if, BRIDGE_NAME);
	close(fd);
	if (cfg->nports)
		publish_ports(cfg);
}

void net_host_teardown(const struct config *cfg)
{
	static const char *const nft[] = { "nft", "-f", "-", NULL };
	char ruleset[128];

	if (!cfg->nports)
		return;
	snprintf(ruleset, sizeof(ruleset), "delete table ip lcr_%s\n", cfg->id);
	run_cmd_stdin(nft, ruleset, 1);
}

void net_container_setup(const struct config *cfg)
{
	int fd;
	unsigned lo, eth;

	if (cfg->net == NET_HOST)
		return;
	fd = nl_open();
	lo = if_nametoindex("lo");
	eth = if_nametoindex("eth0");
	if (lo && link_set(fd, lo, 1, 0) < 0)
		die("bring up lo");
	if (cfg->net != NET_BRIDGE) {
		close(fd);
		return;
	}
	if (!eth)
		fail("eth0 is missing inside the container");
	if (addr_add(fd, eth, cfg->ip, BRIDGE_PREFIX) < 0)
		die("assign %s to eth0", cfg->ip);
	if (link_set(fd, eth, 1, 0) < 0)
		die("bring up eth0");
	if (route_add_default(fd, BRIDGE_ADDR, eth) < 0)
		die("add default route via %s", BRIDGE_ADDR);
	close(fd);
}
