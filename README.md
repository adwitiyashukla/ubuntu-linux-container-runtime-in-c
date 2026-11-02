# ubuntu-linux-container-runtime-in-c

A container runtime for Ubuntu written in C. The command is `lcr`. It depends on nothing but libc and the kernel. One binary, about 3100 lines, built to understand what actually happens when you type `docker run`.

It creates the namespaces, sets cgroup limits, builds an overlayfs root and pivots into it, drops capabilities, installs a seccomp filter, wires a veth pair into a bridge over raw netlink, and runs a real PID 1 that reaps zombies and forwards signals. Containers can be listed, exec'd into, signaled, inspected and removed. It also runs without root through user namespaces.

I built it while preparing for Linux engineering work. Reading about namespaces is one thing. Getting pivot_root, cgroup v2 delegation and a NAT rule to behave on a real Ubuntu box is another.

## What it does

| Area | Mechanism |
|---|---|
| Isolation | clone3 with new pid, mount, uts, ipc, net and cgroup namespaces, plus a user namespace when not running as root |
| Root filesystem | overlayfs with the image directory as the read-only lower layer and a per-container upper layer, then pivot_root into it |
| Special mounts | proc, read-only sysfs, a tmpfs /dev with bind-mounted device nodes, a private devpts instance, /dev/shm, /dev/mqueue, and a read-only cgroup2 mount showing the container its own limits |
| Hardening | the same default capability bounding set as Docker (a80425fb), a hand-written seccomp BPF program that denies 55 syscalls, no_new_privs, masked /proc and /sys paths, read-only /proc/sys |
| Resources | cgroup v2 memory.max, cpu.max and pids.max, with CLONE_INTO_CGROUP so the init is born inside its cgroup, and a cgroup v1 fallback for older hosts |
| Networking | bridge lcr0 on 10.77.0.0/24, one veth pair per container created over rtnetlink with the peer placed straight into the container's netns, nftables masquerade, TCP port publishing with DNAT |
| Init | PID 1 reaps orphans, forwards signals to the command, terminates leftovers and exits with the command's status |
| Lifecycle | run, ps, exec (setns into every namespace plus the cgroup), kill (pidfd based, safe against pid reuse), stats, logs, rm |
| Rootless | uid 0 inside maps to the calling user, overlayfs with userxattr, everything works except cgroup limits and bridge networking |

## Quick start

Ubuntu 22.04 or 24.04:

```
sudo apt install build-essential nftables busybox-static
git clone https://github.com/adwitiyashukla/ubuntu-linux-container-runtime-in-c
cd ubuntu-linux-container-runtime-in-c
make
sudo make install
```

A container needs a root filesystem directory. debootstrap gives you a real Ubuntu in about a minute:

```
sudo debootstrap --variant=minbase noble /var/lib/images/noble http://archive.ubuntu.com/ubuntu
```

The tests use a 3 MB busybox root, which is enough to play with:

```
tests/mkrootfs.sh /tmp/busybox
```

Run a shell:

```
sudo lcr run --net bridge -m 256m --cpus 1 --hostname noble /var/lib/images/noble bash
```

Inside you are PID 2 under the lcr init, with your own hostname, an eth0 on 10.77.0.0/24 with a default route through the bridge, a resolv.conf copied from the host, and a root filesystem whose writes disappear when the container exits.

A web server in the background:

```
$ sudo lcr run -d --name web --net bridge -m 64m -p 8080:80 /tmp/busybox httpd -f -p 80 -h /www
4be50f3a227c
$ curl localhost:8080
hello from lcr
$ sudo lcr ps
ID            NAME              PID      STATUS         IP           AGE     COMMAND
4be50f3a227c  web               2196     running        10.77.0.2    0s      httpd -f -p 80 -h /www
$ sudo lcr stats web
memory current   0.7 MiB
memory peak      0.7 MiB
memory limit     64.0 MiB
cpu usage        0.003 s
pids current     2
pids limit       unlimited
oom kills        0
$ sudo lcr exec web ip -4 -o addr show eth0
2: eth0    inet 10.77.0.2/24 brd 10.77.0.255 scope global eth0\       valid_lft forever preferred_lft forever
$ sudo lcr kill web
$ sudo lcr ps
ID            NAME              PID      STATUS         IP           AGE     COMMAND
4be50f3a227c  web               0        exited (143)   10.77.0.2    1s      httpd -f -p 80 -h /www
$ sudo lcr logs web
$ sudo lcr rm web
```

