#!/bin/bash
. "$(dirname "$0")/lib.sh"

id=$(ctr -d --name lc "$ROOTFS" sh -c 'echo started; trap "echo got TERM; exit 3" TERM; sleep 30 & wait')
sleep 0.5

expect "run -d prints a 12 character id" "$(printf '%s' "$id" | wc -c)" 12
expect "ps lists the container as running" "$("$LCR" ps | awk '$2 == "lc" {print $4}')" running
expect "ps shows the command" "$("$LCR" ps | awk '$2 == "lc" {print $7, $8}')" "sh -c"
expect "a name resolves to the container" "$("$LCR" exec lc hostname)" lc
expect "an id prefix resolves to the container" "$("$LCR" exec "${id:0:6}" hostname)" lc
expect "exec runs inside the pid namespace" "$("$LCR" exec lc cat /proc/1/comm)" lcr
expect "exec sees the container processes" "$("$LCR" exec lc sh -c 'ls -d /proc/2 /proc/3 | tr "\n" " "')" "/proc/2 /proc/3 "
expect "exec honors -w" "$("$LCR" exec -w /tmp lc pwd)" /tmp
expect "exec honors -e" "$("$LCR" exec -e X=1 lc sh -c 'echo $X')" 1
expect "exec applies the capability bounding set" "$("$LCR" exec lc grep CapBnd /proc/self/status | awk '{print $2}')" 00000000a80425fb
expect "exec applies seccomp" "$("$LCR" exec lc probe syscall add_key)" EPERM
expect "exec propagates the exit code" "$("$LCR" exec lc sh -c 'exit 9'; echo $?)" 9
expect "kill rejects an unknown signal" "$("$LCR" kill lc NOPE 2>&1 | grep -c 'unknown signal')" 1
expect "rm refuses a running container" "$("$LCR" rm lc 2>&1 | grep -c 'is running')" 1

"$LCR" kill lc TERM
sleep 1
expect "TERM is forwarded and the trap exit code is kept" "$("$LCR" ps | awk '$2 == "lc" {print $4, $5}')" "exited (3)"
expect "logs keep the output of a detached container" "$("$LCR" logs lc | tr '\n' ' ')" "started got TERM "
expect "exec refuses a stopped container" "$("$LCR" exec lc true 2>&1 | grep -c 'no running container')" 1
"$LCR" rm lc
expect "rm removes the container" "$("$LCR" ps | grep -c ' lc ')" 0
expect "a removed container cannot be resolved" "$("$LCR" logs lc 2>&1 | grep -c 'no container')" 1

expect "the exit code of the command is propagated" "$(ctr "$ROOTFS" sh -c 'exit 7'; echo $?)" 7
expect "a signal death becomes 128 plus the signal" "$(ctr "$ROOTFS" sh -c 'kill -9 $$'; echo $?)" 137
expect "SIGINT sent to the runtime stops the command" "$(timeout --preserve-status -s INT 1 "$LCR" run "$ROOTFS" sleep 10; echo $?)" 130
expect "SIGTERM sent to the runtime stops the command" "$(timeout --preserve-status -s TERM 1 "$LCR" run "$ROOTFS" sleep 10; echo $?)" 143
expect "orphans are reaped by the init" "$(ctr "$ROOTFS" probe zombie)" zombies=0
expect "background children are terminated when the command exits" "$(ctr "$ROOTFS" sh -c 'sleep 31 & echo done'; pgrep -c -x -f 'sleep 31')" $'done\n0'
expect "stdin reaches the command" "$(echo ping | ctr "$ROOTFS" cat)" ping
expect "environment variables are set" "$(ctr -e FOO=bar --hostname h "$ROOTFS" sh -c 'echo $FOO $HOME $HOSTNAME')" "bar /root h"
expect "a variable can be copied from the host" "$(COPYME=42 ctr -e COPYME "$ROOTFS" sh -c 'echo $COPYME')" 42
expect "the environment is not leaked from the host" "$(LEAK=1 ctr "$ROOTFS" sh -c 'echo ${LEAK:-unset}')" unset

dup=$(ctr -d --name dup "$ROOTFS" sleep 30)
expect "duplicate names are refused" "$(ctr --name dup "$ROOTFS" true 2>&1 | grep -c 'already exists')" 1
expect "rm -f kills and removes a running container" "$("$LCR" rm -f dup; "$LCR" ps | grep -c ' dup ')" 0
expect "a bad image path is reported" "$(ctr /nonexistent true 2>&1 | grep -c 'image directory')" 1
expect "the state directory is empty at the end" "$(ls "$LCR_ROOT" | wc -l)" 0

finish
