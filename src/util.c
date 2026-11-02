#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lcr.h"

void die(const char *fmt, ...)
{
	int saved = errno;
	va_list ap;

	fputs("lcr: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (saved)
		fprintf(stderr, ": %s", strerror(saved));
	fputc('\n', stderr);
	exit(125);
}

void fail(const char *fmt, ...)
{
	va_list ap;

	fputs("lcr: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(125);
}

void warnf(const char *fmt, ...)
{
	int saved = errno;
	va_list ap;

	fputs("lcr: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (saved)
		fprintf(stderr, ": %s", strerror(saved));
	fputc('\n', stderr);
	errno = saved;
}

static int write_fd_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

int write_file(const char *path, const char *data)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int rc, saved;

	if (fd < 0)
		return -1;
	rc = write_fd_all(fd, data, strlen(data));
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

int write_file_create(const char *path, const char *data, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	int rc, saved;

	if (fd < 0)
		return -1;
	rc = write_fd_all(fd, data, strlen(data));
	saved = errno;
	close(fd);
	errno = saved;
	return rc;
}

ssize_t read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t got = 0;

	if (fd < 0)
		return -1;
	while (got < len - 1) {
		ssize_t n = read(fd, buf + got, len - 1 - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	buf[got] = '\0';
	return (ssize_t)got;
}

int mkdir_p(const char *path, mode_t mode)
{
	char tmp[PATH_MAX];
	size_t len = strlen(path);
	char *p;

	if (len >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(tmp, path, len + 1);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, mode) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, mode) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

void join_path(char *out, size_t n, const char *a, const char *b)
{
	int len;

	while (*b == '/')
		b++;
	len = snprintf(out, n, "%s/%s", a, b);
	if (len < 0 || (size_t)len >= n) {
		errno = ENAMETOOLONG;
		fail("path too long: %s/%s", a, b);
	}
}

long long parse_size(const char *s)
{
	char *end;
	long long v;

	errno = 0;
	v = strtoll(s, &end, 10);
	if (errno || end == s || v < 0)
		return -1;
	switch (tolower((unsigned char)*end)) {
	case 'k':
		v <<= 10;
		end++;
		break;
	case 'm':
		v <<= 20;
		end++;
		break;
	case 'g':
		v <<= 30;
		end++;
		break;
	case '\0':
		break;
	default:
		return -1;
	}
	if (*end == 'b' || *end == 'B')
		end++;
	if (*end)
		return -1;
	return v;
}

int parse_cpus(const char *s, long *quota, long *period)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (errno || end == s || *end || v <= 0)
		return -1;
	*period = 100000;
	*quota = (long)(v * 100000.0 + 0.5);
	if (*quota < 1000)
		*quota = 1000;
	return 0;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)ftw;
	if (flag == FTW_DP || flag == FTW_D || flag == FTW_DNR)
		rmdir(path);
	else
		unlink(path);
	return 0;
}

int rm_rf(const char *path)
{
	return nftw(path, rm_entry, 64, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

int proc_start_time(pid_t pid, unsigned long long *start)
{
	char path[64];
	char buf[1024];
	char *p;
	int field;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (read_file(path, buf, sizeof(buf)) < 0)
		return -1;
	p = strrchr(buf, ')');
	if (!p)
		return -1;
	p++;
	for (field = 0; field < 19; field++) {
		p = strchr(p + 1, ' ');
		if (!p)
			return -1;
	}
	*start = strtoull(p + 1, NULL, 10);
	return 0;
}

ssize_t run_cmd_output(const char *const argv[], char *buf, size_t len)
{
	int p[2];
	pid_t pid;
	size_t got = 0;
	int status;

	if (pipe2(p, O_CLOEXEC) < 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 2);
		}
		dup2(p[1], 1);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(p[1]);
	while (got < len - 1) {
		ssize_t n = read(p[0], buf + got, len - 1 - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(p[0]);
	buf[got] = '\0';
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return (ssize_t)got;
}

int run_cmd_stdin(const char *const argv[], const char *input, int quiet)
{
	int p[2];
	pid_t pid;
	int status;

	if (pipe2(p, O_CLOEXEC) < 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		dup2(p[0], 0);
		if (quiet) {
			int null = open("/dev/null", O_WRONLY);
			if (null >= 0) {
				dup2(null, 1);
				dup2(null, 2);
			}
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(p[0]);
	if (input)
		write_fd_all(p[1], input, strlen(input));
	close(p[1]);
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = ECHILD;
		return -1;
	}
	return 0;
}
