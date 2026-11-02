#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <linux/sched.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lcr.h"

enum {
	OPT_NET = 256,
	OPT_CPUS,
	OPT_PIDS,
	OPT_READ_ONLY,
	OPT_RW_IMAGE,
	OPT_CAP_ADD,
	OPT_CAP_DROP,
	OPT_PRIVILEGED,
	OPT_SECCOMP,
	OPT_HOSTNAME,
	OPT_HELP,
};

static const struct option run_options[] = {
	{ "detach", no_argument, NULL, 'd' },
	{ "name", required_argument, NULL, 'n' },
	{ "hostname", required_argument, NULL, OPT_HOSTNAME },
	{ "net", required_argument, NULL, OPT_NET },
	{ "publish", required_argument, NULL, 'p' },
	{ "volume", required_argument, NULL, 'v' },
	{ "env", required_argument, NULL, 'e' },
	{ "workdir", required_argument, NULL, 'w' },
	{ "user", required_argument, NULL, 'u' },
	{ "memory", required_argument, NULL, 'm' },
	{ "cpus", required_argument, NULL, OPT_CPUS },
	{ "pids", required_argument, NULL, OPT_PIDS },
	{ "read-only", no_argument, NULL, OPT_READ_ONLY },
	{ "rw-image", no_argument, NULL, OPT_RW_IMAGE },
	{ "cap-add", required_argument, NULL, OPT_CAP_ADD },
	{ "cap-drop", required_argument, NULL, OPT_CAP_DROP },
	{ "privileged", no_argument, NULL, OPT_PRIVILEGED },
	{ "seccomp", required_argument, NULL, OPT_SECCOMP },
	{ "help", no_argument, NULL, OPT_HELP },
	{ NULL, 0, NULL, 0 },
};

static const char run_usage[] =
	"usage: lcr run [options] IMAGE_DIR COMMAND [ARG...]\n"
	"\n"
	"  -d, --detach            run in the background and print the container id\n"
	"  -n, --name NAME         container name (default: generated id)\n"
	"      --hostname NAME     hostname inside the container (default: name)\n"
	"      --net MODE          none, host or bridge (default: none)\n"
	"  -p, --publish H:C       forward host TCP port H to container port C (bridge only)\n"
	"  -v, --volume S:D[:ro]   bind mount host path S at D inside the container\n"
	"  -e, --env K=V           set an environment variable (K alone copies it from the host)\n"
	"  -w, --workdir DIR       working directory inside the container (default: /)\n"
	"  -u, --user UID[:GID]    run the command as this numeric uid and gid\n"
	"  -m, --memory SIZE       memory limit, for example 64m or 1g\n"
	"      --cpus N            cpu limit in cores, for example 0.5 or 2\n"
	"      --pids N            maximum number of processes\n"
	"      --read-only         mount the root filesystem read-only\n"
	"      --rw-image          run on the image directory itself instead of an overlay\n"
	"      --cap-add CAP       add a capability to the default bounding set\n"
	"      --cap-drop CAP      drop a capability from the default bounding set\n"
	"      --privileged        keep every capability and disable seccomp and masking\n"
	"      --seccomp none      disable the default seccomp filter\n";

static volatile pid_t forward_pid;

static void forward_signal(int sig, siginfo_t *si, void *ctx)
{
	(void)ctx;
	if (forward_pid > 0 && si->si_code != SI_KERNEL)
		kill(forward_pid, sig);
}

static void install_forwarders(void)
{
	static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2, SIGWINCH };
	struct sigaction sa;
	size_t i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = forward_signal;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);
}

int exit_status_code(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 125;
}

void setup_env(const struct config *cfg)
{
	char *host_vals[MAX_ENV];
	char *term = getenv("TERM");
	int i;

	if (term)
		term = strdup(term);
	for (i = 0; i < cfg->nenv; i++) {
		char *v = strchr(cfg->env[i], '=') ? NULL : getenv(cfg->env[i]);
		host_vals[i] = v ? strdup(v) : NULL;
	}
	clearenv();
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
	setenv("HOME", cfg->have_user && cfg->uid ? "/" : "/root", 1);
	setenv("HOSTNAME", cfg->hostname, 1);
	if (term)
		setenv("TERM", term, 1);
	for (i = 0; i < cfg->nenv; i++) {
		if (strchr(cfg->env[i], '='))
			putenv(cfg->env[i]);
		else if (host_vals[i])
			setenv(cfg->env[i], host_vals[i], 1);
	}
}

