#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lcr.h"

const char *state_root(void)
{
	static char buf[PATH_MAX];
	const char *env = getenv("LCR_ROOT");
	const char *xdg;

	if (env && *env)
		return env;
	if (geteuid() == 0)
		return "/var/lib/lcr";
	xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg && *xdg)
		snprintf(buf, sizeof(buf), "%s/lcr", xdg);
	else
		snprintf(buf, sizeof(buf), "/tmp/lcr-%u", getuid());
	return buf;
}

static const char *net_name(enum net_mode m)
{
	switch (m) {
	case NET_HOST:
		return "host";
	case NET_BRIDGE:
		return "bridge";
	default:
		return "none";
	}
}

void state_save(const struct config *cfg, pid_t pid, const struct cgroup *cg)
{
	char path[PATH_MAX];
	char buf[8192];
	char cmd[1024] = "";
	size_t off = 0;
	unsigned long long start = 0;
	int i;

	for (i = 0; i < cfg->argc && off + 2 < sizeof(cmd); i++)
		off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, "%s%s", i ? " " : "", cfg->argv[i]);
	if (pid > 0)
		proc_start_time(pid, &start);
	snprintf(buf, sizeof(buf),
		 "id=%s\nname=%s\nhostname=%s\nimage=%s\nworkdir=%s\nnet=%s\nip=%s\ncmd=%s\n"
		 "caps=%llx\nseccomp=%d\nprivileged=%d\nrootless=%d\nnports=%d\n"
		 "cgroup_mode=%d\ncgroup=%s\npid=%d\nstart=%llu\ncreated=%ld\n",
		 cfg->id, cfg->name, cfg->hostname, cfg->image, cfg->workdir, net_name(cfg->net), cfg->ip, cmd,
		 (unsigned long long)cfg->caps, cfg->seccomp, cfg->privileged, cfg->rootless, cfg->nports,
		 cg ? (int)cg->mode : 0, cg ? (cg->mode == CG_V2 ? cg->path : cg->rel) : "",
		 (int)pid, start, (long)time(NULL));
	join_path(path, sizeof(path), cfg->state_dir, "config");
	if (write_file_create(path, buf, 0600) < 0)
		die("write %s", path);
}

void state_mark_exited(const struct config *cfg, int code)
{
	char path[PATH_MAX];
	char buf[16];

	join_path(path, sizeof(path), cfg->state_dir, "exit");
	snprintf(buf, sizeof(buf), "%d\n", code);
	write_file_create(path, buf, 0600);
}

void state_remove(const struct config *cfg)
{
	if (cfg->state_dir[0])
		rm_rf(cfg->state_dir);
}

static void set_field(struct state *st, const char *key, const char *val)
{
	if (!strcmp(key, "id"))
		snprintf(st->id, sizeof(st->id), "%s", val);
	else if (!strcmp(key, "name"))
		snprintf(st->name, sizeof(st->name), "%s", val);
	else if (!strcmp(key, "hostname"))
		snprintf(st->hostname, sizeof(st->hostname), "%s", val);
	else if (!strcmp(key, "image"))
		snprintf(st->image, sizeof(st->image), "%s", val);
	else if (!strcmp(key, "workdir"))
		snprintf(st->workdir, sizeof(st->workdir), "%s", val);
	else if (!strcmp(key, "ip"))
		snprintf(st->ip, sizeof(st->ip), "%s", val);
	else if (!strcmp(key, "cmd"))
		snprintf(st->cmd, sizeof(st->cmd), "%s", val);
	else if (!strcmp(key, "cgroup"))
		snprintf(st->cgroup, sizeof(st->cgroup), "%s", val);
	else if (!strcmp(key, "net"))
		st->net = !strcmp(val, "host") ? NET_HOST : !strcmp(val, "bridge") ? NET_BRIDGE : NET_NONE;
	else if (!strcmp(key, "caps"))
		st->caps = strtoull(val, NULL, 16);
	else if (!strcmp(key, "seccomp"))
		st->seccomp = atoi(val);
	else if (!strcmp(key, "privileged"))
		st->privileged = atoi(val);
	else if (!strcmp(key, "rootless"))
		st->rootless = atoi(val);
	else if (!strcmp(key, "nports"))
		st->nports = atoi(val);
	else if (!strcmp(key, "cgroup_mode"))
		st->cgroup_mode = (enum cg_mode)atoi(val);
	else if (!strcmp(key, "pid"))
		st->pid = (pid_t)atoi(val);
	else if (!strcmp(key, "start"))
		st->start = strtoull(val, NULL, 10);
	else if (!strcmp(key, "created"))
		st->created = atol(val);
}

