#include <ctype.h>
#include <errno.h>
#include <linux/capability.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "lcr.h"

static const char *const cap_names[] = {
	"CHOWN", "DAC_OVERRIDE", "DAC_READ_SEARCH", "FOWNER", "FSETID",
	"KILL", "SETGID", "SETUID", "SETPCAP", "LINUX_IMMUTABLE",
	"NET_BIND_SERVICE", "NET_BROADCAST", "NET_ADMIN", "NET_RAW", "IPC_LOCK",
	"IPC_OWNER", "SYS_MODULE", "SYS_RAWIO", "SYS_CHROOT", "SYS_PTRACE",
	"SYS_PACCT", "SYS_ADMIN", "SYS_BOOT", "SYS_NICE", "SYS_RESOURCE",
	"SYS_TIME", "SYS_TTY_CONFIG", "MKNOD", "LEASE", "AUDIT_WRITE",
	"AUDIT_CONTROL", "SETFCAP", "MAC_OVERRIDE", "MAC_ADMIN", "SYSLOG",
	"WAKE_ALARM", "BLOCK_SUSPEND", "AUDIT_READ", "PERFMON", "BPF",
	"CHECKPOINT_RESTORE",
};

#define NCAPS ((int)(sizeof(cap_names) / sizeof(cap_names[0])))

static int last_cap(void)
{
	char buf[16];

	if (read_file("/proc/sys/kernel/cap_last_cap", buf, sizeof(buf)) > 0)
		return atoi(buf);
	return NCAPS - 1;
}

uint64_t caps_default(void)
{
	static const char *const keep[] = {
		"CHOWN", "DAC_OVERRIDE", "FSETID", "FOWNER", "MKNOD", "NET_RAW",
		"SETGID", "SETUID", "SETFCAP", "SETPCAP", "NET_BIND_SERVICE",
		"SYS_CHROOT", "KILL", "AUDIT_WRITE",
	};
	uint64_t mask = 0;
	size_t i;

	for (i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
		mask |= 1ULL << caps_lookup(keep[i]);
	return mask;
}

uint64_t caps_all(void)
{
	int last = last_cap();

	if (last >= 63)
		return ~0ULL;
	return (1ULL << (last + 1)) - 1;
}

int caps_lookup(const char *name)
{
	int i;

	if (!strncasecmp(name, "CAP_", 4))
		name += 4;
	for (i = 0; i < NCAPS; i++) {
		if (!strcasecmp(name, cap_names[i]))
			return i;
	}
	return -1;
}

void caps_apply(uint64_t keep)
{
	struct __user_cap_header_struct hdr;
	struct __user_cap_data_struct data[2];
	int last = last_cap();
	int cap;

	for (cap = 0; cap <= last; cap++) {
		if (keep & (1ULL << cap))
			continue;
		if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL)
			die("drop capability %d from bounding set", cap);
	}
	if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0)
		die("clear ambient capabilities");
	memset(&hdr, 0, sizeof(hdr));
	memset(data, 0, sizeof(data));
	hdr.version = _LINUX_CAPABILITY_VERSION_3;
	hdr.pid = 0;
	data[0].permitted = (uint32_t)(keep & 0xffffffffULL);
	data[0].effective = data[0].permitted;
	data[1].permitted = (uint32_t)(keep >> 32);
	data[1].effective = data[1].permitted;
	if (syscall(SYS_capset, &hdr, data) < 0)
		die("set process capabilities");
}