void drop_to_user(uid_t uid, gid_t gid)
{
	if (setgroups(0, NULL) < 0 && errno != EPERM)
		die("clear supplementary groups");
	if (setgid(gid) < 0)
		die("setgid %u", gid);
	if (setuid(uid) < 0)
		die("setuid %u", uid);
}

static void shutdown_remaining(void)
{
	struct timespec ts = { 0, 10 * 1000 * 1000 };
	int i;

	if (kill(-1, SIGTERM) < 0 && errno == ESRCH)
		return;
	for (i = 0; i < 200; i++) {
		if (waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD)
			return;
		nanosleep(&ts, NULL);
	}
	kill(-1, SIGKILL);
	for (;;) {
		if (waitpid(-1, NULL, 0) < 0 && errno != EINTR)
			break;
	}
}

static int init_loop(pid_t cmd, const sigset_t *set)
{
	int status = 0;
	int done = 0;

	while (!done) {
		siginfo_t si;

		if (sigwaitinfo(set, &si) < 0) {
			if (errno == EINTR)
				continue;
			die("sigwaitinfo");
		}
		if (si.si_signo == SIGCHLD) {
			pid_t p;
			int st;

			while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
				if (p == cmd) {
					status = st;
					done = 1;
				}
			}
		} else if (si.si_code != SI_KERNEL) {
			kill(cmd, si.si_signo);
		}
	}
	shutdown_remaining();
	return exit_status_code(status);
}

static void container_init(const struct config *cfg, enum cg_mode cg, int sync_fd)
{
	sigset_t set, empty;
	char go;
	pid_t cmd;

	if (read(sync_fd, &go, 1) != 1)
		_exit(125);
	close(sync_fd);
	if (unshare(CLONE_NEWCGROUP) < 0)
		die("unshare cgroup namespace");
	if (sethostname(cfg->hostname, strlen(cfg->hostname)) < 0)
		die("sethostname %s", cfg->hostname);
	net_container_setup(cfg);
	mounts_setup(cfg, cg);
	if (chdir(cfg->workdir) < 0)
		die("chdir %s", cfg->workdir);
	setup_env(cfg);
	if (!cfg->privileged)
		caps_apply(cfg->caps);
	if (cfg->have_user)
		drop_to_user(cfg->uid, cfg->gid);
	if (cfg->seccomp)
		seccomp_apply();

	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGQUIT);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGWINCH);
	sigprocmask(SIG_BLOCK, &set, NULL);
	sigemptyset(&empty);

	cmd = fork();
	if (cmd < 0)
		die("fork the container command");
	if (cmd == 0) {
		sigprocmask(SIG_SETMASK, &empty, NULL);
		execvp(cfg->argv[0], cfg->argv);
		fprintf(stderr, "lcr: exec %s: %s\n", cfg->argv[0], strerror(errno));
		_exit(errno == ENOENT ? 127 : 126);
	}
	_exit(init_loop(cmd, &set));
}

static pid_t spawn_init(const struct config *cfg, const struct cgroup *cg, int *joined)
{
	struct clone_args args;
	uint64_t flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC;
	long pid;

	if (cfg->net != NET_HOST)
		flags |= CLONE_NEWNET;
	if (cfg->rootless)
		flags |= CLONE_NEWUSER;
	memset(&args, 0, sizeof(args));
	args.flags = flags;
	args.exit_signal = SIGCHLD;
	*joined = 0;
	if (cg->mode == CG_V2 && cg->fd >= 0) {
		args.flags |= CLONE_INTO_CGROUP;
		args.cgroup = (uint64_t)cg->fd;
		pid = syscall(SYS_clone3, &args, sizeof(args));
		if (pid >= 0) {
			*joined = 1;
			return (pid_t)pid;
		}
		args.flags = flags;
		args.cgroup = 0;
	}
	pid = syscall(SYS_clone3, &args, sizeof(args));
	if (pid < 0 && errno == ENOSYS)
		pid = syscall(SYS_clone, flags | SIGCHLD, 0, 0, 0, 0);
	return (pid_t)pid;
}

