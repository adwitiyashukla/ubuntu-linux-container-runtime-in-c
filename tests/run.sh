#!/bin/bash
set -u

here=$(cd "$(dirname "$0")" && pwd)
export LCR=${LCR:-$(dirname "$here")/lcr}
mode=${1:-auto}
if [ "$mode" = auto ]; then
	if [ "$(id -u)" = 0 ]; then
		mode=root
	else
		mode=rootless
	fi
fi
export MODE=$mode

work=$(mktemp -d "${TMPDIR:-/tmp}/lcr-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT
export WORK=$work
export ROOTFS=$work/rootfs
export LCR_ROOT=$work/state

"$here/mkrootfs.sh" "$ROOTFS" || exit 1
${CC:-cc} -O2 -static -o "$ROOTFS/bin/probe" "$here/probe.c" || exit 1

failed=0
for t in "$here"/test_*.sh; do
	name=$(basename "$t" .sh)
	case "$name" in
	test_cgroups | test_network)
		if [ "$mode" != root ]; then
			echo "skip $name (needs root)"
			continue
		fi
		;;
	esac
	echo "== $name"
	bash "$t" || failed=$((failed + 1))
done

if [ "$failed" -ne 0 ]; then
	echo "$failed test files failed ($mode mode)"
	exit 1
fi
echo "all test files passed ($mode mode)"
