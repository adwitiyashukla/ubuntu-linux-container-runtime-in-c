#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lcr.h"

#define CG_ROOT "/sys/fs/cgroup"

static const char *const v1_controllers[] = { "memory", "cpu", "cpuacct", "pids" };
#define NV1 ((int)(sizeof(v1_controllers) / sizeof(v1_controllers[0])))

enum cg_mode cgroup_detect(void)
{
	int i;

	if (access(CG_ROOT "/cgroup.controllers", F_OK) == 0)
		return CG_V2;
	for (i = 0; i < NV1; i++) {
		char p[PATH_MAX];
		snprintf(p, sizeof(p), CG_ROOT "/%s", v1_controllers[i]);
		if (access(p, F_OK) == 0)
			return CG_V1;
	}
	return CG_NONE;
}

static int has_word(const char *list, const char *word)
{
	size_t n = strlen(word);
	const char *p = list;

	while ((p = strstr(p, word)) != NULL) {
		int start_ok = (p == list || p[-1] == ' ' || p[-1] == '\n');
		int end_ok = (p[n] == '\0' || p[n] == ' ' || p[n] == '\n');
		if (start_ok && end_ok)
			return 1;
		p += n;
	}
	return 0;
}

static void write_limit(const char *dir, const char *file, const char *value)
{
	char p[PATH_MAX];

	join_path(p, sizeof(p), dir, file);
	if (write_file(p, value) < 0)
		die("write %s to %s", value, p);
}

static void v2_create(const struct config *cfg, struct cgroup *cg)
{
	char controllers[512];
	char enable[64] = "";
	char parent[PATH_MAX];
	char buf[64];

	if (read_file(CG_ROOT "/cgroup.controllers", controllers, sizeof(controllers)) < 0)
		die("read cgroup.controllers");
	if (has_word(controllers, "cpu"))
		strcat(enable, "+cpu ");
	else if (cfg->cpu_quota)
		fail("cpu controller is not available in cgroup v2");
	if (has_word(controllers, "memory"))
		strcat(enable, "+memory ");
	else if (cfg->memory)
		fail("memory controller is not available in cgroup v2");
	if (has_word(controllers, "pids"))
		strcat(enable, "+pids ");
	else if (cfg->pids)
		fail("pids controller is not available in cgroup v2");

	snprintf(parent, sizeof(parent), CG_ROOT "/%s", CGROUP_PARENT);
	if (mkdir(parent, 0755) < 0 && errno != EEXIST)
		die("mkdir %s", parent);
	if (enable[0]) {
		write_file(CG_ROOT "/cgroup.subtree_control", enable);
		write_limit(parent, "cgroup.subtree_control", enable);
	}
	snprintf(cg->path, sizeof(cg->path), "%s/%s", parent, cfg->id);
	if (mkdir(cg->path, 0755) < 0)
		die("mkdir %s", cg->path);
	if (cfg->memory) {
		snprintf(buf, sizeof(buf), "%lld", cfg->memory);
		write_limit(cg->path, "memory.max", buf);
		snprintf(parent, sizeof(parent), "%s/memory.swap.max", cg->path);
		write_file(parent, "0");
	}
	if (cfg->cpu_quota) {
		snprintf(buf, sizeof(buf), "%ld %ld", cfg->cpu_quota, cfg->cpu_period);
		write_limit(cg->path, "cpu.max", buf);
	}
	if (cfg->pids) {
		snprintf(buf, sizeof(buf), "%ld", cfg->pids);
		write_limit(cg->path, "pids.max", buf);
	}
	cg->fd = open(cg->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cg->fd < 0)
		die("open %s", cg->path);
}

static int v1_dir(char *out, size_t n, const char *controller, const char *rel)
{
	snprintf(out, n, CG_ROOT "/%s/%s", controller, rel);
	return access(out, F_OK) == 0;
}