Foreground containers clean up after themselves when they exit. Detached ones keep their state and log until `rm`, like Docker without `--rm`.

Without root:

```
$ id -u
30033
$ lcr run --hostname rootless ~/rootfs sh -c 'id; cat /proc/self/uid_map'
uid=0(root) gid=0(root) groups=0(root)
         0      30033          1
```

## Run options

```
usage: lcr run [options] IMAGE_DIR COMMAND [ARG...]

  -d, --detach            run in the background and print the container id
  -n, --name NAME         container name (default: generated id)
      --hostname NAME     hostname inside the container (default: name)
      --net MODE          none, host or bridge (default: none)
  -p, --publish H:C       forward host TCP port H to container port C (bridge only)
  -v, --volume S:D[:ro]   bind mount host path S at D inside the container
  -e, --env K=V           set an environment variable (K alone copies it from the host)
  -w, --workdir DIR       working directory inside the container (default: /)
  -u, --user UID[:GID]    run the command as this numeric uid and gid
  -m, --memory SIZE       memory limit, for example 64m or 1g
      --cpus N            cpu limit in cores, for example 0.5 or 2
      --pids N            maximum number of processes
      --read-only         mount the root filesystem read-only
      --rw-image          run on the image directory itself instead of an overlay
      --cap-add CAP       add a capability to the default bounding set
      --cap-drop CAP      drop a capability from the default bounding set
      --privileged        keep every capability and disable seccomp and masking
      --seccomp none      disable the default seccomp filter
```

Exit codes follow the Docker convention: the command's own status, 128 plus the signal if it was killed, 126 if the command could not be executed, 127 if it was not found, 125 if the runtime itself failed.

## How a container starts

```
lcr run (host)                      container init (pid 1)             command (pid 2)
-----------------------------       -----------------------------      ---------------
parse options, pick an id
create the state directory
create the cgroup, write limits
clone3 with new pid, mount, uts,
ipc, net namespaces, straight
into the cgroup                 ->  block on a socketpair
write uid_map and gid_map
(rootless only)
create the veth pair, move the
peer into the netns, attach the
host end to the bridge
save state, send go             ->  unshare the cgroup namespace
                                    sethostname
                                    bring up lo, address eth0,
                                    add the default route
                                    overlay, proc, sys, dev, binds,
                                    /etc/hostname, hosts, resolv.conf
                                    pivot_root, mask /proc paths
                                    drop capabilities
                                    setuid/setgid (with -u)
                                    no_new_privs, seccomp filter
                                    fork                           ->  execvp(command)
waitpid, forward signals            reap zombies, forward signals      ...
remove port rules, cgroup,      <-  kill leftovers, exit with the  <-  exit
state directory                     command's status
```

The parent does everything that needs host privileges or host-side knowledge: the cgroup, the id maps, the veth pair. The child does everything that has to happen inside the new namespaces. A one byte message on a socketpair orders the two sides, so the child never configures eth0 before it exists and never moves into cgroups it is already in.

The rootfs is an overlayfs mount: `lowerdir` is the image directory, `upperdir` and `workdir` live under `/var/lib/lcr/<id>`. The image is never written to unless you ask for `--rw-image`. pivot_root uses the `pivot_root(".", ".")` form followed by `umount2(".", MNT_DETACH)`, so no put_old directory is needed inside the image.

The init matters more than it looks. Processes whose parent dies get reparented to PID 1. If PID 1 is a shell script or a server that never calls wait, every one of them becomes a zombie. The lcr init sits in sigwaitinfo, reaps on every SIGCHLD, forwards SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2 and SIGWINCH to the command, and once the command exits it sends SIGTERM to whatever is left, waits up to two seconds, then SIGKILL. Signals that came from the terminal (si_code SI_KERNEL) are not forwarded, because the foreground process group already received them directly.

## Security defaults

