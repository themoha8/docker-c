#define _GNU_SOURCE				/* clone */
#include <sched.h>
#include <stdio.h>				/* printf */
#include <errno.h>				/* EEXIST */
#include <sys/types.h>			/* open */
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mount.h>			/* mount */
#include <unistd.h>				/* close */
#include <sys/syscall.h>
#include <stdlib.h>				/* exit */
#include <string.h>				/* strlen */
#include "lib/netlinklib.h"
#include <signal.h>				/* SIGCHILD */
#include <sys/wait.h>			/* wait */
#include <grp.h>				/* setgroups */

enum { stack_size = 1024 * 8, max_path = 32 };

static char child_stack[stack_size];

struct veth_netns {
	const char *ifname;
	const char *peername;
	const char *ip_addr_if;
	int ip_if_prefix;
	const char *ip_addr_peer;
	int ip_peer_prefix;
	int child_pid;
};

struct child_args {
	char **argv;
	int pipe_fd[2];
};

static char *def_prog[] = { "/bin/sh", NULL };

/*static char *def_prog[] = { "nc", "nc", "-l", "172.16.0.3", "7070", NULL };*/

static int update_map(const char *mapping, const char *map_file)
{
	int fd;
	int map_len;

	map_len = strlen(mapping);

	fd = open(map_file, O_RDWR);
	if (fd < 0) {
		perror("open map_file");
		return 1;
	}

	if (write(fd, mapping, map_len) != map_len) {
		perror("write in map_file");
		return 2;
	}

	return 0;
}

/* Linux 3.19 made a change in the handling of setgroups(2) and
 * the 'gid_map' file to address a security issue.  The issue
 * allowed *unprivileged* users to employ user namespaces in
 * order to drop groups.  The upshot of the 3.19 changes is that
 * in order to update the 'gid_maps' file, use of the setgroups()
 * system call in this user namespace must first be disabled by
 * writing "deny" to one of the /proc/PID/setgroups files for
 * this namespace.  That is the purpose of the following function.
 */

static int proc_setgroups_write(int child_pid, const char *str)
{
	char setgroups_path[max_path];
	int fd;

	snprintf(setgroups_path, max_path, "/proc/%d/setgroups", child_pid);

	fd = open(setgroups_path, O_RDWR);
	if (fd < 0) {

		/* We may be on a system that doesn't support
		 * /proc/PID/setgroups. In that case, the file won't exist,
		 * and the system won't impose the restrictions that Linux 3.19
		 * added. That's fine: we don't need to do anything in order
		 * to permit 'gid_map' to be updated.
		 * However, if the error from open() was something other than
		 * the ENOENT error that is expected for that case,  let the
		 * user know.
		 */

		if (errno != ENOENT) {
			perror("open setgroups");
			return 1;
		}
	}

	if (write(fd, str, strlen(str)) < 0) {
		perror("write in setgroups");
		return 2;
	}

	return 0;
}

static int prepare_veth_netns(const struct veth_netns *vethinfo)
{
	int sock, fd;
	char buf[max_path];

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
		snprintf(buf, max_path, "/proc/%d/ns/net", vethinfo->child_pid);
		fd = open(buf, O_RDONLY);
		if (fd < 0) {
			perror("open ns/net");
			return 7;
		}
		if_to_netns(sock, vethinfo->peername, fd);
		close(fd);
	}

	close(sock);
	return 0;
}

static int prepare_mntns(const char *rootfs)
{
	const char *put_old = ".put_old";

	/* mount --bind rootfs rootfs
	 * Make the directory act as a mount point and then
	 * turned it into a rooted filesystem.
	 */
	if (mount(rootfs, rootfs, "ext4", MS_BIND, NULL)) {
		perror("mount rootfs");
		return 1;
	}

	if (chdir(rootfs)) {
		perror("chdir rootfs");
		return 2;
	}

	if (mkdir(put_old, 0777) && errno != EEXIST) {
		perror("mkdir put_old");
		return 3;
	}

	/* Move the root mount to the directory put_old
	 * and makes the current direcctory the new root mount
	 */
	if (syscall(SYS_pivot_root, ".", put_old) == -1) {
		perror("pivot_root");
		return 4;
	}

	if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
		perror("mount proc");
		return 5;
	}

	/* MNT_DETACH - make the mount anavailable for new accesses,
	 * immediately disconnect the filesystem and all filesystems
	 * mounted below it from each other and from the mount table,
	 * and actually perform the unmount when the mount ceases to be busy.
	 */
	if (umount2(put_old, MNT_DETACH) == -1) {
		perror("unmount2 put_old");
		return 6;
	}

	return 0;
}

