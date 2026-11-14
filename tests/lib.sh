pass=0
fail=0

ok() {
	pass=$((pass + 1))
	printf 'ok   %s\n' "$1"
}

bad() {
	fail=$((fail + 1))
	printf 'FAIL %s\n' "$1"
}

expect() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		bad "$1: expected [$3] got [$2]"
	fi
}

expect_match() {
	case "$2" in
	*"$3"*) ok "$1" ;;
	*) bad "$1: expected something containing [$3] got [$2]" ;;
	esac
}

expect_range() {
	if awk -v v="$2" -v lo="$3" -v hi="$4" 'BEGIN { exit !(v >= lo && v <= hi) }'; then
		ok "$1 ($2)"
	else
		bad "$1: $2 is outside [$3, $4]"
	fi
}

ctr() {
	"$LCR" run "$@"
}

fetch() {
	if command -v curl >/dev/null 2>&1; then
		curl -s --max-time 5 "$1"
	else
		wget -qO- -T 5 "$1"
	fi
}

wait_until() {
	local tries=$1
	shift
	while [ "$tries" -gt 0 ]; do
		if "$@"; then
			return 0
		fi
		tries=$((tries - 1))
		sleep 0.1
	done
	return 1
}

stopped() {
	"$LCR" ps | awk -v c="$1" 'NR > 1 && ($1 == c || $2 == c) && $4 == "exited" {f = 1} END {exit !f}'
}

ready() {
	"$LCR" logs "$1" 2>/dev/null | grep -qx ready
}

reachable() {
	fetch "$1" >/dev/null 2>&1
}

stop_all() {
	for id in $("$LCR" ps | awk 'NR > 1 {print $1}'); do
		"$LCR" rm -f "$id" 2>/dev/null
	done
}

finish() {
	printf '%s: %d passed, %d failed\n' "$(basename "$0" .sh)" "$pass" "$fail"
	stop_all
	[ "$fail" -eq 0 ]
}
