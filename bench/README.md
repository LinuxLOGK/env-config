# bench — 성능 측정 도구

측정 절차서(`../procedure/README.md`)를 실행하는 도구 모음입니다.

| 파일 | 역할 |
|---|---|
| `mc6bench.c` | 측정 1회: W_A/W_B 가입 → 고정 송신률 송신 → 수신·검증 → CPU·손실·지연 계산 → JSON 한 줄 |
| `netns.sh` | 측정용 namespace 구성 (tx `veth0` fd00::1 ↔ rx `veth1` fd00::2, `optmem_max=262144`) |
| `run-one.sh` | 부팅 확인(release·config·image 해시, tainted) → netns → mc6bench → 결과 파일에 추가 |
| `r0_search.py` | 절차서 4.2의 R0 탐색 (구간 찾기 → 이진 탐색 → 60초 × 2회 확인) |
| `../analysis/stats.py` | 절차서 7절의 블록 대응 감소율과 95% CI, 소규모 회귀 |

## 빌드

```bash
gcc -O2 -Wall -Wextra -pthread -o mc6bench mc6bench.c -lm
chmod +x netns.sh run-one.sh r0_search.py
```

## 실행 예

```bash
# 측정 1회 (V0로 부팅한 VM, root)
sudo ./run-one.sh --expect-release 5.4.214-team-v0 --out ../results/P1/pilot.jsonl \
  -- --workload wa --groups 1024 --rate 50000 --meta phase=P1 --meta rep=1

# R0 탐색 12조건
sudo ./r0_search.py --all --start 50000 --expect-release 5.4.214-team-v0 --out ../results/P2/r0.jsonl

# 분석
../analysis/stats.py ../results/M/*.jsonl --csv ../analysis/summary.csv
```

## mc6bench 동작

- 한 프로세스가 rx namespace에서 돌고, **송신 스레드만 `setns()`로 tx namespace**에 들어갑니다. 송신 순번·시각과 측정 창 경계를 같은 메모리·같은 시계(`CLOCK_MONOTONIC`)로 다룹니다.
- 수신: 모든 소켓 `IPV6_MULTICAST_ALL=0`, `MCAST_JOIN_GROUP`, 비차단 + epoll + `recvmmsg`(64개)
- 송신: `IPV6_MULTICAST_LOOP=0`, `HOPS=1`, `sendmmsg` 배치, 누적 목표 개수 방식 pacing. 부분 성공 시 남은 메시지는 **같은 순번**에 새 시각을 넣어 재시도(순번 중복 없음)
- payload 128B: magic · 그룹 번호 · 순번 · 송신 시각 · 길이 · FNV 검증값
- 측정 창 `[t0, t1)`: 별도 스레드가 경계 시각에 `/proc/stat`의 `cpu` 행을 읽음(경계 지연도 기록)
- 손실·p99: 송신 시각이 창 안인 순번 집합 `[seq_t0, seq_t1)` 기준, drain 끝까지 도착 여부
- `valid` = 기술적으로 쓸 수 있는 실행인지 (가입 실패, 정확성 오류, CPU 읽기 실패, 실제 송신률 1% 이상 차이 → false)
- `stable` = 손실 ≤ 0.01% 그리고 p99 ≤ 10ms (R0 탐색 기준)

## 검증 기록 (2026-11-16)

순정 5.4.214(`9f02aa34e179`, defconfig 기반 + IPv6·NET_NS·VETH·IRQ_TIME_ACCOUNTING·IKCONFIG_PROC)를 QEMU(TCG, 가속 없음, 2 vCPU)로 부팅해 확인했습니다.
**가속 없는 에뮬레이션이라 C 값 자체는 의미가 없고, 도구의 동작만 확인한 것입니다.**

| 사례 | 결과 | 확인한 것 |
|---|---|---|
| W_A G=4, 5000pps | valid, 손실 0, g_if=8 | 기본 그룹 4개 + 4 = G_if 8 |
| W_A G=1024, 5000pps | valid, 손실 0, g_if=1028 | 소켓 1024개 + 고유 포트 |
| W_B G=1024, optmem 262144 | valid, 손실 0, `so_meminfo_optmem=65536` | V0 항목 64B × 1024 (영훈 계산과 일치) |
| W_B G=1024, optmem 20480 | **invalid(join_fail)**, 가입 319개 성공·705개 실패, errno 12(ENOMEM) | V0 가입 한도 319 (영훈 계산과 일치), 실패 감지 |
| W_A G=1, 200000pps | **invalid(tx_rate_off_by_1pct)**, 송신 스레드 CPU 100% | 발생기 한계 감지 |
| run-one.sh release 불일치 | invalid 줄만 남기고 실행 안 함 | 잘못 부팅한 커널 차단 |
| run-one.sh 정상 | `optmem_max_init=262144` 기록, netns 정리 확인 | 메타데이터·정리 |

알려진 점:
- 5.4에서 `net.core.optmem_max`는 전역 값이라 rx namespace 안에서는 읽히지 않습니다(JSON의 `optmem_max`는 null). `run-one.sh`가 바깥 값을 `optmem_max_init`으로 기록합니다.
- `r0_search.py`는 문법 확인까지만 했고, 실제 VM에서 처음 돌릴 때 출력을 함께 확인하세요.