static void write_id_maps(pid_t pid)
{
	char path[64];
	char buf[64];

	snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
	write_file(path, "deny");
	snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
	snprintf(buf, sizeof(buf), "0 %u 1\n", getgid());
	if (write_file(path, buf) < 0)
		die("write gid_map");
	snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
	snprintf(buf, sizeof(buf), "0 %u 1\n", getuid());
	if (write_file(path, buf) < 0)
		die("write uid_map");
}

static void gen_id(char *out, size_t n)
{
	unsigned char rnd[6];
	size_t i;

	if (getrandom(rnd, sizeof(rnd), 0) != (ssize_t)sizeof(rnd))
		die("getrandom");
	for (i = 0; i < sizeof(rnd) && 2 * i + 2 < n; i++)
		snprintf(out + 2 * i, 3, "%02x", rnd[i]);
}

static void parse_bind(struct config *cfg, const char *spec)
{
	struct bind_mount *b;
	char tmp[2 * PATH_MAX];
	char *src, *dst, *mode;

	if (cfg->nbinds >= MAX_BINDS)
		fail("too many volumes");
	snprintf(tmp, sizeof(tmp), "%s", spec);
	src = tmp;
	dst = strchr(src, ':');
	if (!dst)
		fail("volume must be SRC:DST[:ro], got %s", spec);
	*dst++ = '\0';
	mode = strchr(dst, ':');
	if (mode)
		*mode++ = '\0';
	if (dst[0] != '/')
		fail("volume destination must be absolute: %s", dst);
	b = &cfg->binds[cfg->nbinds++];
	if (!realpath(src, b->src))
		die("volume source %s", src);
	snprintf(b->dst, sizeof(b->dst), "%s", dst);
	b->ro = 0;
	if (mode) {
		if (!strcmp(mode, "ro"))
			b->ro = 1;
		else if (strcmp(mode, "rw"))
			fail("volume mode must be ro or rw: %s", mode);
	}
}

static void parse_port(struct config *cfg, const char *spec)
{
	char *end;
	long host, ctr;

	if (cfg->nports >= MAX_PORTS)
		fail("too many published ports");
	host = strtol(spec, &end, 10);
	if (*end != ':' || host < 1 || host > 65535)
		fail("publish must be HOSTPORT:CONTAINERPORT, got %s", spec);
	ctr = strtol(end + 1, &end, 10);
	if (*end || ctr < 1 || ctr > 65535)
		fail("publish must be HOSTPORT:CONTAINERPORT, got %s", spec);
	cfg->ports[cfg->nports].host = (int)host;
	cfg->ports[cfg->nports].ctr = (int)ctr;
	cfg->nports++;
}

static void parse_user(struct config *cfg, const char *spec)
{
	char *end;
	unsigned long uid, gid;

	uid = strtoul(spec, &end, 10);
	if (end == spec || (*end && *end != ':'))
		fail("user must be numeric UID[:GID], got %s", spec);
	gid = uid;
	if (*end == ':') {
		const char *g = end + 1;
		gid = strtoul(g, &end, 10);
		if (end == g || *end)
			fail("user must be numeric UID[:GID], got %s", spec);
	}
	cfg->uid = (uid_t)uid;
	cfg->gid = (gid_t)gid;
	cfg->have_user = 1;
}

static void check_name(const char *name)
{
	const char *p;

	if (!*name || strlen(name) > 63)
		fail("name must be 1 to 63 characters");
	for (p = name; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.')
			fail("name may only contain letters, digits, dot, dash and underscore");
	}
}

