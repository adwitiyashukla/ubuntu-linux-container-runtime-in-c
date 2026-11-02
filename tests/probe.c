#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *errname(int e)
{
	switch (e) {
	case EPERM:
		return "EPERM";
	case EACCES:
		return "EACCES";
	case ENOSYS:
		return "ENOSYS";
	case EINVAL:
		return "EINVAL";
	case ENOENT:
		return "ENOENT";
	default:
		return "ERR";
	}
}

static int do_syscall(const char *name)
{
	long rc;

	errno = 0;
	if (!strcmp(name, "add_key"))
		rc = syscall(SYS_add_key, "user", "lcr-probe", "x", 1, -2);
	else if (!strcmp(name, "unshare"))
		rc = unshare(CLONE_NEWUTS);
	else if (!strcmp(name, "mount"))
		rc = mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
	else if (!strcmp(name, "sethostname"))
		rc = sethostname("probe", 5);
	else if (!strcmp(name, "keyctl"))
		rc = syscall(SYS_keyctl, 0, 0, 0, 0, 0);
	else {
		fprintf(stderr, "unknown syscall %s\n", name);
		return 2;
	}
	if (rc < 0) {
		printf("%s\n", errname(errno));
		return 1;
	}
	printf("ok\n");
	return 0;
}

static int do_hog(long mb)
{
	size_t len = (size_t)mb << 20;
	char *p = malloc(len);
	size_t i;

	if (!p) {
		printf("malloc failed\n");
		return 1;
	}
	for (i = 0; i < len; i += 4096)
		p[i] = (char)i;
	printf("allocated %ld MiB\n", mb);
	return 0;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int do_spin(double seconds)
{
	double start = now();
	struct rusage ru;
	volatile unsigned long x = 0;

	while (now() - start < seconds)
		x++;
	getrusage(RUSAGE_SELF, &ru);
	printf("cpu=%.2f\n", (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 +
			      (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6);
	return 0;
}

static int do_syscalls(long n)
{
	double start = now();
	long i;

	for (i = 0; i < n; i++)
		syscall(SYS_getppid);
	printf("ns_per_call=%.1f\n", (now() - start) * 1e9 / (double)n);
	return 0;
}

static int do_forks(int n)
{
	int ok = 0;
	int i;

	for (i = 0; i < n; i++) {
		pid_t pid = fork();
		if (pid == 0) {
			sleep(2);
			_exit(0);
		}
		if (pid > 0)
			ok++;
	}
	printf("forked=%d\n", ok);
	while (wait(NULL) > 0)
		;
	return 0;
}

static int count_zombies(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int zombies = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char path[300];
		char buf[512];
		char *p;
		int fd;
		ssize_t n;

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = '\0';
		p = strrchr(buf, ')');
		if (p && p[1] == ' ' && p[2] == 'Z')
			zombies++;
	}
	closedir(d);
	return zombies;
}

static int do_zombie(void)
{
	pid_t child = fork();
	struct timespec ts = { 0, 500 * 1000 * 1000 };

	if (child == 0) {
		pid_t grandchild = fork();
		if (grandchild == 0) {
			struct timespec t = { 0, 100 * 1000 * 1000 };
			nanosleep(&t, NULL);
			_exit(0);
		}
		_exit(0);
	}
	waitpid(child, NULL, 0);
	nanosleep(&ts, NULL);
	printf("zombies=%d\n", count_zombies());
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "syscall"))
		return do_syscall(argv[2]);
	if (argc >= 3 && !strcmp(argv[1], "hog"))
		return do_hog(atol(argv[2]));
	if (argc >= 3 && !strcmp(argv[1], "spin"))
		return do_spin(atof(argv[2]));
	if (argc >= 3 && !strcmp(argv[1], "forks"))
		return do_forks(atoi(argv[2]));
	if (argc >= 3 && !strcmp(argv[1], "syscalls"))
		return do_syscalls(atol(argv[2]));
	if (argc >= 2 && !strcmp(argv[1], "zombie"))
		return do_zombie();
	fprintf(stderr, "usage: probe syscall NAME | hog MB | spin SECONDS | forks N | syscalls N | zombie\n");
	return 2;
}
