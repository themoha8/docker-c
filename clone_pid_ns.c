#define _GNU_SOURCE
#include <sched.h>				/* clone */
#include <sys/types.h>			/* getpid */
#include <unistd.h>				/* getpid, sleep */
#include <sys/wait.h>			/* wait */
#include <stdio.h>				/* printf */
#include <sys/mount.h>			/* mount */
#include <stdlib.h>				/* exit */

enum { stack_size = 1024 * 8 };

static char child_stack[stack_size];

static int child_fn()
{
	int pid;

	printf("init: PID in a new namespace: %d (PPID is %d)\n\n", getpid(), getppid());

	pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(1);
	}

	if (pid == 0) {
		printf("child1 in the new namespace: PID %d (PPID is %d)\n", getpid(), getppid());
		printf("Without mounting proc:\n");
		execlp("ps", "ps", NULL);
		perror("ps");
		fflush(stderr);
		_exit(2);
	}

	/* In order to first fork have finished */
	wait(NULL);

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
		printf("\nchild2 in the new namespace:: PID %d (PPID is %d)\n", getpid(), getppid());
		printf("After mounting proc:\n");
		execlp("ps", "ps", NULL);
		perror("ps");
		fflush(stderr);
		_exit(6);
	}

	wait(NULL);
	exit(0);
}

/* 							INTRESTING 
 * The PID 1 process has a special function: it should become all
 * the orphan processes' parent process. If PID 1 process in the root
 * namespace exits, kernel will panic. If PID 1 process in a sub namespace
 * exits, linux kernel will call the disable_pid_allocation function, which
 * will clean the PIDNS_HASH_ADDING flag in that namespace.
 */

int main(void)
{
	int child_pid;

	/* SIGCHLD means send a signal the parent after the child has finished */
	/* CLONE_NEWNS - create a new namespace for mount as well as get copy of all mount points */
	child_pid = clone(child_fn, child_stack + stack_size - 1, CLONE_NEWPID | CLONE_NEWNS | SIGCHLD, NULL);
	if (child_pid == -1) {
		perror("clone");
		return 1;
	}

	wait(NULL);
	return 0;
}
