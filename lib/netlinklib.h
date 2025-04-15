#ifndef NETLINKLIB_SENTRY_H
#define NETLINKLIB_SENTRY_H

#include <linux/netlink.h>

#define NLMSG_TAIL(nmsg) \
	((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

enum { page_size = 4096 };

int netlink_request(int sock, struct nlmsghdr *nlh);
int addattr_l(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen);
struct rtattr *addattr_nest(struct nlmsghdr *n, int maxlen, int type);
int netlink_request(int sock, struct nlmsghdr *nlh);
int add_veth_pair(const char *ifname, const char *peername);
int create_socket(void);
int create_veth_pair(int sock, const char *ifname, const char *peername);
int if_to_netns(int sock, const char *ifname, int netns);
int if_up(int sock, const char *ifname);
int addr_add(int sock, const char *ifname, const char *ip_addr, int ip_prefix);
int if_del(int sock, const char *ifname);

#endif
