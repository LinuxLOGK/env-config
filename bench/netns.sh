#!/usr/bin/env bash
# 측정용 namespace 구성: tx(veth0, fd00::1) <-> rx(veth1, fd00::2)
# 영훈 s-fixture/guest/60-netns-fixture.sh 와 같은 구성에서 dummy0 만 뺀 것 (성능 측정은 수신 인터페이스 하나).
# 사용법: sudo ./netns.sh up | down | status
set -euo pipefail
CMD="${1:-up}"
OPTMEM="${OPTMEM_MAX:-262144}"

case "$CMD" in
up)
  if ip netns list | grep -qw tx || ip netns list | grep -qw rx; then
    echo "이미 있음. down 후 다시 실행" >&2; exit 1
  fi
  modprobe veth 2>/dev/null || true
  ip netns add tx
  ip netns add rx
  ip link add veth0 type veth peer name veth1
  ip link set veth0 netns tx
  ip link set veth1 netns rx
  for ns in tx rx; do ip -n "$ns" link set lo up; done
  ip netns exec rx sysctl -qw net.ipv6.conf.all.forwarding=0
  ip netns exec rx sysctl -qw net.ipv6.conf.veth1.forwarding=0 2>/dev/null || true
  ip -n tx addr add fd00::1/64 dev veth0 nodad
  ip -n rx addr add fd00::2/64 dev veth1 nodad
  ip -n tx link set veth0 up
  ip -n rx link set veth1 up
  # 송신 쪽 멀티캐스트 경로
  ip -n tx -6 route add ff00::/8 dev veth0 table local 2>/dev/null || true
  # 5.4 의 optmem_max 는 전역 값. 모든 비교 이미지에서 같은 값으로.
  sysctl -qw net.core.optmem_max="$OPTMEM"
  # 링크 로컬 주소 DAD 완료 대기
  for _ in $(seq 1 40); do
    ip -n rx -6 addr show dev veth1 | grep -q tentative || break; sleep 0.25
  done
  echo "up: optmem_max=$(sysctl -n net.core.optmem_max)"
  echo "rx veth1 기본 가입 그룹 수: $(ip -n rx -6 maddr show dev veth1 | grep -c inet6)"
  ;;
down)
  ip netns del tx 2>/dev/null || true
  ip netns del rx 2>/dev/null || true
  echo down
  ;;
status)
  ip netns list
  ip -n rx -6 addr show dev veth1 || true
  ip -n rx -6 maddr show dev veth1 || true
  echo "optmem_max=$(sysctl -n net.core.optmem_max) kernel=$(uname -r)"
  ;;
*) echo "usage: $0 up|down|status" >&2; exit 2 ;;
esac
