#include <stdio.h>				/* printf */
#include <string.h>				/* strlen */
#include <unistd.h>				/* close */
#include <sys/types.h>			/* open */
#include <sys/stat.h>
#include <fcntl.h>
#include <asm/types.h>			/* netlink protocol */
#include <linux/rtnetlink.h>
#include <sys/socket.h>			/* socket */
#include <linux/veth.h>			/* VETH_INFO_PEER */
#include <net/if.h>				/* if_nametoindex */
#include <linux/if.h>			/* IFF_UP */
#include <arpa/inet.h>			/* inet_proton */

int snprintf(char *str, size_t size, const char *format, ...);

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

enum { page_size = 4096 };

struct veth_netns {
	const char *ifname;
	const char *peername;
	const char *ip_addr_if;
	int ip_if_prefix;
	const char *ip_addr_peer;
	int ip_peer_prefix;
	int child_pid;
};

int addattr_l(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen)
{
	int len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen) {
		fprintf(stderr, "addattr_l ERROR: message exceeded bound of %d\n", maxlen);
		return -1;
	}
	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = len;
	if (alen)
		memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
	return 0;
}

struct rtattr *addattr_nest(struct nlmsghdr *n, int maxlen, int type)
{
	struct rtattr *nest = NLMSG_TAIL(n);

	addattr_l(n, maxlen, type, NULL, 0);
	return nest;
}

int addattr_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
	nest->rta_len = (void *) NLMSG_TAIL(n) - (void *) nest;
	return n->nlmsg_len;
}

int netlink_request(int sock, struct nlmsghdr *nlh)
{
	/* The sockaddr_nl structure describes a netlink client in
	 * user space or in the kernel.
	 */
	struct sockaddr_nl sa = {.nl_family = AF_NETLINK };
	struct iovec iov = {.iov_base = nlh,.iov_len = nlh->nlmsg_len };
	struct msghdr msg = {.msg_name = &sa,.msg_namelen = sizeof(sa),
		.msg_iov = &iov,.msg_iovlen = 1
	};

	if (sendmsg(sock, &msg, 0) < 0) {
		perror("sendmsg");
		return 1;
	}

	if (recvmsg(sock, &msg, 0) < 0) {
		perror("recvmsg");
		return 2;
	}

	if (nlh->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *err = (struct nlmsgerr *) NLMSG_DATA(nlh);
		if (err->error < 0) {
			fprintf(stderr, "NLMSG_ERROR: %s\n", strerror(-err->error));
			return 3;
		}
	}

	return 0;
}

int create_socket(void)
{
	int sock;

	/* NETLINK_ROUTE used to modify ip addresses, link parameters. */
	sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (sock < 0) {
		perror("Cannot open netlink socket");
		return -1;
	}

	return sock;
}

static int create_veth_pair(int sock, const char *ifname, const char *peername)
{
	char msg[page_size];
	struct rtattr *linfo, *linfodata, *infopeer;
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi, *peer_ifi;

	memset(&msg, 0, page_size);

	nlh = (struct nlmsghdr *) msg;
	/* NLMSG_DATA(nlh)  ((void *)(((char *)nlh) + NLMSG_HDRLEN)) */
	ifi = (struct ifinfomsg *) NLMSG_DATA(nlh);

	/* NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN) */
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST |	/* Request. */
		NLM_F_CREATE |			/* Create object. */
		NLM_F_EXCL |			/* Do not update object if it exists. */
		NLM_F_ACK;				/* Request for an ack on success. */

	/* Request isn't related to setting up addresses */
	ifi->ifi_family = AF_UNSPEC;
	/* Should be always set to 0xffffffff (from man). */
	ifi->ifi_change = 0xffffffff;

	/* Add attributes. */

	/* Add name of the first interface. */
	addattr_l(nlh, page_size, IFLA_IFNAME, ifname, strlen(ifname) + 1);


	/* Add info about interface type. */
	linfo = addattr_nest(nlh, page_size, IFLA_LINKINFO);
	/* Add interface type. */
	addattr_l(nlh, page_size, IFLA_INFO_KIND, "veth", 5);

	/* Add interface type specific data. */
	linfodata = addattr_nest(nlh, page_size, IFLA_INFO_DATA);

	/* Add description of the secons inteface in the pair. */
	infopeer = addattr_nest(nlh, page_size, VETH_INFO_PEER);
	peer_ifi = (struct ifinfomsg *) NLMSG_TAIL(nlh);
	peer_ifi->ifi_family = AF_UNSPEC;
	peer_ifi->ifi_change = 0xffffffff;
	nlh->nlmsg_len += sizeof(struct ifinfomsg);
	/* Add name of the second interface. */
	addattr_l(nlh, page_size, IFLA_IFNAME, peername, strlen(peername) + 1);

	addattr_nest_end(nlh, infopeer);
	addattr_nest_end(nlh, linfodata);
	addattr_nest_end(nlh, linfo);

	/* Send request. */
	if (netlink_request(sock, nlh)) {
		fprintf(stderr, "create_veth_pair is failed\n");
		return 2;
	}

	return 0;
}