int state_load(const char *dir, struct state *st)
{
	char path[PATH_MAX];
	char buf[8192];
	char *line;

	memset(st, 0, sizeof(*st));
	snprintf(st->dir, sizeof(st->dir), "%s", dir);
	join_path(path, sizeof(path), dir, "config");
	if (read_file(path, buf, sizeof(buf)) <= 0)
		return -1;
	line = buf;
	while (line && *line) {
		char *next = strchr(line, '\n');
		char *eq;

		if (next)
			*next++ = '\0';
		eq = strchr(line, '=');
		if (eq) {
			*eq = '\0';
			set_field(st, line, eq + 1);
		}
		line = next;
	}
	join_path(path, sizeof(path), dir, "exit");
	if (read_file(path, buf, sizeof(buf)) > 0) {
		st->exited = 1;
		st->exit_code = atoi(buf);
	}
	return st->id[0] ? 0 : -1;
}

int state_alive(const struct state *st)
{
	unsigned long long start;

	if (st->pid <= 0 || st->exited)
		return 0;
	if (kill(st->pid, 0) < 0 && errno != EPERM)
		return 0;
	if (proc_start_time(st->pid, &start) < 0)
		return 0;
	return start == st->start;
}

int state_resolve(const char *ref, struct state *st)
{
	DIR *d = opendir(state_root());
	struct dirent *e;
	struct state cand;
	char dir[PATH_MAX];
	int matches = 0;
	int found = 0;

	if (!d || !*ref)
		return -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		join_path(dir, sizeof(dir), state_root(), e->d_name);
		if (state_load(dir, &cand) < 0)
			continue;
		if (!strcmp(cand.id, ref) || (cand.name[0] && !strcmp(cand.name, ref))) {
			*st = cand;
			closedir(d);
			return 0;
		}
		if (!strncmp(cand.id, ref, strlen(ref))) {
			*st = cand;
			found = 1;
			matches++;
		}
	}
	closedir(d);
	if (matches > 1)
		fail("%s matches more than one container", ref);
	return found ? 0 : -1;
}

static int by_created(const void *a, const void *b)
{
	const struct state *x = a;
	const struct state *y = b;

	if (x->created != y->created)
		return x->created < y->created ? -1 : 1;
	return strcmp(x->id, y->id);
}

static void format_age(long secs, char *out, size_t n)
{
	if (secs < 0)
		secs = 0;
	if (secs < 60)
		snprintf(out, n, "%lds", secs);
	else if (secs < 3600)
		snprintf(out, n, "%ldm", secs / 60);
	else if (secs < 86400)
		snprintf(out, n, "%ldh", secs / 3600);
	else
		snprintf(out, n, "%ldd", secs / 86400);
}

int cmd_ps(int argc, char **argv)
{
	DIR *d;
	struct dirent *e;
	struct state *list = NULL;
	size_t n = 0, cap = 0, i;
	long now = (long)time(NULL);

	(void)argc;
	(void)argv;
	printf("%-12s  %-16s  %-7s  %-13s  %-11s  %-6s  %s\n",
	       "ID", "NAME", "PID", "STATUS", "IP", "AGE", "COMMAND");
	d = opendir(state_root());
	if (!d)
		return 0;
	while ((e = readdir(d)) != NULL) {
		char dir[PATH_MAX];

		if (e->d_name[0] == '.')
			continue;
		join_path(dir, sizeof(dir), state_root(), e->d_name);
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			list = realloc(list, cap * sizeof(*list));
			if (!list)
				fail("out of memory");
		}
		if (state_load(dir, &list[n]) == 0)
			n++;
	}
	closedir(d);
	qsort(list, n, sizeof(*list), by_created);
	for (i = 0; i < n; i++) {
		char status[32];
		char age[16];
		int alive = state_alive(&list[i]);

		if (alive)
			snprintf(status, sizeof(status), "running");
		else if (list[i].exited)
			snprintf(status, sizeof(status), "exited (%d)", list[i].exit_code);
		else
			snprintf(status, sizeof(status), "dead");
		format_age(now - list[i].created, age, sizeof(age));
		printf("%-12s  %-16s  %-7d  %-13s  %-11s  %-6s  %s\n",
		       list[i].id, list[i].name[0] ? list[i].name : "-", alive ? list[i].pid : 0,
		       status, list[i].ip[0] ? list[i].ip : "-", age, list[i].cmd);
	}
	free(list);
	return 0;
}