static void parse_run_options(int argc, char **argv, struct config *cfg)
{
	int c, cap;

	memset(cfg, 0, sizeof(*cfg));
	cfg->caps = caps_default();
	cfg->seccomp = 1;
	cfg->net = NET_NONE;
	cfg->cpu_period = 100000;
	snprintf(cfg->workdir, sizeof(cfg->workdir), "/");
	optind = 1;
	while ((c = getopt_long(argc, argv, "+dn:p:v:e:w:u:m:", run_options, NULL)) != -1) {
		switch (c) {
		case 'd':
			cfg->detach = 1;
			break;
		case 'n':
			check_name(optarg);
			snprintf(cfg->name, sizeof(cfg->name), "%s", optarg);
			break;
		case OPT_HOSTNAME:
			check_name(optarg);
			snprintf(cfg->hostname, sizeof(cfg->hostname), "%s", optarg);
			break;
		case OPT_NET:
			if (!strcmp(optarg, "none"))
				cfg->net = NET_NONE;
			else if (!strcmp(optarg, "host"))
				cfg->net = NET_HOST;
			else if (!strcmp(optarg, "bridge"))
				cfg->net = NET_BRIDGE;
			else
				fail("--net must be none, host or bridge");
			break;
		case 'p':
			parse_port(cfg, optarg);
			break;
		case 'v':
			parse_bind(cfg, optarg);
			break;
		case 'e':
			if (cfg->nenv >= MAX_ENV)
				fail("too many environment variables");
			cfg->env[cfg->nenv++] = optarg;
			break;
		case 'w':
			if (optarg[0] != '/')
				fail("workdir must be an absolute path");
			snprintf(cfg->workdir, sizeof(cfg->workdir), "%s", optarg);
			break;
		case 'u':
			parse_user(cfg, optarg);
			break;
		case 'm':
			cfg->memory = parse_size(optarg);
			if (cfg->memory <= 0)
				fail("invalid memory size %s", optarg);
			break;
		case OPT_CPUS:
			if (parse_cpus(optarg, &cfg->cpu_quota, &cfg->cpu_period) < 0)
				fail("invalid cpu count %s", optarg);
			break;
		case OPT_PIDS:
			cfg->pids = strtol(optarg, NULL, 10);
			if (cfg->pids <= 0)
				fail("invalid pids limit %s", optarg);
			break;
		case OPT_READ_ONLY:
			cfg->read_only = 1;
			break;
		case OPT_RW_IMAGE:
			cfg->rw_image = 1;
			break;
		case OPT_CAP_ADD:
			cap = caps_lookup(optarg);
			if (cap < 0)
				fail("unknown capability %s", optarg);
			cfg->caps |= 1ULL << cap;
			break;
		case OPT_CAP_DROP:
			cap = caps_lookup(optarg);
			if (cap < 0)
				fail("unknown capability %s", optarg);
			cfg->caps &= ~(1ULL << cap);
			break;
		case OPT_PRIVILEGED:
			cfg->privileged = 1;
			break;
		case OPT_SECCOMP:
			if (!strcmp(optarg, "none"))
				cfg->seccomp = 0;
			else if (strcmp(optarg, "default"))
				fail("--seccomp must be default or none");
			break;
		case OPT_HELP:
			fputs(run_usage, stdout);
			exit(0);
		default:
			fputs(run_usage, stderr);
			exit(125);
		}
	}
	if (argc - optind < 2) {
		fputs(run_usage, stderr);
		exit(125);
	}
	if (!realpath(argv[optind], cfg->image))
		die("image directory %s", argv[optind]);
	cfg->argv = argv + optind + 1;
	cfg->argc = argc - optind - 1;
	if (cfg->privileged) {
		cfg->caps = caps_all();
		cfg->seccomp = 0;
	}
	if (cfg->nports && cfg->net != NET_BRIDGE)
		fail("publishing ports needs --net bridge");
}

static struct config *cleanup_cfg;
static struct cgroup *cleanup_cg;
static pid_t cleanup_pid;
static int cleanup_armed;

static void cleanup_failed_start(void)
{
	if (!cleanup_armed)
		return;
	if (cleanup_pid > 0) {
		kill(cleanup_pid, SIGKILL);
		waitpid(cleanup_pid, NULL, 0);
	}
	if (cleanup_cg)
		cgroup_destroy(cleanup_cg);
	if (cleanup_cfg) {
		net_host_teardown(cleanup_cfg);
		if (cleanup_cfg->detach)
			state_mark_exited(cleanup_cfg, 125);
		else
			state_remove(cleanup_cfg);
	}
}