int if_to_netns(int sock, const char *ifname, int netns)
{
	char msg[page_size];
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi;

	memset(&msg, 0, page_size);

	nlh = (struct nlmsghdr *) msg;
	ifi = (struct ifinfomsg *) NLMSG_DATA(nlh);

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST |	/* Request. */
		NLM_F_ACK;				/* Request for an ack on success. */

	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_change = 0xffffffff;

	addattr_l(nlh, page_size, IFLA_NET_NS_FD, &netns, 4);
	addattr_l(nlh, page_size, IFLA_IFNAME, ifname, strlen(ifname) + 1);

	if (netlink_request(sock, nlh)) {
		fprintf(stderr, "if_to_netns is failed\n");
		return 1;
	}

	return 0;
}

int if_up(int sock, const char *ifname)
{
	int ifi_index;
	char msg[page_size];
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi;

	memset(&msg, 0, page_size);

	nlh = (struct nlmsghdr *) msg;
	ifi = (struct ifinfomsg *) NLMSG_DATA(nlh);

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST |	/* Request. */
		NLM_F_ACK;				/* Request for an ack on success. */

	ifi->ifi_family = AF_UNSPEC;
	ifi_index = if_nametoindex(ifname);
	if (!ifi_index) {
		perror("if_nametoindex");
		return 1;
	}
	ifi->ifi_index = if_nametoindex(ifname);
	ifi->ifi_change = 0xffffffff;
	ifi->ifi_flags = IFF_UP;

	if (netlink_request(sock, nlh)) {
		fprintf(stderr, "if_up is failed\n");
		return 2;
	}

	return 0;
}

int addr_add(int sock, const char *ifname, const char *ip_addr, int ip_prefix)
{
	int ifa_index;
	char msg[page_size];
	struct nlmsghdr *nlh;
	struct ifaddrmsg *ifa;
	struct in_addr addr;

	memset(&msg, 0, page_size);

	nlh = (struct nlmsghdr *) msg;
	ifa = (struct ifaddrmsg *) NLMSG_DATA(nlh);

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	nlh->nlmsg_type = RTM_NEWADDR;
	nlh->nlmsg_flags = NLM_F_REQUEST |	/* Request. */
		NLM_F_CREATE |			/* Create object. */
		NLM_F_EXCL |			/* Do not update object if it exists. */
		NLM_F_ACK;				/* Request for an ack on success. */

	ifa->ifa_family = AF_INET;
	ifa->ifa_prefixlen = ip_prefix;
	ifa->ifa_scope = RT_SCOPE_UNIVERSE;
	ifa_index = if_nametoindex(ifname);
	if (!ifa_index) {
		perror("if_nametoindex");
		return 1;
	}
	ifa->ifa_index = ifa_index;

	if (inet_pton(AF_INET, ip_addr, &addr) != 1) {
		perror("inet_pton");
		return 2;
	}
	addattr_l(nlh, page_size, IFA_LOCAL, &addr, 4);

	if (netlink_request(sock, nlh)) {
		fprintf(stderr, "addr_add is failed\n");
		return 3;
	}

	return 0;
}

int prepare_veth_netns(const struct veth_netns *vethinfo)
{
	int sock, fd;
	char buf[32];

	sock = create_socket();
	if (sock < 0) {
		return 1;
	}

	if (create_veth_pair(sock, vethinfo->ifname, vethinfo->peername))
		return 2;

	if (if_up(sock, vethinfo->ifname)) {
		fprintf(stderr, "if_up %s is failed\n", vethinfo->ifname);
		return 3;
	}

	/* Add address for ifname. */
	if (addr_add(sock, vethinfo->ifname, vethinfo->ip_addr_if, vethinfo->ip_if_prefix)) {
		fprintf(stderr, "addr_add %s is failed\n", vethinfo->ifname);
		return 4;
	}

	if (if_up(sock, vethinfo->peername)) {
		fprintf(stderr, "if_up %s is failed\n", vethinfo->peername);
		return 5;
	}

	/* Add address for peername. */
	if (addr_add(sock, vethinfo->peername, vethinfo->ip_addr_peer, vethinfo->ip_peer_prefix)) {
		fprintf(stderr, "addr_add %s is failed\n", vethinfo->peername);
		return 6;
	}

	/* Move peername to netns. */
	if (vethinfo->child_pid) {
		snprintf(buf, 32, "/proc/%d/ns/net", vethinfo->child_pid);
		fd = open(buf, O_RDONLY);
		if (fd < 0) {
			perror("open ns/net");
			return 7;
		}
		if_to_netns(sock, vethinfo->peername, fd);
		close(fd);
	}

	return 0;
}

int main(void)
{
/*	if (add_veth_pair("veth0", "ceth0")) {
		fprintf(stderr, "add_veth_pair is failed\n");
		return 1;
	}

	printf("Interface veth is created\n");	*/

	struct veth_netns vn;

	vn.ifname = "veth0";
	vn.peername = "ceth0";
	vn.ip_addr_if = "172.16.0.2";
	vn.ip_if_prefix = 24;
	vn.ip_addr_peer = "172.16.0.3";
	vn.ip_peer_prefix = 24;
	vn.child_pid = 0;

	if (prepare_veth_netns(&vn))
		return 1;

	return 0;
}