static int parse_signal(const char *s)
{
	static const struct {
		const char *name;
		int sig;
	} table[] = {
		{ "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "KILL", SIGKILL },
		{ "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "TERM", SIGTERM }, { "STOP", SIGSTOP },
		{ "CONT", SIGCONT }, { "WINCH", SIGWINCH }, { "ALRM", SIGALRM }, { "PIPE", SIGPIPE },
	};
	size_t i;

	if (isdigit((unsigned char)s[0]))
		return atoi(s);
	if (!strncasecmp(s, "SIG", 3))
		s += 3;
	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (!strcasecmp(s, table[i].name))
			return table[i].sig;
	}
	return -1;
}

static int signal_container(const struct state *st, int sig)
{
	unsigned long long start;
	int pidfd = (int)syscall(SYS_pidfd_open, st->pid, 0);

	if (pidfd < 0)
		return -1;
	if (proc_start_time(st->pid, &start) < 0 || start != st->start) {
		close(pidfd);
		errno = ESRCH;
		return -1;
	}
	if (syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0) < 0) {
		int saved = errno;
		close(pidfd);
		errno = saved;
		return -1;
	}
	close(pidfd);
	return 0;
}

int cmd_kill(int argc, char **argv)
{
	struct state st;
	int sig = SIGTERM;

	if (argc < 2 || argc > 3) {
		fputs("usage: lcr kill CONTAINER [SIGNAL]\n", stderr);
		return 125;
	}
	if (argc == 3) {
		sig = parse_signal(argv[2]);
		if (sig <= 0)
			fail("unknown signal %s", argv[2]);
	}
	if (state_resolve(argv[1], &st) < 0 || !state_alive(&st))
		fail("no running container matches %s", argv[1]);
	if (signal_container(&st, sig) < 0)
		die("send signal %d to %s", sig, st.id);
	return 0;
}

static int wait_until_dead(const struct state *st, int tries)
{
	struct timespec ts = { 0, 20 * 1000 * 1000 };

	while (tries-- > 0) {
		if (!state_alive(st))
			return 0;
		nanosleep(&ts, NULL);
	}
	return -1;
}

int cmd_rm(int argc, char **argv)
{
	struct timespec ts = { 0, 20 * 1000 * 1000 };
	struct state st;
	struct cgroup cg;
	int force = 0;
	int i;
	const char *ref = NULL;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--force"))
			force = 1;
		else if (!ref)
			ref = argv[i];
		else
			ref = "";
	}
	if (!ref || !*ref) {
		fputs("usage: lcr rm [-f] CONTAINER\n", stderr);
		return 125;
	}
	if (state_resolve(ref, &st) < 0)
		fail("no container matches %s", ref);
	if (state_alive(&st)) {
		if (!force)
			fail("container %s is running, stop it first or use -f", st.id);
		signal_container(&st, SIGKILL);
		if (wait_until_dead(&st, 250) < 0)
			fail("container %s did not stop", st.id);
	}
	if (st.cgroup_mode != CG_NONE && st.cgroup[0]) {
		cgroup_open_existing(&cg, st.cgroup_mode, st.cgroup);
		cgroup_destroy(&cg);
	}
	for (i = 0; i < 50; i++) {
		rm_rf(st.dir);
		if (access(st.dir, F_OK) < 0)
			return 0;
		nanosleep(&ts, NULL);
	}
	fail("could not remove %s", st.dir);
	return 125;
}