static int daemonize(const struct config *cfg)
{
	char log[PATH_MAX];
	char buf[4096];
	int p[2];
	int fd, null;
	pid_t pid;
	char ok = 0;

	if (pipe2(p, O_CLOEXEC) < 0)
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid > 0) {
		close(p[1]);
		if (read(p[0], &ok, 1) == 1 && ok == 'o') {
			printf("%s\n", cfg->id);
			exit(0);
		}
		join_path(log, sizeof(log), cfg->state_dir, "log");
		if (read_file(log, buf, sizeof(buf)) > 0)
			fputs(buf, stderr);
		fprintf(stderr, "lcr: container %s failed to start\n", cfg->id);
		exit(125);
	}
	close(p[0]);
	if (setsid() < 0)
		die("setsid");
	join_path(log, sizeof(log), cfg->state_dir, "log");
	fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0)
		die("open %s", log);
	null = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (null < 0)
		die("open /dev/null");
	dup2(null, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	close(null);
	close(fd);
	return p[1];
}

int cmd_run(int argc, char **argv)
{
	struct config cfg;
	struct cgroup cg;
	struct stat st;
	int sync[2];
	int joined = 0;
	int ready_fd = -1;
	int status;
	int code;
	pid_t pid;

	parse_run_options(argc, argv, &cfg);
	if (stat(cfg.image, &st) < 0 || !S_ISDIR(st.st_mode))
		fail("image %s is not a directory", cfg.image);
	if (geteuid() != 0) {
		cfg.rootless = 1;
		if (cfg.net == NET_BRIDGE)
			fail("bridge networking needs root, use --net none or --net host");
	}
	gen_id(cfg.id, sizeof(cfg.id));
	if (!cfg.hostname[0])
		snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", cfg.name[0] ? cfg.name : cfg.id);
	if (mkdir_p(state_root(), 0700) < 0)
		die("mkdir %s", state_root());
	join_path(cfg.state_dir, sizeof(cfg.state_dir), state_root(), cfg.id);
	if (mkdir(cfg.state_dir, 0700) < 0)
		die("mkdir %s", cfg.state_dir);
	join_path(cfg.rootfs, sizeof(cfg.rootfs), cfg.state_dir, "rootfs");
	if (cfg.name[0]) {
		struct state other;
		if (state_resolve(cfg.name, &other) == 0) {
			rmdir(cfg.state_dir);
			fail("a container named %s already exists", cfg.name);
		}
	}
	state_save(&cfg, 0, NULL);
	if (cfg.detach)
		ready_fd = daemonize(&cfg);

	cleanup_cfg = &cfg;
	cleanup_cg = &cg;
	cleanup_armed = 1;
	atexit(cleanup_failed_start);
	cgroup_create(&cfg, &cg);
	if (cfg.net == NET_BRIDGE)
		net_alloc_ip(&cfg);
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sync) < 0)
		die("socketpair");
	pid = spawn_init(&cfg, &cg, &joined);
	if (pid < 0)
		die("clone");
	if (pid == 0) {
		cleanup_armed = 0;
		close(sync[0]);
		container_init(&cfg, cg.mode, sync[1]);
	}
	cleanup_pid = pid;
	close(sync[1]);
	forward_pid = pid;
	install_forwarders();
	if (cfg.rootless)
		write_id_maps(pid);
	if (!joined)
		cgroup_add(&cg, pid);
	if (cfg.net == NET_BRIDGE)
		net_host_setup(&cfg, pid);
	state_save(&cfg, pid, &cg);
	if (write(sync[0], "g", 1) != 1)
		die("release the container");
	close(sync[0]);
	cleanup_armed = 0;
	if (ready_fd >= 0) {
		if (write(ready_fd, "o", 1) != 1)
			warnf("report startup");
		close(ready_fd);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			die("waitpid");
	}
	forward_pid = 0;
	code = exit_status_code(status);
	net_host_teardown(&cfg);
	cgroup_destroy(&cg);
	if (cfg.detach)
		state_mark_exited(&cfg, code);
	else
		state_remove(&cfg);
	return code;
}
