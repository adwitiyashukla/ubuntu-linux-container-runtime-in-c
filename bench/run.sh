#!/bin/bash
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
LCR=${LCR:-$root/lcr}
runs=${RUNS:-50}

if [ "$(id -u)" != 0 ]; then
	echo "the benchmark needs root for cgroups and bridge networking" >&2
	exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/lcr-bench.XXXXXX")
trap 'rm -rf "$work"' EXIT
rootfs=$work/rootfs
export LCR_ROOT=$work/state
"$root/tests/mkrootfs.sh" "$rootfs" || exit 1
${CC:-cc} -O2 -static -o "$rootfs/bin/probe" "$root/tests/probe.c" || exit 1

timeit() {
	local s e
	s=$EPOCHREALTIME
	"$@" >/dev/null 2>&1
	e=$EPOCHREALTIME
	awk -v s="$s" -v e="$e" 'BEGIN { printf "%.3f\n", (e - s) * 1000 }'
}

stats() {
	sort -n | awk '{ a[NR] = $1; sum += $1 } END { printf "%.1f %.1f %.1f %.1f", a[1], a[int(NR * 0.5 + 0.5)], a[int(NR * 0.95 + 0.5)], sum / NR }'
}

latency() {
	local name=$1
	local i min p50 p95 mean
	shift
	for i in $(seq "$runs"); do
		timeit "$@"
	done | stats | {
		read -r min p50 p95 mean
		printf '%-40s %8s %8s %8s %8s\n' "$name" "$min" "$p50" "$p95" "$mean"
	}
}

echo "kernel $(uname -r), $(nproc) cpus, cgroup v$(test -f /sys/fs/cgroup/cgroup.controllers && echo 2 || echo 1)"
echo
echo "startup latency for /bin/true, $runs runs, milliseconds"
printf '%-40s %8s %8s %8s %8s\n' "" min p50 p95 mean
latency "unshare -pfmuin + chroot (baseline)" unshare -pfmuin chroot "$rootfs" /bin/true
latency "lcr run" "$LCR" run "$rootfs" /bin/true
latency "lcr run -m 64m --cpus 1 --pids 100" "$LCR" run -m 64m --cpus 1 --pids 100 "$rootfs" /bin/true
latency "lcr run --net bridge" "$LCR" run --net bridge "$rootfs" /bin/true
latency "lcr run --net bridge -p 18080:80" "$LCR" run --net bridge -p 18080:80 "$rootfs" /bin/true
if docker info >/dev/null 2>&1; then
	tar -C "$rootfs" -c . | docker import - lcr-bench >/dev/null
	latency "docker run --rm --network none" docker run --rm --network none lcr-bench /bin/true
	latency "docker run --rm (bridge)" docker run --rm lcr-bench /bin/true
	docker rmi lcr-bench >/dev/null
fi
id=$("$LCR" run -d "$rootfs" sleep 600)
latency "lcr exec" "$LCR" exec "$id" /bin/true
"$LCR" rm -f "$id"

echo
echo "seccomp filter cost, 5000000 getppid calls"
printf '%-40s %s\n' "default filter" "$("$LCR" run "$rootfs" probe syscalls 5000000)"
printf '%-40s %s\n' "--seccomp none" "$("$LCR" run --seccomp none "$rootfs" probe syscalls 5000000)"

echo
echo "cpu quota accuracy, 2 second busy loop"
for c in 0.25 0.5 1; do
	printf '%-40s %s\n' "--cpus $c" "$("$LCR" run --cpus "$c" "$rootfs" probe spin 2)"
done

echo
echo "memory limit of 32m"
for mb in 16 24 64 128; do
	printf '%-40s %s\n' "allocate and touch $mb MiB" "$("$LCR" run -m 32m "$rootfs" probe hog "$mb" 2>/dev/null || echo "killed, exit code $?")"
done

echo
echo "pids limit"
for n in 4 10 50; do
	printf '%-40s %s\n' "--pids $n, 100 fork attempts" "$("$LCR" run --pids "$n" "$rootfs" probe forks 100)"
done
