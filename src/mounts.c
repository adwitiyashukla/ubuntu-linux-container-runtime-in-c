#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "lcr.h"

static void mount_or_die(const char *src, const char *dst, const char *type, unsigned long flags, const char *data)
{
	if (mount(src, dst, type, flags, data) < 0)
		die("mount %s on %s", type ? type : src, dst);
}

static void ensure_dir(const char *path)
{
	if (mkdir_p(path, 0755) < 0)
		die("mkdir %s", path);
}

static void ensure_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

	if (fd < 0)
		die("create %s", path);
	close(fd);
}

static unsigned long flags_of(const char *path)
{
	struct statvfs sv;
	unsigned long flags = 0;

	if (statvfs(path, &sv) < 0)
		return 0;
	if (sv.f_flag & ST_RDONLY)
		flags |= MS_RDONLY;
	if (sv.f_flag & ST_NOSUID)
		flags |= MS_NOSUID;
	if (sv.f_flag & ST_NODEV)
		flags |= MS_NODEV;
	if (sv.f_flag & ST_NOEXEC)
		flags |= MS_NOEXEC;
	if (sv.f_flag & ST_NOATIME)
		flags |= MS_NOATIME;
	if (sv.f_flag & ST_NODIRATIME)
		flags |= MS_NODIRATIME;
	if (sv.f_flag & ST_RELATIME)
		flags |= MS_RELATIME;
	if (sv.f_flag & ST_SYNCHRONOUS)
		flags |= MS_SYNCHRONOUS;
	if (sv.f_flag & ST_MANDLOCK)
		flags |= MS_MANDLOCK;
	return flags;
}

static void remount_ro(const char *path)
{
	unsigned long flags = flags_of(path) | MS_BIND | MS_REMOUNT | MS_RDONLY;

	if (mount(NULL, path, NULL, flags, NULL) < 0)
		die("remount %s read-only", path);
}

static void bind_mount(const char *src, const char *dst, int ro)
{
	struct stat st;

	if (stat(src, &st) < 0)
		die("bind mount source %s", src);
	if (S_ISDIR(st.st_mode)) {
		ensure_dir(dst);
	} else {
		char parent[PATH_MAX];
		char *slash;

		snprintf(parent, sizeof(parent), "%s", dst);
		slash = strrchr(parent, '/');
		if (slash && slash != parent) {
			*slash = '\0';
			ensure_dir(parent);
		}
		ensure_file(dst);
	}
	mount_or_die(src, dst, NULL, MS_BIND | MS_REC, NULL);
	if (ro)
		remount_ro(dst);
}

static void mount_rootfs(const struct config *cfg)
{
	char upper[PATH_MAX];
	char work[PATH_MAX];
	char opts[3 * PATH_MAX + 64];

	ensure_dir(cfg->rootfs);
	if (cfg->rw_image) {
		mount_or_die(cfg->image, cfg->rootfs, NULL, MS_BIND | MS_REC, NULL);
		return;
	}
	join_path(upper, sizeof(upper), cfg->state_dir, "upper");
	join_path(work, sizeof(work), cfg->state_dir, "work");
	ensure_dir(upper);
	ensure_dir(work);
	snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s%s",
		 cfg->image, upper, work, cfg->rootless ? ",userxattr" : "");
	if (mount("overlay", cfg->rootfs, "overlay", 0, opts) < 0)
		die("mount overlayfs over %s (use --rw-image to run the image directory directly)", cfg->image);
}