static int prepare_child()
{
	int sock;
	sock = create_socket();
	if (sock < 0)
		return 1;

	if (if_up(sock, "lo")) {
		fprintf(stderr, "if_up %s is failed\n", "lo");
		return 2;
	}
	close(sock);

	if (sethostname("container", 9) < 0) {
		perror("sethostname");
		return 3;
	}

	if (prepare_mntns("alpine"))
		return 5;

	/* 405 - guest in alpine image */
	if (setgid(100) < 0) {
		perror("setgid in child");
		return 6;
	}

	if (setuid(405) < 0) {
		perror("setuid in child");
		return 7;
	}

	return 0;
}

static int child_fn(void *arg)
{
	struct child_args *args = arg;
	int ch;

	close(args->pipe_fd[1]);
	/* Wait until the parent makes the setting */
	if (read(args->pipe_fd[0], &ch, 1) != 0) {
		exit(1);
	}
	close(args->pipe_fd[0]);

	if (prepare_child()) {
		fprintf(stderr, "prepare_child is failed\n");
		exit(2);
	}

/*	sleep(600);*/
	printf("About to exec %s\n", args->argv[0]);
	/* Execute a shell command */
	execvp(args->argv[0], args->argv);
	perror(args->argv[0]);
	fflush(stderr);
	_exit(3);
}

static int restore(const char *ifname, const char *rootfs)
{
	int sock;

	sock = create_socket();
	if (sock < 0) {
		return 1;
	}

	if (if_del(sock, ifname)) {
		return 2;
	}

	close(sock);
	return 0;
}

int main(void)
{
	int child_pid;
	struct child_args ch_args;
	struct veth_netns vn;
	char buf[max_path];

	if (pipe(ch_args.pipe_fd) == -1) {
		perror("pipe");
		return 1;
	}

	ch_args.argv = def_prog;

	if (setgroups(0, NULL) < 0) {
		perror("sentgroups");
		return 2;
	}

	/* SIGCHLD means send a signal the parent after the child has finished
	 * CLONE_NEWNS - create a new namespace for mount as well as get
	 * copy of all mount points.
	 */
	child_pid = clone(child_fn, child_stack + stack_size - 1, CLONE_NEWNS |
					  CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWPID |
					  CLONE_NEWUSER | CLONE_NEWCGROUP | SIGCHLD, &ch_args);
	if (child_pid == -1) {
		perror("clone");
		return 3;
	}

	close(ch_args.pipe_fd[0]);

	vn.ifname = "veth0";
	vn.peername = "ceth0";
	vn.ip_addr_if = "172.16.0.2";
	vn.ip_if_prefix = 24;
	vn.ip_addr_peer = "172.16.0.3";
	vn.ip_peer_prefix = 24;
	vn.child_pid = child_pid;

	snprintf(buf, max_path, "/proc/%d/uid_map", child_pid);
	if (update_map("0 500 65534", buf)) {
		write(ch_args.pipe_fd[1], "-1", 1);
		wait(NULL);
		return 4;
	}

	if (proc_setgroups_write(child_pid, "deny")) {
		write(ch_args.pipe_fd[1], "-1", 1);
		wait(NULL);
		return 5;
	}

	snprintf(buf, max_path, "/proc/%d/gid_map", child_pid);
	if (update_map("0 500 65534", buf)) {
		write(ch_args.pipe_fd[1], "-1", 1);
		wait(NULL);
		return 6;
	}

	if (prepare_veth_netns(&vn)) {
		write(ch_args.pipe_fd[1], "-1", 1);
		wait(NULL);
		return 7;
	}

	close(ch_args.pipe_fd[1]);

	wait(NULL);

	if (restore(vn.ifname, "alpine"))
		return 8;

	return 0;
}