static void v1_create(const struct config *cfg, struct cgroup *cg)
{
	char base[PATH_MAX];
	char dir[PATH_MAX];
	char buf[64];
	int i;

	snprintf(cg->rel, sizeof(cg->rel), "%s/%s", CGROUP_PARENT, cfg->id);
	for (i = 0; i < NV1; i++) {
		const char *c = v1_controllers[i];
		int requested = (!strcmp(c, "memory") && cfg->memory) ||
				(!strcmp(c, "cpu") && cfg->cpu_quota) ||
				(!strcmp(c, "pids") && cfg->pids);

		snprintf(base, sizeof(base), CG_ROOT "/%s", c);
		if (access(base, F_OK) < 0) {
			if (requested)
				fail("%s controller is not available in cgroup v1", c);
			continue;
		}
		snprintf(dir, sizeof(dir), "%s/%s", base, cg->rel);
		if (mkdir_p(dir, 0755) < 0)
			die("mkdir %s", dir);
		if (!strcmp(c, "memory") && cfg->memory) {
			snprintf(buf, sizeof(buf), "%lld", cfg->memory);
			write_limit(dir, "memory.limit_in_bytes", buf);
			snprintf(base, sizeof(base), "%s/memory.memsw.limit_in_bytes", dir);
			write_file(base, buf);
		}
		if (!strcmp(c, "cpu") && cfg->cpu_quota) {
			snprintf(buf, sizeof(buf), "%ld", cfg->cpu_period);
			write_limit(dir, "cpu.cfs_period_us", buf);
			snprintf(buf, sizeof(buf), "%ld", cfg->cpu_quota);
			write_limit(dir, "cpu.cfs_quota_us", buf);
		}
		if (!strcmp(c, "pids") && cfg->pids) {
			snprintf(buf, sizeof(buf), "%ld", cfg->pids);
			write_limit(dir, "pids.max", buf);
		}
	}
}

void cgroup_create(const struct config *cfg, struct cgroup *cg)
{
	memset(cg, 0, sizeof(*cg));
	cg->fd = -1;
	cg->mode = cfg->rootless ? CG_NONE : cgroup_detect();
	if (cg->mode == CG_NONE) {
		if (cfg->memory || cfg->cpu_quota || cfg->pids)
			fail("resource limits need a cgroup hierarchy and root");
		return;
	}
	if (cg->mode == CG_V2)
		v2_create(cfg, cg);
	else
		v1_create(cfg, cg);
}

void cgroup_add(const struct cgroup *cg, pid_t pid)
{
	char buf[32];
	char dir[PATH_MAX];
	int i;

	snprintf(buf, sizeof(buf), "%d", pid);
	if (cg->mode == CG_V2) {
		write_limit(cg->path, "cgroup.procs", buf);
		return;
	}
	if (cg->mode != CG_V1)
		return;
	for (i = 0; i < NV1; i++) {
		if (!v1_dir(dir, sizeof(dir), v1_controllers[i], cg->rel))
			continue;
		write_limit(dir, "cgroup.procs", buf);
	}
}

static void rmdir_retry(const char *dir)
{
	struct timespec ts = { 0, 20 * 1000 * 1000 };
	int tries;

	for (tries = 0; tries < 150; tries++) {
		if (rmdir(dir) == 0 || errno == ENOENT)
			return;
		nanosleep(&ts, NULL);
	}
	warnf("remove cgroup %s", dir);
}

void cgroup_destroy(struct cgroup *cg)
{
	char dir[PATH_MAX];
	int i;

	if (cg->fd >= 0) {
		close(cg->fd);
		cg->fd = -1;
	}
	if (cg->mode == CG_V2) {
		snprintf(dir, sizeof(dir), "%s/cgroup.kill", cg->path);
		write_file(dir, "1");
		rmdir_retry(cg->path);
		return;
	}
	if (cg->mode != CG_V1)
		return;
	for (i = 0; i < NV1; i++) {
		if (v1_dir(dir, sizeof(dir), v1_controllers[i], cg->rel))
			rmdir_retry(dir);
	}
}

