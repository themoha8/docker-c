#define _GNU_SOURCE
#include <sched.h>				/* clone */
#include <sys/types.h>			/* getpid */
#include <unistd.h>				/* getpid, sleep */
#include <sys/wait.h>			/* wait */
#include <stdio.h>				/* printf */
#include <sys/mount.h>			/* mount */
#include <stdlib.h>				/* exit */

enum { stack_size = 1024 * 8, add_veth = 0, del_veth = 1, pid_buf_size = 16 };

static char child_stack[stack_size];

static char *ip_add_veth[] = { "ip", "link", "add", "veth0", "type", "veth", "peer", "name", "ceth0", NULL };
static char *ip_del_veth[] = { "ip", "link", "del", "veth0", "type", "veth", NULL };
static char *ip_link_veth0_up[] = { "ip", "link", "set", "veth0", "up", NULL };
static char *ip_add_addr[] = { "ip", "addr", "add", "172.12.0.2/24", "dev", "veth0", NULL };
static char *ip_add_veth_newns[] = { "ip", "link", "set", "ceth0", "netns", NULL, NULL };
static char *ip_link_lo_up_newns[] = { "ip", "link", "set", "lo", "up", NULL };
static char *ip_link_ceth0_up_newns[] = { "ip", "link", "set", "ceth0", "up", NULL };
static char *ip_add_addr_newns[] = { "ip", "addr", "add", "172.12.0.3/24", "dev", "ceth0", NULL };

int itoa(char *buf, int size, long n)
{
	long n_digits = 0, rx = 0, i = 0;

	if (!buf)
		return 0;

	if (n == 0) {
		buf[0] = '0';
		return 1;
	}

	/* sign */
	if (n < 0) {
		buf[i] = '-';
		i++;
		n = -n;
	}

	while (n > 0) {
		rx = (10 * rx) + (n % 10);
		n /= 10;
		n_digits++;
	}

	while (n_digits > 0 && size > i) {
		buf[i] = (rx % 10) + '0';
		i++;
		rx /= 10;
		n_digits--;
	}

	return i;
}

int exec_ip(char **args)
{
	pid_t pid;
	int wstatus;

	pid = fork();
	if (pid == -1) {
		perror("fork");
		return 1;
	}

	if (pid == 0) {
		execvp("ip", args);
		perror("ip");
		fflush(stderr);
		_exit(1);
	}

	wait(&wstatus);

	if (WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 0))
		return 0;

	return 2;
}

int init_network_cont()
{
	/* Configure ceth0. */
	if (exec_ip(ip_link_lo_up_newns)) {
		fprintf(stderr, "ip_link_lo_up_newns is failed\n");
		return 1;
	}

	if (exec_ip(ip_link_ceth0_up_newns)) {
		fprintf(stderr, "ip_link_ceth0_up_newns is failed\n");
		return 2;
	}

	if (exec_ip(ip_add_addr_newns)) {
		fprintf(stderr, "ip_add_addr_newns is failed\n");
		return 3;
	}

	return 0;
}

static int child_fn(void *p)
{
	int pid;
	char ch;
	int *fd = (int *) p;

	printf("init: PID in a new namespace: %d (PPID is %d)\n\n", getpid(), getppid());

	close(fd[1]);

	/* Wait until the parent makes the setting */
	if (read(fd[0], &ch, 1) != 0)
		exit(1);

	close(fd[0]);

	if (init_network_cont()) {
		fprintf(stderr, "init_network_cont is failed\n");
		exit(2);
	}

	/* Change the propogation type of an existing mount
	 * MS_PRIVATE - Mount and unmount events do not propagate
	 * into or out of this mount.
	 * MS_REC - change propagate type on all mount point (recursive)
	 */
	if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
		perror("mount");
		exit(3);
	}

	/* Mount procfs for the new PID namespace
	 * MS_NOSUID - ignore SUID/SGID bits on files in the filesystem
	 * MS_NODEV - disallow access to device files
	 * MS_NOEXEC - disallow program execution
	 */
	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME, NULL) == -1) {
		perror("mount");
		exit(4);
	}

	pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(5);
	}

	if (pid == 0) {
		execlp("nc", "nc", "-l", "172.12.0.3", "7070", NULL);
		perror("nc");
		fflush(stderr);
		_exit(6);
	}

	wait(NULL);
	exit(0);
}

int restore_network()
{
	if (exec_ip(ip_del_veth)) {
		fprintf(stderr, "ip_del_veth is failed\n");
		return 1;
	}
	return 0;
}

/* Create tunnel between container and host system. */
int init_network_host(int child_pid)
{
	int n;
	char pid[pid_buf_size];

	/* Create virtual ethernet device (tunnel between network namespaces). */
	if (exec_ip(ip_add_veth)) {
		fprintf(stderr, "ip_add_veth is failed\n");
		return 1;
	}

	/* Move ceth0 in a new network namespace */
	n = itoa(pid, pid_buf_size - 1, child_pid);
	pid[n] = '\0';
	ip_add_veth_newns[5] = pid;
	if (exec_ip(ip_add_veth_newns)) {
		fprintf(stderr, "ip_add_veth_newns is failed\n");
		restore_network();
		return 2;
	}

	/* Configure veth0. */
	if (exec_ip(ip_link_veth0_up)) {
		fprintf(stderr, "ip_link_veth0_up is failed\n");
		restore_network();
		return 3;
	}

	if (exec_ip(ip_add_addr)) {
		fprintf(stderr, "ip_add_addr is failed\n");
		restore_network();
		return 4;
	}

	return 0;
}

int main(void)
{
	int child_pid;
	int fd[2];

	if (pipe(fd) == -1) {
		perror("pipe");
		return 1;
	}

	/* SIGCHLD means send a signal the parent after the child has finished
	 * CLONE_NEWNS - create a new namespace for mount as well as get
	 * copy of all mount points.
	 */
	child_pid = clone(child_fn, child_stack + stack_size - 1, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, fd);
	if (child_pid == -1) {
		perror("clone");
		return 2;
	}

	close(fd[0]);
	/* Create veth interface. */
	if (init_network_host(child_pid)) {
		fprintf(stderr, "init_network_host is failed\n");
		write(fd[1], "-1", 1);
		wait(NULL);
		return 3;
	}
	close(fd[1]);

	wait(NULL);
	restore_network();
	return 0;
}
