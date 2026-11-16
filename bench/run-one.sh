#!/usr/bin/env bash
# 측정 1회 실행: 부팅 확인(절차서 2.4) → netns 구성 → mc6bench → 정리 → JSON 한 줄을 결과 파일에 추가
#
# 사용법:
#   sudo ./run-one.sh --expect-release 5.4.214-team-vi --expect-config-sha <sha> --out results/M/run.jsonl \
#        -- --workload wa --groups 1024 --rate 50000 --meta phase=M --meta block=3
#
# --expect-* 를 주면 불일치 시 실행하지 않고 invalid 줄을 남긴다.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
EXPECT_REL="" EXPECT_CFG="" EXPECT_IMG="" OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --expect-release) EXPECT_REL=$2; shift 2 ;;
    --expect-config-sha) EXPECT_CFG=$2; shift 2 ;;
    --expect-image-sha) EXPECT_IMG=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[[ -n $OUT ]] || { echo "--out 필요" >&2; exit 2; }
mkdir -p "$(dirname "$OUT")"

rel=$(uname -r)
cfg_sha=$(zcat /proc/config.gz 2>/dev/null | sha256sum | cut -d' ' -f1 || true)
img_sha=$(sha256sum "/boot/vmlinuz-$rel" 2>/dev/null | cut -d' ' -f1 || true)
tainted=$(cat /proc/sys/kernel/tainted)
tool_sha=$(sha256sum "$HERE/mc6bench" | cut -d' ' -f1)

reason=""
[[ -n $EXPECT_REL && $rel != "$EXPECT_REL" ]] && reason="release_mismatch"
[[ -z $reason && -n $EXPECT_CFG && $cfg_sha != "$EXPECT_CFG" ]] && reason="config_mismatch"
[[ -z $reason && -n $EXPECT_IMG && $img_sha != "$EXPECT_IMG" ]] && reason="image_mismatch"
[[ -z $reason && $tainted != 0 ]] && reason="tainted_$tainted"
if [[ -n $reason ]]; then
  printf '{"tool":"run-one.sh","kernel":"%s","valid":false,"invalid_reason":"%s","args":"%s"}\n' \
    "$rel" "$reason" "$*" >> "$OUT"
  echo "INVALID: $reason" >&2
  exit 1
fi

"$HERE/netns.sh" down >/dev/null 2>&1 || true
"$HERE/netns.sh" up >&2
trap '"$HERE/netns.sh" down >/dev/null 2>&1 || true' EXIT
optmem_init=$(sysctl -n net.core.optmem_max)   # netns.sh 가 설정한 뒤 값. 5.4 에서는 전역이라 rx namespace 안에서 안 보임

ip netns exec rx "$HERE/mc6bench" "$@" \
  --meta "config_sha256=$cfg_sha" --meta "image_sha256=$img_sha" --meta "tool_sha256=$tool_sha" \
  --meta "optmem_max_init=$optmem_init" --meta "tainted_before=$tainted" \
  >> "$OUT"
tail -n1 "$OUT" >&2