void cgroup_open_existing(struct cgroup *cg, enum cg_mode mode, const char *path)
{
	memset(cg, 0, sizeof(*cg));
	cg->fd = -1;
	cg->mode = mode;
	if (mode == CG_V2)
		snprintf(cg->path, sizeof(cg->path), "%s", path);
	else if (mode == CG_V1)
		snprintf(cg->rel, sizeof(cg->rel), "%s", path);
}

static long long read_number(const char *dir, const char *file)
{
	char p[PATH_MAX];
	char buf[128];

	snprintf(p, sizeof(p), "%s/%s", dir, file);
	if (read_file(p, buf, sizeof(buf)) <= 0)
		return -1;
	if (!strncmp(buf, "max", 3))
		return LLONG_MAX;
	return strtoll(buf, NULL, 10);
}

static long long read_keyed(const char *dir, const char *file, const char *key)
{
	char p[PATH_MAX];
	char buf[1024];
	char *line;

	snprintf(p, sizeof(p), "%s/%s", dir, file);
	if (read_file(p, buf, sizeof(buf)) <= 0)
		return -1;
	line = buf;
	while (line && *line) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == ' ')
			return strtoll(line + strlen(key) + 1, NULL, 10);
		line = next;
	}
	return -1;
}

static void print_bytes(const char *label, long long v)
{
	if (v < 0)
		return;
	if (v >= (1LL << 62))
		printf("%-16s unlimited\n", label);
	else
		printf("%-16s %.1f MiB\n", label, (double)v / 1048576.0);
}

static void print_count(const char *label, long long v)
{
	if (v < 0)
		return;
	if (v == LLONG_MAX)
		printf("%-16s unlimited\n", label);
	else
		printf("%-16s %lld\n", label, v);
}

int cgroup_print_stats(const struct cgroup *cg)
{
	char dir[PATH_MAX];
	long long cpu;

	if (cg->mode == CG_V2) {
		if (access(cg->path, F_OK) < 0)
			return -1;
		print_bytes("memory current", read_number(cg->path, "memory.current"));
		print_bytes("memory peak", read_number(cg->path, "memory.peak"));
		print_bytes("memory limit", read_number(cg->path, "memory.max"));
		cpu = read_keyed(cg->path, "cpu.stat", "usage_usec");
		if (cpu >= 0)
			printf("%-16s %.3f s\n", "cpu usage", (double)cpu / 1e6);
		print_count("pids current", read_number(cg->path, "pids.current"));
		print_count("pids limit", read_number(cg->path, "pids.max"));
		print_count("oom kills", read_keyed(cg->path, "memory.events", "oom_kill"));
		return 0;
	}
	if (cg->mode != CG_V1)
		return -1;
	if (v1_dir(dir, sizeof(dir), "memory", cg->rel)) {
		print_bytes("memory current", read_number(dir, "memory.usage_in_bytes"));
		print_bytes("memory peak", read_number(dir, "memory.max_usage_in_bytes"));
		print_bytes("memory limit", read_number(dir, "memory.limit_in_bytes"));
	}
	cpu = -1;
	if (v1_dir(dir, sizeof(dir), "cpuacct", cg->rel))
		cpu = read_number(dir, "cpuacct.usage");
	if (cpu < 0 && v1_dir(dir, sizeof(dir), "cpu", cg->rel))
		cpu = read_number(dir, "cpuacct.usage");
	if (cpu >= 0)
		printf("%-16s %.3f s\n", "cpu usage", (double)cpu / 1e9);
	if (v1_dir(dir, sizeof(dir), "pids", cg->rel)) {
		print_count("pids current", read_number(dir, "pids.current"));
		print_count("pids limit", read_number(dir, "pids.max"));
	}
	if (v1_dir(dir, sizeof(dir), "memory", cg->rel))
		print_count("oom kills", read_keyed(dir, "memory.oom_control", "oom_kill"));
	return 0;
}