The bounding set keeps CHOWN, DAC_OVERRIDE, FOWNER, FSETID, KILL, SETGID, SETUID, SETPCAP, NET_BIND_SERVICE, NET_RAW, SYS_CHROOT, MKNOD, AUDIT_WRITE and SETFCAP. Everything else is dropped with PR_CAPBSET_DROP, the ambient set is cleared and the inheritable set emptied, so no execve inside the container can regain them. `cat /proc/self/status` inside a container shows `CapBnd: 00000000a80425fb`, the same value you see in a default Docker container. `--cap-add` and `--cap-drop` adjust the set, `--privileged` keeps all of them.

The seccomp filter is a classic BPF program assembled in `src/seccomp.c`. It kills the process on a foreign architecture or an x32 syscall, returns EPERM for 55 syscalls (mount, umount2, pivot_root, unshare, setns, reboot, swapon, kexec_load, init_module, bpf, perf_event_open, ptrace helpers like process_vm_readv, the keyring calls, the mount API calls, io_uring and so on) and allows the rest. Allowing by default means glibc's clone3, rseq or any syscall added next year keep working. `--seccomp none` turns it off. The filter is installed with PR_SET_NO_NEW_PRIVS, so setuid binaries inside the container cannot gain privileges either.

After pivot_root the init bind mounts /dev/null over /proc/kcore, /proc/keys, /proc/timer_list and friends, puts an empty read-only tmpfs over /proc/acpi, /proc/scsi and /sys/firmware, and remounts /proc/bus, /proc/fs, /proc/irq, /proc/sys and /proc/sysrq-trigger read-only. `--privileged` skips the masking.

`-u 1000:1000` calls setgroups, setgid and setuid before the exec. The effective capability set of a non-root command is empty.

## Networking

`--net none` gives the container a fresh network namespace with only a loopback device. `--net host` keeps the host namespace. `--net bridge` does the full thing:

1. Create the bridge `lcr0` if it is missing, give it 10.77.0.1/24, bring it up, set `net.ipv4.ip_forward=1`. All of this is rtnetlink: RTM_NEWLINK with an IFLA_LINKINFO kind of `bridge`, RTM_NEWADDR, another RTM_NEWLINK for IFF_UP.
2. Load an nftables table with a masquerade rule for traffic leaving 10.77.0.0/24 and a forward chain that accepts bridge traffic. The ruleset is only applied when the table does not exist yet, which keeps a bridge start at about 35 ms instead of about 50.
3. Pick the lowest free address, reading the addresses of running containers from the state directory.
4. Create the veth pair with one RTM_NEWLINK: the host end is `lcr` plus the first ten characters of the id, and the peer is created as `eth0` directly inside the container's namespace through IFLA_NET_NS_FD. No rename, no second move.
5. Attach the host end to the bridge with IFLA_MASTER and bring it up.
6. Inside the namespace the init brings up lo, adds the address with RTM_NEWADDR, brings eth0 up and installs the default route via 10.77.0.1 with RTM_NEWROUTE.

`-p 8080:80` adds a per-container nft table with a DNAT rule in prerouting for traffic arriving from outside and one in the output hook for traffic from the host itself, matched with `fib daddr type local`. Reaching a published port on 127.0.0.1 also needs `route_localnet=1` on the bridge and a masquerade rule for locally sourced packets leaving the bridge, otherwise the container answers 127.0.0.1 and the reply dies in its own loopback. That one took a while to figure out. The table is deleted when the container exits.

If Docker or ufw is installed the iptables FORWARD policy is DROP, which would silently drop routed container traffic. lcr reads `iptables -S FORWARD` once per start and, only when the policy is DROP, inserts accept rules for the bridge. Bridged traffic between containers never reaches iptables because `nf_call_iptables` is turned off on the bridge.

The container's /etc/resolv.conf comes from `/run/systemd/resolve/resolv.conf` when systemd-resolved is running, since the stub address 127.0.0.53 is unreachable from another network namespace. Loopback nameservers are skipped, and 8.8.8.8 and 1.1.1.1 are used if nothing is left.

## Resource limits

