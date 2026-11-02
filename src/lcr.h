#ifndef LCR_H
#define LCR_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#define LCR_VERSION "0.1.0"
#define MAX_BINDS 32
#define MAX_PORTS 32
#define MAX_ENV 64
#define BRIDGE_NAME "lcr0"
#define BRIDGE_ADDR "10.77.0.1"
#define BRIDGE_PREFIX 24
#define BRIDGE_SUBNET "10.77.0.0/24"
#define CGROUP_PARENT "lcr"

enum net_mode { NET_NONE, NET_HOST, NET_BRIDGE };
enum cg_mode { CG_NONE, CG_V1, CG_V2 };

struct bind_mount {
	char src[PATH_MAX];
	char dst[PATH_MAX];
	int ro;
};

struct port_map {
	int host;
	int ctr;
};

struct config {
	char id[32];
	char name[64];
	char hostname[65];
	char image[PATH_MAX];
	char state_dir[PATH_MAX];
	char rootfs[PATH_MAX];
	char workdir[PATH_MAX];
	char ip[32];
	enum net_mode net;
	struct bind_mount binds[MAX_BINDS];
	int nbinds;
	struct port_map ports[MAX_PORTS];
	int nports;
	char *env[MAX_ENV];
	int nenv;
	uid_t uid;
	gid_t gid;
	int have_user;
	long long memory;
	long cpu_quota;
	long cpu_period;
	long pids;
	uint64_t caps;
	int seccomp;
	int privileged;
	int rootless;
	int read_only;
	int rw_image;
	int detach;
	char **argv;
	int argc;
};

struct cgroup {
	enum cg_mode mode;
	char path[PATH_MAX];
	char rel[PATH_MAX];
	int fd;
};

struct state {
	char dir[PATH_MAX];
	char id[32];
	char name[64];
	char hostname[65];
	char image[PATH_MAX];
	char ip[32];
	char cmd[1024];
	char workdir[PATH_MAX];
	char cgroup[PATH_MAX];
	enum cg_mode cgroup_mode;
	enum net_mode net;
	uint64_t caps;
	int seccomp;
	int privileged;
	int rootless;
	int nports;
	pid_t pid;
	unsigned long long start;
	long created;
	int exited;
	int exit_code;
};

void die(const char *fmt, ...);
void fail(const char *fmt, ...);
void warnf(const char *fmt, ...);
int write_file(const char *path, const char *data);
int write_file_create(const char *path, const char *data, mode_t mode);
ssize_t read_file(const char *path, char *buf, size_t len);
int mkdir_p(const char *path, mode_t mode);
void join_path(char *out, size_t n, const char *a, const char *b);
long long parse_size(const char *s);
int parse_cpus(const char *s, long *quota, long *period);
int rm_rf(const char *path);
int proc_start_time(pid_t pid, unsigned long long *start);
int run_cmd_stdin(const char *const argv[], const char *input, int quiet);
ssize_t run_cmd_output(const char *const argv[], char *buf, size_t len);

const char *state_root(void);
void state_save(const struct config *cfg, pid_t pid, const struct cgroup *cg);
void state_mark_exited(const struct config *cfg, int code);
void state_remove(const struct config *cfg);
int state_load(const char *dir, struct state *st);
int state_resolve(const char *ref, struct state *st);
int state_alive(const struct state *st);
int cmd_ps(int argc, char **argv);
int cmd_exec(int argc, char **argv);
int cmd_kill(int argc, char **argv);
int cmd_stats(int argc, char **argv);
int cmd_logs(int argc, char **argv);
int cmd_rm(int argc, char **argv);

int cmd_run(int argc, char **argv);
void setup_env(const struct config *cfg);
void drop_to_user(uid_t uid, gid_t gid);
int exit_status_code(int status);

void mounts_setup(const struct config *cfg, enum cg_mode cg);

enum cg_mode cgroup_detect(void);
void cgroup_create(const struct config *cfg, struct cgroup *cg);
void cgroup_add(const struct cgroup *cg, pid_t pid);
void cgroup_destroy(struct cgroup *cg);
void cgroup_open_existing(struct cgroup *cg, enum cg_mode mode, const char *path);
int cgroup_print_stats(const struct cgroup *cg);

void net_host_setup(struct config *cfg, pid_t pid);
void net_host_teardown(const struct config *cfg);
void net_container_setup(const struct config *cfg);
void net_alloc_ip(struct config *cfg);

void seccomp_apply(void);

uint64_t caps_default(void);
uint64_t caps_all(void);
int caps_lookup(const char *name);
void caps_apply(uint64_t keep);

#endif