static void mount_proc_sys(const struct config *cfg, enum cg_mode cg)
{
	char p[PATH_MAX];

	join_path(p, sizeof(p), cfg->rootfs, "proc");
	ensure_dir(p);
	mount_or_die("proc", p, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

	join_path(p, sizeof(p), cfg->rootfs, "sys");
	ensure_dir(p);
	if (mount("sysfs", p, "sysfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
		if (errno != EPERM)
			die("mount sysfs on %s", p);
		bind_mount("/sys", p, 1);
		return;
	}
	if (cg == CG_V2) {
		join_path(p, sizeof(p), cfg->rootfs, "sys/fs/cgroup");
		mount("cgroup2", p, "cgroup2", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	}
}

static void mount_dev(const struct config *cfg)
{
	static const char *const nodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
	static const struct {
		const char *name;
		const char *target;
	} links[] = {
		{ "ptmx", "pts/ptmx" },
		{ "fd", "/proc/self/fd" },
		{ "stdin", "/proc/self/fd/0" },
		{ "stdout", "/proc/self/fd/1" },
		{ "stderr", "/proc/self/fd/2" },
	};
	char dev[PATH_MAX];
	char p[PATH_MAX];
	char src[64];
	size_t i;

	join_path(dev, sizeof(dev), cfg->rootfs, "dev");
	ensure_dir(dev);
	mount_or_die("tmpfs", dev, "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=65536k");
	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
		snprintf(src, sizeof(src), "/dev/%s", nodes[i]);
		join_path(p, sizeof(p), dev, nodes[i]);
		ensure_file(p);
		mount_or_die(src, p, NULL, MS_BIND, NULL);
	}
	join_path(p, sizeof(p), dev, "pts");
	ensure_dir(p);
	mount_or_die("devpts", p, "devpts", MS_NOSUID | MS_NOEXEC,
		     cfg->rootless ? "newinstance,ptmxmode=0666,mode=0620"
				   : "newinstance,ptmxmode=0666,mode=0620,gid=5");
	join_path(p, sizeof(p), dev, "shm");
	ensure_dir(p);
	mount_or_die("tmpfs", p, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777,size=65536k");
	join_path(p, sizeof(p), dev, "mqueue");
	ensure_dir(p);
	mount("mqueue", p, "mqueue", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

	for (i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
		join_path(p, sizeof(p), dev, links[i].name);
		if (symlink(links[i].target, p) < 0)
			die("symlink %s", p);
	}
}

static void mount_binds(const struct config *cfg)
{
	char dst[PATH_MAX];
	int i;

	for (i = 0; i < cfg->nbinds; i++) {
		join_path(dst, sizeof(dst), cfg->rootfs, cfg->binds[i].dst);
		bind_mount(cfg->binds[i].src, dst, cfg->binds[i].ro);
	}
}

static void write_resolv(const char *path, int skip_loopback)
{
	char buf[4096];
	char out[4608];
	const char *src = "/etc/resolv.conf";
	char *line;
	size_t off = 0;
	int servers = 0;

	if (skip_loopback && access("/run/systemd/resolve/resolv.conf", R_OK) == 0)
		src = "/run/systemd/resolve/resolv.conf";
	if (read_file(src, buf, sizeof(buf)) < 0)
		buf[0] = '\0';
	line = buf;
	while (line && *line) {
		char *next = strchr(line, '\n');
		size_t len;

		if (next)
			*next++ = '\0';
		len = strlen(line);
		if (!(skip_loopback && !strncmp(line, "nameserver 127.", 15)) && off + len + 1 < sizeof(out)) {
			if (!strncmp(line, "nameserver", 10))
				servers++;
			memcpy(out + off, line, len);
			off += len;
			out[off++] = '\n';
		}
		line = next;
	}
	out[off] = '\0';
	if (!servers)
		snprintf(out + off, sizeof(out) - off, "nameserver 8.8.8.8\nnameserver 1.1.1.1\n");
	if (write_file_create(path, out, 0644) < 0)
		die("write %s", path);
}

static void bind_etc_file(const struct config *cfg, const char *name, const char *content)
{
	char src[PATH_MAX];
	char dst[PATH_MAX];

	join_path(src, sizeof(src), cfg->state_dir, name);
	if (content && write_file_create(src, content, 0644) < 0)
		die("write %s", src);
	snprintf(dst, sizeof(dst), "%s/etc/%s", cfg->rootfs, name);
	ensure_file(dst);
	mount_or_die(src, dst, NULL, MS_BIND, NULL);
}

static void setup_etc(const struct config *cfg)
{
	char etc[PATH_MAX];
	char hosts[512];
	char hostname[80];
	char resolv[PATH_MAX];
	struct stat st;

	join_path(etc, sizeof(etc), cfg->rootfs, "etc");
	if (stat(etc, &st) < 0 || !S_ISDIR(st.st_mode))
		return;
	snprintf(hostname, sizeof(hostname), "%s\n", cfg->hostname);
	bind_etc_file(cfg, "hostname", hostname);
	snprintf(hosts, sizeof(hosts),
		 "127.0.0.1\tlocalhost\n::1\tlocalhost ip6-localhost ip6-loopback\n%s\t%s\n",
		 cfg->ip[0] ? cfg->ip : "127.0.1.1", cfg->hostname);
	bind_etc_file(cfg, "hosts", hosts);
	if (cfg->net == NET_NONE)
		return;
	join_path(resolv, sizeof(resolv), cfg->state_dir, "resolv.conf");
	write_resolv(resolv, cfg->net == NET_BRIDGE);
	bind_etc_file(cfg, "resolv.conf", NULL);
}

static void pivot(const char *root)
{
	if (chdir(root) < 0)
		die("chdir %s", root);
	if (syscall(SYS_pivot_root, ".", ".") < 0)
		die("pivot_root into %s", root);
	if (umount2(".", MNT_DETACH) < 0)
		die("detach the old root");
	if (chdir("/") < 0)
		die("chdir /");
}

static void mask_paths(void)
{
	static const char *const masked[] = {
		"/proc/acpi", "/proc/asound", "/proc/kcore", "/proc/keys", "/proc/latency_stats",
		"/proc/timer_list", "/proc/timer_stats", "/proc/sched_debug", "/proc/scsi", "/sys/firmware",
	};
	static const char *const readonly[] = {
		"/proc/bus", "/proc/fs", "/proc/irq", "/proc/sys", "/proc/sysrq-trigger",
	};
	struct stat st;
	size_t i;

	for (i = 0; i < sizeof(masked) / sizeof(masked[0]); i++) {
		if (stat(masked[i], &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode))
			mount("tmpfs", masked[i], "tmpfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "size=4k,mode=755");
		else
			mount("/dev/null", masked[i], NULL, MS_BIND, NULL);
	}
	for (i = 0; i < sizeof(readonly) / sizeof(readonly[0]); i++) {
		if (stat(readonly[i], &st) < 0)
			continue;
		if (mount(readonly[i], readonly[i], NULL, MS_BIND | MS_REC, NULL) < 0)
			die("bind %s", readonly[i]);
		remount_ro(readonly[i]);
	}
}

void mounts_setup(const struct config *cfg, enum cg_mode cg)
{
	if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		die("make mount propagation private");
	mount_rootfs(cfg);
	mount_proc_sys(cfg, cg);
	mount_dev(cfg);
	mount_binds(cfg);
	setup_etc(cfg);
	pivot(cfg->rootfs);
	if (!cfg->privileged)
		mask_paths();
	if (cfg->read_only)
		remount_ro("/");
}
