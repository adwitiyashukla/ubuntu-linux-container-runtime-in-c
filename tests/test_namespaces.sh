#!/bin/bash
. "$(dirname "$0")/lib.sh"

host_hostname=$(hostname)
host_ifaces=$(ls /sys/class/net | wc -l)

expect "command runs as pid 2 behind the init" "$(ctr "$ROOTFS" sh -c 'echo $$')" 2
expect "pid 1 inside is the lcr init" "$(ctr "$ROOTFS" cat /proc/1/comm)" lcr
expect "the init has no visible parent" "$(ctr "$ROOTFS" sh -c 'cut -d" " -f4 /proc/1/stat')" 0
expect "only the container processes are visible" "$(ctr "$ROOTFS" sh -c 'ls /proc > /tmp/p; grep -c "^[0-9]" /tmp/p')" 3
expect "hostname is set inside" "$(ctr --hostname box "$ROOTFS" hostname)" box
expect "hostname defaults to the name" "$(ctr --name named "$ROOTFS" hostname)" named
expect "host hostname is untouched" "$(hostname)" "$host_hostname"
expect "only loopback exists with --net none" "$(ctr "$ROOTFS" ls /sys/class/net)" lo
expect "loopback is up" "$(ctr "$ROOTFS" sh -c 'ip link show lo | grep -c UP')" 1
expect "--net host sees the host interfaces" "$(ctr --net host "$ROOTFS" ls /sys/class/net | wc -l)" "$host_ifaces"
expect "ipc namespace starts empty" "$(ctr "$ROOTFS" sh -c 'wc -l < /proc/sysvipc/shm')" 1
expect "cgroup namespace puts the container at the root" "$(ctr "$ROOTFS" grep '^0::' /proc/self/cgroup)" "0::/"

id=$(ctr -d --privileged "$ROOTFS" sh -c 'mkdir -p /mnt && mount -t tmpfs none /mnt && echo ready && sleep 30')
wait_until 100 ready "$id"
expect "a mount made inside is visible inside" "$("$LCR" exec "$id" grep -c ' /mnt tmpfs' /proc/mounts)" 1
expect "the same mount is invisible on the host" "$(grep -c "$WORK/state/$id/rootfs/mnt" /proc/mounts)" 0
"$LCR" rm -f "$id"

if [ "$MODE" = rootless ]; then
	expect "uid 0 inside maps to the real user" "$(ctr "$ROOTFS" sh -c 'id -u; awk "{print \$2}" /proc/self/uid_map')" $'0\n'"$(id -u)"
	expect "the user namespace has one mapping" "$(ctr "$ROOTFS" sh -c 'wc -l < /proc/self/uid_map')" 1
else
	expect "no user namespace is used as root" "$(ctr "$ROOTFS" sh -c 'awk "{print \$3}" /proc/self/uid_map')" 4294967295
fi

finish
