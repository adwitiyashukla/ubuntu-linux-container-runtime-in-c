#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "lcr.h"

#define X32_SYSCALL_BIT 0x40000000

static const int denied[] = {
#ifdef __NR_acct
	__NR_acct,
#endif
#ifdef __NR_add_key
	__NR_add_key,
#endif
#ifdef __NR_bpf
	__NR_bpf,
#endif
#ifdef __NR_clock_adjtime
	__NR_clock_adjtime,
#endif
#ifdef __NR_clock_settime
	__NR_clock_settime,
#endif
#ifdef __NR_create_module
	__NR_create_module,
#endif
#ifdef __NR_delete_module
	__NR_delete_module,
#endif
#ifdef __NR_finit_module
	__NR_finit_module,
#endif
#ifdef __NR_fsconfig
	__NR_fsconfig,
#endif
#ifdef __NR_fsmount
	__NR_fsmount,
#endif
#ifdef __NR_fsopen
	__NR_fsopen,
#endif
#ifdef __NR_fspick
	__NR_fspick,
#endif
#ifdef __NR_get_kernel_syms
	__NR_get_kernel_syms,
#endif
#ifdef __NR_get_mempolicy
	__NR_get_mempolicy,
#endif
#ifdef __NR_init_module
	__NR_init_module,
#endif
#ifdef __NR_io_uring_enter
	__NR_io_uring_enter,
#endif
#ifdef __NR_io_uring_register
	__NR_io_uring_register,
#endif
#ifdef __NR_io_uring_setup
	__NR_io_uring_setup,
#endif
#ifdef __NR_ioperm
	__NR_ioperm,
#endif
#ifdef __NR_iopl
	__NR_iopl,
#endif
#ifdef __NR_kcmp
	__NR_kcmp,
#endif
#ifdef __NR_kexec_file_load
	__NR_kexec_file_load,
#endif
#ifdef __NR_kexec_load
	__NR_kexec_load,
#endif
#ifdef __NR_keyctl
	__NR_keyctl,
#endif
#ifdef __NR_lookup_dcookie
	__NR_lookup_dcookie,
#endif
#ifdef __NR_mbind
	__NR_mbind,
#endif
#ifdef __NR_mount
	__NR_mount,
#endif
#ifdef __NR_mount_setattr
	__NR_mount_setattr,
#endif
#ifdef __NR_move_mount
	__NR_move_mount,
#endif
#ifdef __NR_move_pages
	__NR_move_pages,
#endif
#ifdef __NR_name_to_handle_at
	__NR_name_to_handle_at,
#endif
#ifdef __NR_nfsservctl
	__NR_nfsservctl,
#endif
#ifdef __NR_open_by_handle_at
	__NR_open_by_handle_at,
#endif
#ifdef __NR_open_tree
	__NR_open_tree,
#endif
#ifdef __NR_perf_event_open
	__NR_perf_event_open,
#endif
#ifdef __NR_pivot_root
	__NR_pivot_root,
#endif
#ifdef __NR_process_vm_readv
	__NR_process_vm_readv,
#endif
#ifdef __NR_process_vm_writev
	__NR_process_vm_writev,
#endif
#ifdef __NR_query_module
	__NR_query_module,
#endif
#ifdef __NR_quotactl
	__NR_quotactl,
#endif
#ifdef __NR_quotactl_fd
	__NR_quotactl_fd,
#endif
#ifdef __NR_reboot
	__NR_reboot,
#endif
#ifdef __NR_request_key
	__NR_request_key,
#endif
#ifdef __NR_set_mempolicy
	__NR_set_mempolicy,
#endif
#ifdef __NR_setns
	__NR_setns,
#endif
#ifdef __NR_settimeofday
	__NR_settimeofday,
#endif
#ifdef __NR_swapoff
	__NR_swapoff,
#endif
#ifdef __NR_swapon
	__NR_swapon,
#endif
#ifdef __NR__sysctl
	__NR__sysctl,
#endif
#ifdef __NR_sysfs
	__NR_sysfs,
#endif
#ifdef __NR_umount2
	__NR_umount2,
#endif
#ifdef __NR_unshare
	__NR_unshare,
#endif
#ifdef __NR_uselib
	__NR_uselib,
#endif
#ifdef __NR_userfaultfd
	__NR_userfaultfd,
#endif
#ifdef __NR_ustat
	__NR_ustat,
#endif
#ifdef __NR_vm86
	__NR_vm86,
#endif
#ifdef __NR_vm86old
	__NR_vm86old,
#endif
};

#define NDENIED ((int)(sizeof(denied) / sizeof(denied[0])))

void seccomp_apply(void)
{
	struct sock_filter prog[6 + 2 * NDENIED + 1];
	struct sock_fprog fprog;
	int n = 0;
	int i;

	prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
	prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
	prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
	prog[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
	prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, X32_SYSCALL_BIT, 0, 1);
	prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
	for (i = 0; i < NDENIED; i++) {
		prog[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)denied[i], 0, 1);
		prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
	}
	prog[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

	fprog.len = (unsigned short)n;
	fprog.filter = prog;
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
		die("set no_new_privs");
	if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) < 0)
		die("install seccomp filter");
}
