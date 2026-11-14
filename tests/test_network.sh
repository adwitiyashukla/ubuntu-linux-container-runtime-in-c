#!/bin/bash
. "$(dirname "$0")/lib.sh"

web=$(ctr -d --net bridge --name web "$ROOTFS" httpd -f -p 80 -h /www)
peer=$(ctr -d --net bridge --name peer "$ROOTFS" sleep 60)
ip=$("$LCR" ps | awk '$2 == "web" {print $5}')
peer_ip=$("$LCR" ps | awk '$2 == "peer" {print $5}')
wait_until 100 reachable "http://$ip/index.html"

expect_match "the container gets an address in the lcr subnet" "$ip" "10.77.0."
expect "two containers get different addresses" "$(test "$ip" != "$peer_ip" && echo yes || echo no)" yes
expect "the bridge exists on the host" "$(ip -o link show lcr0 | wc -l)" 1
expect "the bridge carries the gateway address" "$(ip -4 -o addr show lcr0 | awk '{print $4}')" 10.77.0.1/24
expect "eth0 inside carries the address" "$("$LCR" exec web ip -4 -o addr show eth0 | awk '{print $4}')" "$ip/24"
expect "the default route points at the bridge" "$("$LCR" exec web ip route | grep -c 'default via 10.77.0.1 dev eth0')" 1
expect "the host reaches the container over the bridge" "$(fetch "http://$ip/index.html")" "hello from lcr"
expect "the container reaches the gateway" "$("$LCR" exec web ping -c 1 -W 2 10.77.0.1 >/dev/null 2>&1 && echo yes || echo no)" yes
expect "containers reach each other" "$("$LCR" exec peer wget -qO- "http://$ip/index.html")" "hello from lcr"
expect "resolv.conf has a usable nameserver" "$("$LCR" exec peer grep -c '^nameserver' /etc/resolv.conf | awk '{print ($1 > 0) ? "yes" : "no"}')" yes
expect "no loopback resolver is copied in" "$("$LCR" exec peer grep -c '^nameserver 127\.' /etc/resolv.conf)" 0
expect "/etc/hosts maps the hostname to the address" "$("$LCR" exec web grep -c "^$ip.*web" /etc/hosts)" 1
expect "ip_forward is enabled" "$(cat /proc/sys/net/ipv4/ip_forward)" 1
expect "the masquerade rule is installed" "$(nft list table ip lcr0 | grep -c masquerade)" 2

pub=$(ctr -d --net bridge --name pub -p 18080:80 "$ROOTFS" httpd -f -p 80 -h /www)
pub_ip=$("$LCR" ps | awk '$2 == "pub" {print $5}')
wait_until 100 reachable http://127.0.0.1:18080/index.html
expect "a published port answers on localhost" "$(fetch http://127.0.0.1:18080/index.html)" "hello from lcr"
expect "the dnat rule targets the container" "$(nft list table ip "lcr_$pub" | grep -c "dnat to $pub_ip:80")" 2
expect "publishing without bridge networking is rejected" "$(ctr -p 18081:80 "$ROOTFS" true 2>&1 | grep -c 'needs --net bridge')" 1

"$LCR" kill pub KILL
wait_until 100 stopped pub
expect "the port table is removed with the container" "$(nft list tables | grep -c "lcr_$pub")" 0
wait_until 100 sh -c '! ip -o link show | grep -q "lcr'"${pub:0:10}"'"'
expect "the veth is removed with the container" "$(ip -o link show | grep -c "lcr${pub:0:10}")" 0
"$LCR" rm pub

expect "addresses are reused after release" "$(ctr --net bridge "$ROOTFS" ip -4 -o addr show eth0 | awk '{print $4}')" "$pub_ip/24"

stop_all
expect "nothing is left running" "$("$LCR" ps | wc -l)" 1

finish
