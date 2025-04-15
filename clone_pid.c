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

int main(void)
{
	int child_pid;

	printf("parent_pid: %d\n", getpid());

	/* SIGCHLD means send a signal the parent after the child has finished */
	child_pid = clone(child_fn, child_stack + stack_size - 1, CLONE_NEWPID | SIGCHLD, NULL);
	if (child_pid == -1) {
		perror("clone");
		return 1;
	}

	printf("child_pid: %d\n", child_pid);
	wait(NULL);
	return 0;
}
