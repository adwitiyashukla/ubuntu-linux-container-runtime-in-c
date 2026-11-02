#!/bin/bash
. "$(dirname "$0")/lib.sh"

vol=$WORK/vol
mkdir -p "$vol"
echo data > "$vol/f"

expect "root is an overlay" "$(ctr "$ROOTFS" sh -c 'awk "\$2 == \"/\" {print \$3}" /proc/mounts')" overlay
expect "writes go to the upper layer" "$(ctr "$ROOTFS" sh -c 'echo hi > /marker && cat /marker')" hi
expect "the image directory is untouched" "$(test -e "$ROOTFS/marker" && echo present || echo absent)" absent
ctr --rw-image "$ROOTFS" sh -c 'echo hi > /marker'
expect "--rw-image writes land in the image" "$(test -e "$ROOTFS/marker" && echo present || echo absent)" present
rm -f "$ROOTFS/marker"
expect "--read-only refuses writes" "$(ctr --read-only "$ROOTFS" sh -c 'touch /x 2>/dev/null && echo wrote || echo refused')" refused
expect "proc is mounted" "$(ctr "$ROOTFS" sh -c 'awk "\$2 == \"/proc\" {print \$3}" /proc/mounts')" proc
expect "sysfs is read-only" "$(ctr "$ROOTFS" sh -c 'awk "\$2 == \"/sys\" {print \$4}" /proc/mounts | cut -d, -f1')" ro
expect "dev has the standard nodes" "$(ctr "$ROOTFS" sh -c 'cd /dev && ls -d fd full null ptmx pts random shm tty urandom zero | tr "\n" " "')" "fd full null ptmx pts random shm tty urandom zero "
expect "dev/null accepts writes" "$(ctr "$ROOTFS" sh -c 'echo x > /dev/null && echo ok')" ok
expect "dev/urandom produces data" "$(ctr "$ROOTFS" sh -c 'head -c 16 /dev/urandom | wc -c')" 16
expect "devpts is a private instance" "$(ctr "$ROOTFS" sh -c 'ls /dev/pts')" ptmx
expect "dev/shm is a tmpfs" "$(ctr "$ROOTFS" sh -c 'awk "\$2 == \"/dev/shm\" {print \$3}" /proc/mounts')" tmpfs
expect "/proc/sys is read-only" "$(ctr "$ROOTFS" sh -c '(echo x > /proc/sys/kernel/hostname) 2>/dev/null && echo wrote || echo refused')" refused
expect "/sys/firmware is masked" "$(ctr "$ROOTFS" sh -c 'ls /sys/firmware | wc -l')" 0
expect "a volume is readable" "$(ctr -v "$vol:/data" "$ROOTFS" cat /data/f)" data
expect "a volume is writable by default" "$(ctr -v "$vol:/data" "$ROOTFS" sh -c 'echo w > /data/w && echo ok')" ok
expect "the write reached the host directory" "$(cat "$vol/w")" w
expect "a read-only volume refuses writes" "$(ctr -v "$vol:/data:ro" "$ROOTFS" sh -c 'touch /data/y 2>/dev/null && echo wrote || echo refused')" refused
expect "a file can be bind mounted" "$(ctr -v "$vol/f:/etc/f" "$ROOTFS" cat /etc/f)" data
expect "/etc/hostname is written" "$(ctr --hostname box "$ROOTFS" cat /etc/hostname)" box
expect "/etc/hosts knows the hostname" "$(ctr --hostname box "$ROOTFS" grep -c box /etc/hosts)" 1
expect "no resolv.conf without a network" "$(ctr "$ROOTFS" sh -c 'test -e /etc/resolv.conf && echo present || echo absent')" absent
expect "--net host copies the host resolver" "$(ctr --net host "$ROOTFS" grep -c nameserver /etc/resolv.conf)" "$(grep -c nameserver /etc/resolv.conf)"
expect "workdir is honored" "$(ctr -w /tmp "$ROOTFS" pwd)" /tmp
expect "state directory is removed after a foreground run" "$(ls "$LCR_ROOT" | wc -l)" 0

finish