int cmd_stats(int argc, char **argv)
{
	struct state st;
	struct cgroup cg;

	if (argc != 2) {
		fputs("usage: lcr stats CONTAINER\n", stderr);
		return 125;
	}
	if (state_resolve(argv[1], &st) < 0 || !state_alive(&st))
		fail("no running container matches %s", argv[1]);
	if (st.cgroup_mode == CG_NONE || !st.cgroup[0]) {
		fail("container %s has no cgroup", st.id);
	}
	cgroup_open_existing(&cg, st.cgroup_mode, st.cgroup);
	if (cgroup_print_stats(&cg) < 0)
		die("read cgroup statistics for %s", st.id);
	return 0;
}

int cmd_logs(int argc, char **argv)
{
	struct state st;
	char path[PATH_MAX];
	char buf[65536];
	int fd;
	ssize_t n;

	if (argc != 2) {
		fputs("usage: lcr logs CONTAINER\n", stderr);
		return 125;
	}
	if (state_resolve(argv[1], &st) < 0)
		fail("no container matches %s", argv[1]);
	join_path(path, sizeof(path), st.dir, "log");
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		fail("container %s was not started with -d, there is no log", st.id);
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
			break;
	}
	close(fd);
	return 0;
}

static const char exec_usage[] =
	"usage: lcr exec [-u UID[:GID]] [-e K=V] [-w DIR] CONTAINER COMMAND [ARG...]\n";

int cmd_exec(int argc, char **argv)
{
	static const char *const names[] = { "user", "mnt", "uts", "ipc", "net", "pid", "cgroup" };
	struct state st;
	struct config cfg;
	struct cgroup cg;
	int fds[7];
	int c, i, status;
	pid_t pid;
	const char *workdir = NULL;

	memset(&cfg, 0, sizeof(cfg));
	optind = 1;
	while ((c = getopt(argc, argv, "+u:e:w:h")) != -1) {
		switch (c) {
		case 'u': {
			char *end;
			cfg.uid = (uid_t)strtoul(optarg, &end, 10);
			cfg.gid = cfg.uid;
			if (*end == ':')
				cfg.gid = (gid_t)strtoul(end + 1, &end, 10);
			if (*end || end == optarg)
				fail("user must be numeric UID[:GID]");
			cfg.have_user = 1;
			break;
		}
		case 'e':
			if (cfg.nenv >= MAX_ENV)
				fail("too many environment variables");
			cfg.env[cfg.nenv++] = optarg;
			break;
		case 'w':
			workdir = optarg;
			break;
		default:
			fputs(exec_usage, stderr);
			return 125;
		}
	}
	if (argc - optind < 2) {
		fputs(exec_usage, stderr);
		return 125;
	}
	if (state_resolve(argv[optind], &st) < 0 || !state_alive(&st))
		fail("no running container matches %s", argv[optind]);
	snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", st.hostname);
	if (st.cgroup_mode != CG_NONE && st.cgroup[0]) {
		cgroup_open_existing(&cg, st.cgroup_mode, st.cgroup);
		cgroup_add(&cg, getpid());
	}
	for (i = 0; i < 7; i++) {
		char path[64];

		fds[i] = -1;
		if (i == 0 && !st.rootless)
			continue;
		snprintf(path, sizeof(path), "/proc/%d/ns/%s", st.pid, names[i]);
		fds[i] = open(path, O_RDONLY | O_CLOEXEC);
		if (fds[i] < 0 && !(i == 6 && errno == ENOENT))
			die("open %s", path);
	}
	for (i = 0; i < 7; i++) {
		if (fds[i] < 0)
			continue;
		if (setns(fds[i], 0) < 0)
			die("join %s namespace of %s", names[i], st.id);
		close(fds[i]);
	}
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		const char *dir = workdir ? workdir : (st.workdir[0] ? st.workdir : "/");

		if (chdir(dir) < 0)
			die("chdir %s", dir);
		setup_env(&cfg);
		if (!st.privileged)
			caps_apply(st.caps);
		if (cfg.have_user)
			drop_to_user(cfg.uid, cfg.gid);
		if (st.seccomp)
			seccomp_apply();
		execvp(argv[optind + 1], argv + optind + 1);
		fprintf(stderr, "lcr: exec %s: %s\n", argv[optind + 1], strerror(errno));
		_exit(errno == ENOENT ? 127 : 126);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			die("waitpid");
	}
	return exit_status_code(status);
}
