#!/bin/bash
. "$(dirname "$0")/lib.sh"

if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
	version=2
	parent=/sys/fs/cgroup/lcr
else
	version=1
	parent=/sys/fs/cgroup/memory/lcr
fi
echo "cgroup v$version host"

expect "64 MiB allocation under a 32m limit is killed" "$(ctr -m 32m "$ROOTFS" probe hog 64 >/dev/null 2>&1; echo $?)" 137
expect "16 MiB allocation under a 32m limit succeeds" "$(ctr -m 32m "$ROOTFS" probe hog 16)" "allocated 16 MiB"
expect_range "cpu time with --cpus 0.5 over 2 seconds" "$(ctr --cpus 0.5 "$ROOTFS" probe spin 2 | cut -d= -f2)" 0.6 1.25
expect_range "cpu time with --cpus 0.25 over 2 seconds" "$(ctr --cpus 0.25 "$ROOTFS" probe spin 2 | cut -d= -f2)" 0.3 0.65
expect "pids limit of 10 leaves room for 8 forks" "$(ctr --pids 10 "$ROOTFS" probe forks 20)" forked=8
expect "pids limit of 4 leaves room for 2 forks" "$(ctr --pids 4 "$ROOTFS" probe forks 20)" forked=2
expect "the cgroup is removed after the run" "$(ls "$parent" 2>/dev/null | grep -c '^[0-9a-f]\{12\}$')" 0

id=$(ctr -d -m 64m --cpus 1 --pids 50 "$ROOTFS" sh -c 'sleep 30 & sleep 30 & wait')
sleep 0.5
stats=$("$LCR" stats "$id")
expect_match "stats shows the memory limit" "$stats" "memory limit     64.0 MiB"
expect_match "stats shows the pids limit" "$stats" "pids limit       50"
expect_match "stats counts the processes" "$stats" "pids current     4"
expect_match "stats shows no oom kills" "$stats" "oom kills        0"
before=$(wc -l < "$parent/$id/cgroup.procs")
"$LCR" exec "$id" sleep 2 &
sleep 0.5
expect "exec and its child join the container cgroup" "$(( $(wc -l < "$parent/$id/cgroup.procs") - before ))" 2
wait
if [ "$version" = 2 ]; then
	expect "memory.max is visible inside the container" "$("$LCR" exec "$id" cat /sys/fs/cgroup/memory.max)" 67108864
	expect "cpu.max is visible inside the container" "$("$LCR" exec "$id" cat /sys/fs/cgroup/cpu.max)" "100000 100000"
	expect "the cgroup2 mount inside is read-only" "$("$LCR" exec "$id" sh -c '(echo max > /sys/fs/cgroup/memory.max) 2>/dev/null && echo wrote || echo refused')" refused
	expect "the host sees the init in the container cgroup" "$(grep -c "^$("$LCR" ps | awk -v id="$id" '$1 == id {print $3}')\$" "$parent/$id/cgroup.procs")" 1
fi
"$LCR" kill "$id" KILL
sleep 0.5
"$LCR" rm "$id"
expect "rm removes the cgroup of a detached container" "$(test -d "$parent/$id" && echo present || echo absent)" absent

expect "an oom kill is reported in stats" "$(id=$(ctr -d -m 32m "$ROOTFS" sh -c 'probe hog 64; sleep 30'); sleep 1; "$LCR" stats "$id" | grep 'oom kills' | awk '{print $3}'; "$LCR" kill "$id" KILL; sleep 0.3; "$LCR" rm "$id")" 1

finish
