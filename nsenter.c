/* A simplified version of the nsenter program. */
#define _GNU_SOURCE				/* clone */
#include <sched.h>
#include <stdio.h>				/* printf */
#include <stdlib.h>				/* exit */
#include <unistd.h>				/* execvp */
#include <sys/types.h>			/* open */
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>				/* strncpy */
#include <sys/wait.h>			/* wait */

/* Linux core 5.6 */
#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME	0x00000080
#endif

enum { a_flag = 1 << 0, m_flag = 1 << 1, u_flag = 1 << 2,
	i_flag = 1 << 3, n_flag = 1 << 4, p_flag = 1 << 5,
	U_flag = 1 << 6, c_flag = 1 << 7, T_flag = 1 << 8
};

enum { max_buf = 32, max_ns = 8 };

struct cmdline_opts {
	int target;
	unsigned int flags;
	char **argv;
};

struct ns_info {
	unsigned int flag;
	const char *name;
	int clone_flag;
};

static const struct ns_info ns_list[max_ns] = {
	{m_flag, "mnt", CLONE_NEWNS},
	{u_flag, "uts", CLONE_NEWUTS},
	{i_flag, "ipc", CLONE_NEWIPC},
	{n_flag, "net", CLONE_NEWNET},
	{p_flag, "pid", CLONE_NEWPID},
	{U_flag, "user", CLONE_NEWUSER},
	{c_flag, "cgroup", CLONE_NEWCGROUP},
	{T_flag, "time", CLONE_NEWTIME}
};

/* The help prints information about using program. */
static void help()
{
	puts("nsenter program: run a program with namespaces of other processes\n"
		 "\n"
		 "Usage: nsenter -t <target> [options] <program> \n"
		 "Example: ./nsenter -t `pidof <program_name>` /bin/bash\n"
		 "\n"
		 "Options are:\n"
		 " -t <target>  target process to get namespaces from\n"
		 " -a           enter all namespaces (default)\n"
		 " -m           enter mount namespace\n"
		 " -u           enter UTS namespace\n"
		 " -i           enter System V IPC namespace\n"
		 " -n           enter network namespace\n"
		 " -p           enter pid namespace\n"
		 " -U           enter user namespace\n"
		 " -c           enter cgroup namespace\n"
		 " -T           enter time namespace\n" " -h           display this help\n");
}

/* The pas_atoi converts from ASCII to int (only positive number). */
static int pos_atoi(const char *s)
{
	int i, n = 0;

	if (!s)
		return -1;

	for (i = 0; s[i] >= '0' && s[i] <= '9'; i++) {
		n = n * 10 + s[i] - '0';

		/* overflow */
		if (n < 0)
			return -1;
	}

	return n;

}

/* The init_cmdline_opts initializes struct cmline_opts. */
static void init_cmdline_opts(struct cmdline_opts *opts)
{
	opts->target = 0;
	opts->flags = 0;
	opts->argv = NULL;
}

/* The parse_cmdline parses command line arguments. */
static int parse_cmdline(int argc, char *argv[], struct cmdline_opts *opts)
{
	int idx = 1;
	while (idx < argc) {
		if (argv[idx][0] == '-') {
			char optc = argv[idx][1];
			if (optc == 't' && (idx + 1 >= argc || argv[idx + 1][0] == '-')) {
				fprintf(stderr, "-%c needs argument; try -h for help\n", optc);
				return 1;
			}
			switch (optc) {
			case 't':
				opts->target = pos_atoi(argv[idx + 1]);
				if (opts->target == -1) {
					fprintf(stderr, "invalid pid for target\n");
					return 2;
				}
				idx += 2;
				break;
			case 'a':
				opts->flags |= a_flag;
				idx++;
				break;
			case 'm':
				opts->flags |= m_flag;
				idx++;
				break;
			case 'u':
				opts->flags |= u_flag;
				idx++;
				break;
			case 'i':
				opts->flags |= i_flag;
				idx++;
				break;
			case 'n':
				opts->flags |= n_flag;
				idx++;
				break;
			case 'p':
				opts->flags |= p_flag;
				idx++;
				break;
			case 'U':
				opts->flags |= U_flag;
				idx++;
				break;
			case 'c':
				opts->flags |= c_flag;
				idx++;
				break;
			case 'T':
				opts->flags |= T_flag;
				idx++;
				break;
			case 'h':
				help();
				exit(0);
			default:
				fprintf(stderr, "unknowm option '%c'\n", argv[idx][1]);
				return 3;
			}
		} else if (opts->target) {
			opts->argv = &argv[idx];
			break;
		} else {
			fprintf(stderr, "stray parameter '%s'\n", argv[idx]);
			return 4;
		}
	}

	if (opts->flags == 0)
		opts->flags = a_flag;

	return 0;
}

/* The exec_in_container reassociates process with namespace(s) and
 * executes program in a new namespace(s).
 */
int exec_in_container(struct cmdline_opts *opts)
{
	char *start;
	char buf[max_buf];
	int fd[max_ns];
	int n, i, pid;

	for (i = 0; i < max_ns; i++)
		fd[i] = -1;

	/* Prepare /proc/<pid>/ns/ path. */
	n = snprintf(buf, max_buf, "/proc/%d/ns/", opts->target);
	start = buf + n;

	if (opts->flags & a_flag)
		opts->flags = ~opts->flags;

	/* Open all requested namespaces. */
	for (i = 0; i < max_ns; i++) {
		if (!(opts->flags & ns_list[i].flag))
			continue;

		strncpy(start, ns_list[i].name, max_buf - (start - buf));
		fd[i] = open(buf, O_RDONLY);
		if (fd[i] == -1) {
			perror("open");
			fprintf(stderr, "%s\n", buf);
			return 1;
		}
	}

	/* Join all namespaces. */
	for (i = 0; i < max_ns; i++) {
		if (fd[i] == -1)
			continue;

		if (setns(fd[i], ns_list[i].clone_flag)) {
			perror("setns");
			fprintf(stderr, "join in %s namespace is failed\n", ns_list[i].name);
		}
		close(fd[i]);
	}

	/* clone(child_stack=NULL, CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD,
	 *       child_tidptr=0x7f117f138850);
	 */
	pid = fork();
	if (pid == -1) {
		perror("fork");
		return 2;
	}

	if (pid == 0) {
		execvp(opts->argv[0], opts->argv);
		perror(opts->argv[0]);
		fflush(stderr);
		_exit(1);
	}

	wait(NULL);
	return 0;
}

int main(int argc, char **argv)
{
	struct cmdline_opts opts;

	init_cmdline_opts(&opts);
	if (parse_cmdline(argc, argv, &opts))
		return 1;

/*	printf("target: %d\n", opts.target);
	printf("flags: %x\n", opts.flags);
	int i;
	if (opts.argv)
		for (i = 0; opts.argv[i]; i++) {
			if (i == 0)
				printf("name_prog: %s\n", opts.argv[0]);
			printf("args_prog: %s\n", opts.argv[i]);
		}*/

	if (opts.target == 0) {
		fprintf(stderr, "The -t parameter must be specified\n");
		return 2;
	}

	if (opts.argv == NULL) {
		fprintf(stderr, "Program must be specified that will run inside the other namespace\n");
		return 3;
	}

	if (exec_in_container(&opts)) {
		fprintf(stderr, "exec_in_container is failed\n");
		return 4;
	}

	return 0;
}