On a cgroup v2 host lcr creates `/sys/fs/cgroup/lcr/<id>`, enables the cpu, memory and pids controllers in the parent's `cgroup.subtree_control`, writes `memory.max` (and `memory.swap.max=0` so the limit cannot be dodged through swap), `cpu.max` as `quota period` with a 100 ms period, and `pids.max`. The init is created directly inside that cgroup with clone3 and CLONE_INTO_CGROUP, so there is no window where it runs unlimited. `stats` reads `memory.current`, `memory.peak`, `memory.events`, `cpu.stat` and `pids.current`. On exit `cgroup.kill` makes sure nothing survives, then the directory is removed.

Inside the container a read-only cgroup2 mount at /sys/fs/cgroup shows the container's own cgroup as the root, because the init also unshares the cgroup namespace after it has been moved. `cat /sys/fs/cgroup/memory.max` inside a `-m 64m` container prints 67108864.

On a cgroup v1 host the same limits go to `memory.limit_in_bytes`, `cpu.cfs_quota_us` and `pids.max` under each controller's hierarchy, and the pid is written into each `cgroup.procs` before the init is released. Both paths are tested, v1 on the development VM and v2 in CI.

## Rootless mode

When lcr is started by a normal user it adds CLONE_NEWUSER to the clone flags, writes `deny` to the child's setgroups file and maps uid and gid 0 inside to the caller's ids outside, with a single-entry map and no newuidmap helper. Inside the user namespace the init holds all capabilities, so the same mount, pivot_root, capability and seccomp code runs unchanged. overlayfs gets the `userxattr` option, devpts is mounted without `gid=5`, and sysfs falls back to a read-only bind mount of the host's /sys when the container shares the host network namespace. State lives in `$XDG_RUNTIME_DIR/lcr`.

cgroup limits and bridge networking need root and are refused with a clear message. Ubuntu 24.04 restricts unprivileged user namespaces by default, so either run `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0` or give the binary an AppArmor profile.

## Measured results

Development VM: Ubuntu 24.04, kernel 6.18, 2 vCPUs, cgroup v1, busybox rootfs, 100 runs per row. Docker 29.9 ran on the same VM with the same rootfs imported as an image, dockerd started with --iptables=false. `make bench` reproduces the table.

| Start a container and run /bin/true | min | p50 | p95 | mean |
|---|---|---|---|---|
| unshare -pfmuin + chroot (what the kernel alone costs) | 3.6 ms | 4.6 ms | 5.6 ms | 4.8 ms |
| lcr run | 6.4 ms | 8.2 ms | 16.7 ms | 9.6 ms |
| lcr run -m 64m --cpus 1 --pids 100 | 6.5 ms | 8.3 ms | 15.7 ms | 9.1 ms |
| lcr run --net bridge | 20.2 ms | 35.6 ms | 50.6 ms | 36.1 ms |
| lcr run --net bridge -p 18080:80 | 41.2 ms | 58.8 ms | 73.6 ms | 58.3 ms |
| lcr exec | 2.8 ms | 3.5 ms | 4.6 ms | 3.6 ms |
| docker run --rm --network none | 173.6 ms | 216.7 ms | 236.0 ms | 216.7 ms |
| docker run --rm (bridge) | 241.8 ms | 297.3 ms | 334.2 ms | 298.4 ms |

A plain start costs about 4 ms more than the bare namespaces, which pays for the overlay mount, a dozen other mounts, the cgroup, the state file and the capability and seccomp setup. Bridge mode is dominated by three forks: `nft list table` to check the ruleset, `iptables -S FORWARD` to check the policy, and the veth creation itself, which can wait on the kernel's cleanup of the previous container's network namespace.

| Limits | Result |
|---|---|
| allocate 16 MiB under -m 32m | completes |
| allocate 24 MiB under -m 32m | completes |
| allocate 64 MiB under -m 32m | killed by the OOM killer, exit code 137, `oom kills 1` in stats |
| allocate 128 MiB under -m 32m | killed, exit code 137 |
| --cpus 0.25, 2 second busy loop | 0.52 s of CPU time |
| --cpus 0.5, 2 second busy loop | 1.04 s of CPU time |
| --cpus 1, 2 second busy loop | 1.97 s of CPU time |
| --pids 4, 100 fork attempts | 2 succeed (init and the probe hold the other two) |
| --pids 10, 100 fork attempts | 8 succeed |
| --pids 50, 100 fork attempts | 48 succeed |

