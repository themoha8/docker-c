#include <asm/types.h>			/* netlink protocol */
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>			/* socket */
#include <string.h>				/* strlen */
#include <stdio.h>				/* printf */
#include <stdlib.h>				/* exit */
#include <unistd.h>				/* close */
#include <linux/veth.h>			/* VETH_INFO_PEER */

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

enum { max_len = 1024, page_size = 4096 };

static int netlink_request(int sock, struct nlmsghdr *nlh)
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

	close(sock);

	if (nlh->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *err = (struct nlmsgerr *) NLMSG_DATA(nlh);
		if (err->error < 0) {
			fprintf(stderr, "Failed to create veth: %s\n", strerror(-err->error));
			return 3;
		}
	}

	return 0;
}

static int add_veth_pair(const char *ifname, const char *peername)
{
	int sock, rtalen;
	char msg[page_size];
	struct rtattr *attr, *nest1, *nest2, *nest3;
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi, *peer_ifi;

	memset(&msg, 0, page_size);

	nlh = (struct nlmsghdr *) msg;
	/* NLMSG_DATA(nlh)  ((void *)(((char *)nlh) + NLMSG_HDRLEN)) */
	ifi = (struct ifinfomsg *) NLMSG_DATA(nlh);

	/* NETLINK_ROUTE used to modify ip addresses, link parameters. */
	sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (sock < 0) {
		perror("Cannot open netlink socket");
		return 1;
	}

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

	/* Add attributes */

	/* Add the name of the main interface. ATTR - 3 (IFLA_IFNAME)
	 * NLMSG_ALIGNTO    4U
	 * NLMSG_ALIGN(len) (((len)+NLMSG_ALIGNTO-1) & ~(NLMSG_ALIGNTO-1))
	 * NLMSG_TAIL(nmsg) \
	 * ((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))
	 * RTA_ALIGNTO  4U
	 * RTA_ALIGN(len) (((len)+RTA_ALIGNTO-1) & ~(RTA_ALIGNTO-1))
	 * RTA_LENGTH(len)  (RTA_ALIGN(sizeof(struct rtattr)) + (len))
	 * RTA_DATA(rta) ((void*)(((char*)(rta)) + RTA_LENGTH(0)))
	 */
	attr = NLMSG_TAIL(nlh);
	attr->rta_type = IFLA_IFNAME;
	rtalen = strlen(ifname) + 1;
	attr->rta_len = RTA_LENGTH(rtalen);
	memcpy(RTA_DATA(attr), ifname, rtalen);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(attr->rta_len);

	/* Add interface type. ATTR - 18 (IFLA_LINKINFO) */
	nest1 = NLMSG_TAIL(nlh);
	attr = NLMSG_TAIL(nlh);
	attr->rta_type = IFLA_LINKINFO;
	attr->rta_len = RTA_LENGTH(0);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(attr->rta_len);

	/* Add the first nested attribute for IFLA_LINKINFO. */
	attr = NLMSG_TAIL(nlh);
	attr->rta_type = IFLA_INFO_KIND;
	attr->rta_len = RTA_LENGTH(5);	/* veth */
	memcpy(RTA_DATA(attr), "veth", 5);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(attr->rta_len);

	/* Add the first nested attribute for IFLA_LINKINFO. */
	nest2 = NLMSG_TAIL(nlh);
	attr = NLMSG_TAIL(nlh);
	attr->rta_type = IFLA_INFO_DATA;
	attr->rta_len = RTA_LENGTH(0);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(attr->rta_len);

	/* Add the first nested attribute for IFLA_INFO_DATA. */
	nest3 = NLMSG_TAIL(nlh);
	attr = NLMSG_TAIL(nlh);
	attr->rta_type = VETH_INFO_PEER;
	attr->rta_len = RTA_LENGTH(0);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(attr->rta_len);

	peer_ifi = (struct ifinfomsg *) NLMSG_TAIL(nlh);
	peer_ifi->ifi_family = AF_UNSPEC;
	peer_ifi->ifi_change = 0xffffffff;
	nlh->nlmsg_len += sizeof(struct ifinfomsg);

	/* Add the second nested attrubute. Add the name of the peer interface. */
	attr = NLMSG_TAIL(nlh);
	attr->rta_type = IFLA_IFNAME;
	rtalen = strlen(peername) + 1;
	attr->rta_len = RTA_LENGTH(rtalen);
	memcpy(RTA_DATA(attr), peername, rtalen);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(attr->rta_len);

	nest3->rta_len = (void *) NLMSG_TAIL(nlh) - (void *) nest3;
	nest2->rta_len = (void *) NLMSG_TAIL(nlh) - (void *) nest2;
	nest1->rta_len = (void *) NLMSG_TAIL(nlh) - (void *) nest1;

	/* Send request. */
	if (netlink_request(sock, nlh))
		return 2;

	close(sock);
	return 0;
}

int main(void)
{
	if (add_veth_pair("veth0", "ceth0")) {
		fprintf(stderr, "add_veth_pair is failed\n");
		return 1;
	}

	printf("Interface veth is created\n");
	return 0;

}

/*
nlmsghdr (RTM_NEWLINK)  
│  
├── IFLA_IFNAME          (name of the first interface)  
├── IFLA_LINKINFO        (info about interface type)  
│   │  
│   ├── IFLA_INFO_KIND   (interface type, "veth")  
│   └── IFLA_INFO_DATA   (interface type specific data)  
│       │  
│       └── VETH_INFO_PEER  (description of the secons inteface in the pair)  
│           │  
│           ├── IFLA_IFNAME (name of the second interface)  
│           ├── IFLA_MTU    (MTU)  
│           └── IFLA_NET_NS_PID/FD (netns)  
│  
└── (additions attributes for the first interface)  
*/
