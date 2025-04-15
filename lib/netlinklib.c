#include <asm/types.h>			/* netlink protocol */
#include <linux/rtnetlink.h>
#include <sys/socket.h>			/* socket */
#include <linux/veth.h>			/* VETH_INFO_PEER */
#include <stdio.h>				/* print */
#include <string.h>				/* strlen */
#include <unistd.h>				/* close */
#include <net/if.h>				/* if_nametoindex */
#include <linux/if.h>			/* IFF_UP */
#include <arpa/inet.h>			/* inet_proton */
#include "netlinklib.h"

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

int create_veth_pair(int sock, const char *ifname, const char *peername)
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

int if_del(int sock, const char *ifname)
{
	int ifi_index;
	char msg[page_size];
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi;

	memset(&msg, 0, page_size);

	nlh = (struct nlmsghdr *) msg;
	ifi = (struct ifinfomsg *) NLMSG_DATA(nlh);

	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_DELLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST |	/* Request. */
		NLM_F_ACK;				/* Request for an ack on success. */

	ifi->ifi_family = AF_UNSPEC;
	ifi_index = if_nametoindex(ifname);
	if (!ifi_index) {
		perror("if_nametoindex");
		return 1;
	}
	ifi->ifi_index = ifi_index;
	ifi->ifi_change = 0xffffffff;

	if (netlink_request(sock, nlh)) {
		fprintf(stderr, "addr_del is failed\n");
		return 2;
	}

	return 0;
}