| seccomp filter cost | ns per getppid call |
|---|---|
| default filter (55 denied syscalls, linear scan) | 167.4 |
| --seccomp none | 137.7 |

About 30 ns per syscall for the filter walk, which is what a 117 instruction BPF program should cost.

## Tests

`sudo make test` builds a busybox rootfs and a static probe binary, then runs 132 checks across six files. `tests/run.sh rootless` runs 94 of them as a normal user. Each check runs a real container and looks at the result from both sides, for example a tmpfs mounted inside a privileged container must show up in the container's /proc/mounts and must not show up in the host's.

| File | What it checks |
|---|---|
| test_namespaces | pid 2 behind the init, process visibility, hostname inside and outside, loopback only, host networking, ipc, cgroup root, mount propagation, uid_map in rootless mode |
| test_mounts | overlay layering, --rw-image, --read-only, proc, read-only sysfs, /dev nodes, private devpts, /dev/shm, masked and read-only /proc paths, volumes in rw and ro, file binds, /etc files, workdir, state cleanup |
| test_security | the exact bounding set with and without cap-add and cap-drop, no_new_privs, sethostname with and without SYS_ADMIN, add_key, keyctl, unshare and mount against the seccomp filter, --privileged, -u, exit codes 126 and 127 |
| test_cgroups | the OOM kill at 64 MiB and the pass at 16 MiB, cpu time under 0.5 and 0.25 cores, pids limits, stats output, exec joining the cgroup, memory.max visible inside on v2, cgroup removal after run and rm |
| test_network | address assignment, two containers with different addresses, bridge address, eth0 and default route inside, host to container HTTP, ping to the gateway, container to container HTTP, resolv.conf without loopback servers, ip_forward, masquerade rules, a published port on localhost, DNAT rules, cleanup of the table and the veth, address reuse |
| test_lifecycle | run -d, ps, name and prefix resolution, exec with -w and -e, exec inheriting caps and seccomp, exit code propagation, kill TERM reaching a shell trap, logs, rm and rm -f, 130 and 143 from signals sent to the runtime, zombie reaping, leftover children terminated, stdin, environment handling, duplicate names |

The probe (`tests/probe.c`) is a static C program copied into the rootfs. It calls a named syscall and prints the errno, allocates and touches a given number of MiB, spins for a number of seconds and prints its CPU time, forks N children, counts zombies after an orphan exits, and times a tight getppid loop.

CI on GitHub Actions builds with gcc and clang, runs the root suite on Ubuntu 24.04 (cgroup v2, kernel 6.8), runs the rootless suite as the runner user, and builds and installs the .deb.

## Requirements

Linux 5.7 or newer for clone3 with CLONE_INTO_CGROUP, falling back to a plain clone3 or clone plus a `cgroup.procs` write on older kernels. overlayfs, cgroup v2 or v1, and nftables for bridge mode. Developed and tested on Ubuntu 24.04 with kernels 6.8 and 6.18. The code is x86_64 only because the seccomp program checks for AUDIT_ARCH_X86_64.

## Layout

```
src/main.c      command dispatch
src/run.c       option parsing, clone3, the parent side, the init process
src/mounts.c    overlay, proc, sys, dev, binds, /etc files, pivot_root, masking
src/cgroup.c    cgroup v2 and v1 creation, limits, stats, cleanup
src/net.c       rtnetlink bridge, veth, address and route setup, nftables NAT and DNAT
src/seccomp.c   the BPF denylist
src/caps.c      capability names, default set, bounding set drop and capset
src/state.c     state files, ps, exec, kill, stats, logs, rm
src/util.c      error reporting, file helpers, size parsing, process spawning
tests/          the test suite, the busybox rootfs builder and the probe
bench/run.sh    the benchmark behind the tables above
Makefile        make, make test, make bench, make deb, make install
```

`make deb` builds `build/lcr_0.1.0_amd64.deb` with dpkg-deb, installable with `sudo apt install ./build/lcr_0.1.0_amd64.deb`.
