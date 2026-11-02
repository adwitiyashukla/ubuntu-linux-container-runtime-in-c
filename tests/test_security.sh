#!/bin/bash
. "$(dirname "$0")/lib.sh"

capbnd() {
	ctr "$@" "$ROOTFS" grep CapBnd /proc/self/status | awk '{print $2}'
}

expect "default bounding set matches the docker default" "$(capbnd)" 00000000a80425fb
expect "--cap-drop NET_RAW clears bit 13" "$(capbnd --cap-drop NET_RAW)" 00000000a80405fb
expect "--cap-add NET_ADMIN sets bit 12" "$(capbnd --cap-add NET_ADMIN)" 00000000a80435fb
expect "the CAP_ prefix is accepted" "$(capbnd --cap-add CAP_SYS_PTRACE)" 00000000a80c25fb
expect "no_new_privs is set" "$(ctr "$ROOTFS" grep NoNewPrivs /proc/self/status | awk '{print $2}')" 1
expect "effective caps equal the bounding set for root" "$(ctr "$ROOTFS" grep CapEff /proc/self/status | awk '{print $2}')" 00000000a80425fb
expect "sethostname is refused without SYS_ADMIN" "$(ctr "$ROOTFS" probe syscall sethostname)" EPERM
expect "sethostname works with SYS_ADMIN" "$(ctr --cap-add SYS_ADMIN "$ROOTFS" probe syscall sethostname)" ok
expect "seccomp blocks add_key" "$(ctr "$ROOTFS" probe syscall add_key)" EPERM
expect "seccomp blocks keyctl" "$(ctr "$ROOTFS" probe syscall keyctl)" EPERM
expect "--seccomp none allows add_key" "$(ctr --seccomp none "$ROOTFS" probe syscall add_key)" ok
expect "seccomp blocks unshare even with SYS_ADMIN" "$(ctr --cap-add SYS_ADMIN "$ROOTFS" probe syscall unshare)" EPERM
expect "unshare works with SYS_ADMIN once seccomp is off" "$(ctr --cap-add SYS_ADMIN --seccomp none "$ROOTFS" probe syscall unshare)" ok
expect "mount is refused by default" "$(ctr "$ROOTFS" probe syscall mount)" EPERM
expect "--privileged allows mount" "$(ctr --privileged "$ROOTFS" probe syscall mount)" ok
expect "--privileged turns seccomp off" "$(ctr --privileged "$ROOTFS" grep Seccomp: /proc/self/status | awk '{print $2}')" 0
expect "the default filter is seccomp mode 2" "$(ctr "$ROOTFS" grep Seccomp: /proc/self/status | awk '{print $2}')" 2
expect "exec of a missing command exits 127" "$(ctr "$ROOTFS" nosuchcommand 2>/dev/null; echo $?)" 127
expect "exec of a non-executable exits 126" "$(ctr "$ROOTFS" /etc/passwd 2>/dev/null; echo $?)" 126
expect "an unknown capability is rejected" "$(ctr --cap-add NOPE "$ROOTFS" true 2>&1 | grep -c 'unknown capability')" 1

if [ "$MODE" = root ]; then
	expect "-u runs the command as that uid" "$(ctr -u 1000:1000 "$ROOTFS" id -u)" 1000
	expect "-u sets the gid" "$(ctr -u 1000:1000 "$ROOTFS" id -g)" 1000
	expect "-u clears the effective capabilities" "$(ctr -u 1000:1000 "$ROOTFS" grep CapEff /proc/self/status | awk '{print $2}')" 0000000000000000
	expect "a non-root user cannot read a root-only file" "$(ctr -u 1000:1000 "$ROOTFS" sh -c 'cat /proc/1/environ >/dev/null 2>&1 && echo read || echo refused')" refused
fi

finish
