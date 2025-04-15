#define _GNU_SOURCE
#include <sched.h>				/* clone */
#include <sys/types.h>			/* getpid */
#include <unistd.h>				/* getpid, sleep */
#include <sys/wait.h>			/* wait */
#include <stdio.h>				/* printf */
#include <stdlib.h>				/* exit */

enum { stack_size = 1024 * 2 };

static char child_stack[stack_size];

static int child_fn()
{
	printf("child1: PID in a new namespace: %d (PPID: %d)\n", getpid(), getppid());
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

	printf("parent_pid: %d\n", getpid());

	/* The calling process is not moved into the new namespace. The first child
	 * created by the calling process will have the proccess ID 1
	 */
	if (unshare(CLONE_NEWPID) == -1) {
		perror("unshare");
		return 1;
	}

	/* SIGCHLD means send a signal the parent after the child has finished */
	child_pid = clone(child_fn, child_stack + stack_size - 1, SIGCHLD, NULL);
	if (child_pid == -1) {
		perror("clone");
		return 2;
	}

	printf("child_pid: %d\n", child_pid);
	wait(NULL);
	return 0;
}
