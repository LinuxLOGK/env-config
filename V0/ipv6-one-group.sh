#!/usr/bin/env bash
# Replacement smoke test; the prior session's macOS guide is not available here.
set -Eeuo pipefail

[[ $(uname -s) == Linux ]] || { echo 'FAIL: run inside the Linux guest' >&2; exit 1; }
[[ $EUID -eq 0 ]] || { echo 'FAIL: run with sudo bash ipv6-one-group.sh' >&2; exit 1; }
for command in ip python3 mktemp; do
    command -v "$command" >/dev/null || { echo "FAIL: missing $command" >&2; exit 1; }
done
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
[[ -r "$script_dir/ipv6-one-group.py" ]] || { echo 'FAIL: missing Python companion' >&2; exit 1; }
work=$(mktemp -d /tmp/team-v0-mcast.XXXXXXXX)
tx="${work##*/}-tx"
rx="${work##*/}-rx"
tx_created=0
rx_created=0
receiver_pid=''

cleanup() {
    local failed=0
    if [[ -n $receiver_pid ]]; then
        kill "$receiver_pid" 2>/dev/null || true
        wait "$receiver_pid" 2>/dev/null || true
        receiver_pid=''
    fi
    if (( rx_created )); then
        if ip netns delete "$rx"; then rx_created=0; else failed=1; fi
    fi
    if (( tx_created )); then
        if ip netns delete "$tx"; then tx_created=0; else failed=1; fi
    fi
    rm -rf -- "$work"
    return "$failed"
}
trap 'status=$?; cleanup || status=1; exit "$status"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'kernel_release=%s\n' "$(uname -r)"
printf 'boot_id=%s\n' "$(cat /proc/sys/kernel/random/boot_id)"
printf 'test_namespaces=%s,%s\n' "$tx" "$rx"
ip netns add "$tx"
tx_created=1
ip netns add "$rx"
rx_created=1
for ns in "$tx" "$rx"; do
    # These sysctls belong only to the newly created test namespace.
    ip netns exec "$ns" sh -c '
        printf 0 > /proc/sys/net/ipv6/conf/all/disable_ipv6
        printf 0 > /proc/sys/net/ipv6/conf/default/disable_ipv6
    '
    ip -n "$ns" link set lo up
done
# Create both ends inside the test namespaces; no host interface is changed.
ip -n "$tx" link add tx0 type veth peer name rx0 netns "$rx"
ip -n "$tx" link set tx0 up
ip -n "$rx" link set rx0 up
ip -n "$tx" -6 addr add fe80::1/64 dev tx0 nodad
ip -n "$rx" -6 addr add fe80::2/64 dev rx0 nodad
ip -n "$tx" -6 route add ff00::/8 dev tx0

ip netns exec "$rx" python3 -u "$script_dir/ipv6-one-group.py" receive "$work" &
receiver_pid=$!
for ((attempt=0; attempt<100; attempt++)); do
    [[ -f "$work/ready" ]] && break
    kill -0 "$receiver_pid" 2>/dev/null || { wait "$receiver_pid"; exit 1; }
    sleep 0.1
done
[[ -f "$work/ready" ]] || { echo 'FAIL: receiver did not become ready' >&2; exit 1; }
ip -n "$rx" -6 maddr show dev rx0
ip netns exec "$tx" python3 -u "$script_dir/ipv6-one-group.py" send "$work"
wait "$receiver_pid"
receiver_pid=''
cleanup
trap - EXIT
printf '%s\n' 'PASS: joined payload received, unjoined payload absent'
