# 성능 측정 절차서 v1 (초안)

Linux 5.4.214 IPv6 멀티캐스트 개선(I·S)의 성능 비교를 위한 측정 절차입니다.
작성: 준엽(측정 설계) · 실행: 태영(VM·측정 운영) · 작성일 2026-11-16 · 상태: **초안, 11/19 리뷰에서 확정**

> 이 문서에서 **"확정 필요"**로 표시한 값은 11/19 리뷰 전까지 바뀔 수 있습니다.
> 나머지 값은 측정을 시작하면 바꾸지 않습니다. 바꿔야 하면 문서를 먼저 개정하고, 영향을 받는 데이터를 다시 수집합니다.

---

## 0. 한눈에 보기

| 항목 | 값 |
|---|---|
| 비교 버전 | **V0 · VL · VI · VIS** (VIr 제외, 11/16 결정) |
| 주 판정 | W_A, G_app=1024, payload 128B, 고정 송신률에서 **V0 대비 VI의 패킷당 커널 CPU 감소율** |
| 목표 | 10블록 대응 감소율의 95% 신뢰구간 **하한 ≥ 20%** |
| 주 지표 C | (system + irq + softirq) CPU 증가 시간 ÷ 측정 창 안에서 정상 수신한 패킷 수 [ns/packet] |
| 송신률 | 조건별 R = 0.5 × R0 (R0 = V0가 안정적으로 받는 최대 송신률) |
| 한 실행 | 준비 → 워밍업 10초 → **측정 60초** → drain → 검증 |
| 반복 | 완전한 시간 블록 **10개**, 블록마다 부팅 순서를 바꿈 |
| 실행 수 | 블록당 38회 × 10 = **380회, 부팅 40회**, 약 12시간 (60초 기준) |

---

## 1. 단계 순서와 통과 조건

측정은 아래 단계를 **순서대로** 진행합니다. 앞 단계의 게이트를 통과하지 못하면 다음 단계로 가지 않습니다.

| 단계 | 내용 | 통과 조건(게이트) | 담당 |
|---|---|---|---|
| **G0 환경** | 측정 호스트, 공통 .config, VM 사양 고정 | 2절 체크리스트 전부 통과 | 태영 (준엽 검토) |
| **P1 예비 판단** | V0에서 W_A 1그룹 vs 1024그룹 규모 민감도 | 1024에서 C가 1그룹보다 뚜렷이 큼 + perf에서 검색 함수가 보임 | 태영 실행 / 준엽 판단 |
| **P2 R0 탐색** | 12조건(W_A/W_B × 6규모)의 V0 안정 최대 송신률 | 12조건 모두 R0 확정 (또는 사유 기록) | 태영 |
| **P3 측정 품질** | 60초 vs 120초, 시작 위상 영향 | 반복 중앙값 차이 ≤ 2% | 태영 실행 / 준엽 판단 |
| **P4 기능 확인** | VI·VIS 이미지에서 해시 경로를 실제로 타는지 | 3.4절 확인 통과 | 태영·종원·영훈 |
| **F 동결** | 코드·config·도구·R·배치·측정 시간·순서표 고정 | 11/27, 커밋과 SHA 기록 | 전원 |
| **R 예행연습** | 4개 이미지 한 바퀴 자동 실행 | 커널 전환·로그·재개가 사람 손 없이 동작 | 태영 |
| **M 공식 측정** | 10블록 수집 | 6절 무효 규칙에 따라 완전한 10블록 확보 | 태영 운영 / 준엽 감시 |
| **A 분석** | 통계·그래프·판정 | 7절 | 준엽 |

---

## 2. G0 — 환경 게이트 (측정 전 필수)

### 2.1 측정 호스트

- [ ] **VirtualBox가 VT-x를 직접 사용** (`VBox.log`에 `fall back to NEM`이 **없을 것**)
  - Windows 호스트라면: `bcdedit /set hypervisorlaunchtype off`, 메모리 무결성(VBS) 끄기, WSL2·"가상 머신 플랫폼"·Hyper-V 기능 끄기 → 재부팅 → VM 실행 후 `VBox.log` 재확인
- [ ] 측정 중 호스트에서 다른 무거운 작업 금지 (빌드, 압축, 동영상, 게임). 절전·자동 업데이트·예약 재부팅 끄기, 전원 연결
- [ ] 측정 중 VM 스냅샷 금지

### 2.2 공통 .config (현재 `V0/common-v5.4.214.config` 기준 변경 사항)

| 옵션 | 현재 | 변경 | 이유 |
|---|---|---|---|
| `CONFIG_IRQ_TIME_ACCOUNTING` | not set | **y** | irq·softirq 시간을 틱 샘플링이 아니라 실제 시각으로 계산. 주 지표의 핵심 |
| `CONFIG_IKCONFIG` / `CONFIG_IKCONFIG_PROC` | m / not set | **y / y** | 실행 중 커널의 config를 `/proc/config.gz`로 매 실행 확인 |
| `CONFIG_VBOXGUEST` 등 VirtualBox 게스트 지원 | 설치됨 | **측정 이미지에서 제외** | Tainted 방지. DKMS `vboxadd`도 측정 커널에 빌드되지 않게 |
| `CONFIG_KASAN`, `CONFIG_PROVE_LOCKING`, `CONFIG_IPV6_MC_IF_LOOKUP_DEBUG` | off | **off 유지** | 성능 이미지에서는 반드시 off |

그대로 두되 기록할 것: `HZ=250`, `PREEMPT_VOLUNTARY`, `NO_HZ_IDLE`, `PAGE_TABLE_ISOLATION=y`, `RETPOLINE=y` (모든 이미지 동일하면 비교에 문제 없음).

- [ ] 위 변경 반영 후 **V0 재빌드**, 설정 SHA-256 갱신, `V0/` 기록 갱신
- [ ] 버전별 config는 **공통 config + 아래 옵션만** 다르게. `scripts/diffconfig`로 확인해 그 외 차이가 0인지 기록

| 버전 | `IPV6_MC_IF_LOOKUP_*` | `IPV6_MC_IF_HASH_THRESH` | `IPV6_MC_SOCK_HASH` | `IPV6_MC_SOCK_HASH_THRESH` | 소스 |
|---|---|---|---|---|---|
| V0 | (옵션 없음) | — | — | — | `baseline-v5.4.214` |
| VL | `_LIST` | — | n | — | 동결 커밋 (integration) |
| VI | `_HASH` | 16 | n | — | 동결 커밋 |
| VIS | `_HASH` | 16 | y | 4 | 동결 커밋 |

- [ ] `LOCALVERSION`: `-team-v0`, `-team-vl`, `-team-vi`, `-team-vis` (make 시 `LOCALVERSION=` 지정, `+` 붙지 않게)
- [ ] 설치 직후 `sha256sum /boot/vmlinuz-*` 기록

### 2.3 VM 사양 (확정 필요)

- 아키텍처 x86_64, Ubuntu 20.04.6, RAM 8GB
- **vCPU: 현재 8개. 4개로 줄일지 11/19까지 결정** (줄이면 유휴 CPU가 줄어 회계 잡음이 작아짐. 정하면 고정)
- 기록: vCPU, 메모리, NIC 모델, offload 설정(`ethtool -k`), MTU, 호스트 CPU 모델

### 2.4 매 부팅 직후 자동 확인 (불일치 시 그 부팅의 실행은 무효)

```bash
uname -r                                   # 기대 release
cat /proc/sys/kernel/random/boot_id        # 로그에 기록
zcat /proc/config.gz | sha256sum           # 기대 config 해시
sha256sum /boot/vmlinuz-$(uname -r)        # 기대 이미지 해시
cat /proc/sys/kernel/tainted               # 0 이어야 함
sysctl -w net.core.optmem_max=262144       # 모든 이미지 동일
getconf CLK_TCK                            # /proc/stat 단위
```

---

## 3. 측정 구성

### 3.1 네트워크

```
[ns tx] veth0 (fd00::1/64) ═══ veth1 (fd00::2/64) [ns rx]
```

- 영훈 `s-fixture/guest/60-netns-fixture.sh`와 같은 구성을 쓰되 **`dummy0`는 만들지 않음** (성능 측정에는 수신 인터페이스 하나)
- rx: `net.ipv6.conf.all.forwarding=0`, DAD 완료 후 시작
- rx 기본 가입 그룹 수(`ip -6 maddr show dev veth1`)를 기록. G_if = G_app + 기본 그룹 수

### 3.2 워크로드

| | W_A (I 검증) | W_B (S 검증) |
|---|---|---|
| 수신 소켓 | G_app개 | 1개 |
| bind | 그룹 주소 + 포트 5000+i | `::` + 포트 5000 |
| 가입 | 소켓 i가 그룹 i 하나 | 소켓 하나가 G_app개 |
| 가입 API | `MCAST_JOIN_GROUP` | `MCAST_JOIN_GROUP` |

- G_app ∈ {1, 4, 16, 64, 256, 1024}
- 그룹 주소: `ff15::1:0` 의 하위 16비트를 0..G_app−1 (조건마다 고정)
- 모든 수신 소켓 `IPV6_MULTICAST_ALL=0`, 송신 소켓 `IPV6_MULTICAST_LOOP=0`, `IPV6_MULTICAST_HOPS=1`
- payload 128B: 순번(u64) · 송신 시각(u64, CLOCK_MONOTONIC ns) · 그룹 번호 · 검증값
- 송신 순서: 그룹을 라운드로빈 (패킷 k → 그룹 k mod G_app)
- 수신: 비차단 + epoll + recvmmsg
- 송신: sendmmsg 배치 (8/32/64 중 P3에서 하나로 고정. **확정 필요**)

### 3.3 실행 1회의 순서

```
부팅 확인(2.4) → netns 구성 → 가입 → 가입 결과·G_if 확인 → 안정화 2초
→ 워밍업 10초 → [t0] 측정 60초 [t1] → drain(최대 2초) → 검증 → 정리 → 로그 1줄
```

- CPU 스냅샷은 t0, t1 경계에서 `/proc/stat`의 `cpu` 행을 읽음
- 같은 부팅 안에서 여러 조건을 실행할 때는 **조건마다 netns를 새로 만들고 지움**

### 3.4 P4 — 해시 경로 확인 (측정 창 밖에서만)

- **VIS (S)**: `getsockopt(SO_MEMINFO)` optmem 값이 `88 × 가입수 + 2064` 인지 확인 (W_B, G_app ≥ 4)
- **VI (I)**: 인터페이스 테이블 생성 여부 확인 방법은 이슈 #2-16 결정에 따름. 결정 전까지는 디버그 빌드(`IPV6_MC_IF_LOOKUP_DEBUG=y`)의 `/proc/net/mc6_if_lookup`으로 P4에서 확인하고, 공식 성능 이미지에는 디버그 옵션을 켜지 않음
- 확인값은 실행 로그에 함께 저장

---

## 4. P1 · P2 · P3 상세

### 4.1 P1 예비 판단 (V0만)

1. W_A, G_app = 1 과 1024를 **같은 송신률**로 각 60초 × 3회
   - 송신률: 1024그룹에서 손실 없이 받는 대략적인 값의 절반 (간이 탐색)
2. D_A = 100 × (1 − C₁ / C₁₀₂₄) 기록 → 규모에 따라 비용이 늘어나는지
3. 별도 실행에서 `perf record -a -g -- sleep 30` → `ipv6_chk_mcast_addr`, `inet6_mc_check` 비중 확인
4. **판단**: D_A가 작고(예: 10% 미만) 검색 함수 비중도 작으면 20% 목표 달성 가능성이 낮다고 팀에 보고하고 범위를 재논의

### 4.2 P2 R0 탐색 (V0, 12조건)

- 안정 기준: 손실률 ≤ 0.01%, p99 응용 지연 ≤ 10ms, 실제 송신률이 목표의 99% 이상
- 송신 스레드 CPU ≥ 90%면 "발생기 한계"로 표시 (수신 한계로 해석하지 않음)
- 절차: 20초 시험으로 안정/불안정 구간 찾기(최대 4회) → 이진 탐색(최대 6회) → 후보값 60초 × 2회 통과 시 R0 확정
- 결과: `R0.csv` (W, G_app, R0, 시도 기록), 공식 송신률 R = 0.5 × R0

### 4.3 P3 측정 품질

- V0와 VI에서 대표 조건(W_A 1024, W_B 1024)을 60초·120초 각 3회
- 반복 중앙값 차이 ≤ 2%이면 60초 확정, 아니면 120초로 통일
- CPU 증가량이 너무 작지 않은지 확인 (측정 창에서 system+irq+softirq 증가가 수천 tick 이상)

---

## 5. 공식 측정 (M)

### 5.1 블록 구성

블록 하나 = 4개 이미지를 아래 순서로 한 번씩 부팅하고, 부팅마다 해당 버전의 조건을 1회씩 실행.

| 블록 | 부팅 순서 |
|---|---|
| 1 | V0 → VL → VIS → VI |
| 2 | VL → VI → V0 → VIS |
| 3 | VI → VL → VIS → V0 |
| 4 | VIS → VI → V0 → VL |
| 5 | VIS → V0 → VL → VI |
| 6 | VI → VIS → VL → V0 |
| 7 | VIS → V0 → VI → VL |
| 8 | V0 → VIS → VL → VI |
| 9 | VL → V0 → VI → VIS |
| 10 | VI → VL → V0 → VIS |

- 한 부팅 안의 조건 실행 순서는 블록별 seed로 섞되, **같은 블록의 모든 버전이 같은 순서**를 씀

### 5.2 버전별 조건

| 묶음 | 버전 | 조건 | 블록당 |
|---|---|---|---|
| W_A 규모 곡선 | V0, VL, VI | 6규모 | 18 |
| W_A S 부작용 확인 | VIS | G_app 1, 1024 | 2 |
| W_B 규모 곡선 | V0, VI, VIS | 6규모 | 18 |
| **합계** | | | **38** |

### 5.3 자동화 (태영)

`grub-reboot <항목>` → 재부팅 → 2.4 확인 → 해당 부팅의 조건 실행 → 로그 저장 → 다음 항목.
불일치·시간 초과 시 자동 진행을 멈추고 상태를 기록. 복구 커널 항목은 항상 GRUB에 남겨 둠.

---

## 6. 무효·실패 규칙 (사전 고정)

| 구분 | 예시 | 처리 |
|---|---|---|
| **정확성 실패** | 내용 오류, 중복, 미가입 그룹 수신 | 해당 버전 실패로 공개. 성능 비교에서 제외 |
| **성능·자원 실패** | 같은 입력에서 손실/지연 기준 초과, 가입 실패 | 해당 조건 실패로 공개 |
| **기술적 무효** | 커널·config 불일치, Tainted≠0, 도구 중단, 확인된 호스트 간섭 | 블록 전체를 같은 순서·seed로 **최대 1회** 재수집. 원본 로그 보존 |
| **조사 신호** | 실제 송신률이 목표와 1% 이상 차이, steal 급증 | 원인 기록 후 판단 |

**결과(개선율, 신뢰구간)를 이유로 재측정하지 않습니다.**

---

## 7. 분석 (A)

- 블록 b의 주 판정 감소율: `d_b = 100 × (1 − C(VI,b) / C(V0,b))`
- 95% 신뢰구간: `d̄ ± 2.262 × s_d / √10` (자유도 9)
- 하한 ≥ 20% → 목표 달성 / 반폭 ≤ 5%p → 정밀도 목표 달성 / 미달이면 미달로 보고 (블록 추가 없음)
- 보조: W_B 1024 V0 대 VIS, VI 대 VIS (S 추가 효과), W_A 1024 V0 대 VL (공통 필드 부담), G_app 1·4 소규모 회귀(≤ 5%)
- 그래프: 그룹 수별 C (x축 로그), 주 판정 블록별 감소율과 CI, 손실·p99 표

---

## 8. 로그 형식

실행 1회 = JSON 한 줄 (`results/<단계>/<날짜>.jsonl`). 측정 도구 `bench/mc6bench`가 출력하고, `bench/run-one.sh`가 config·이미지·도구 해시를 덧붙입니다. 도구 사용법과 5.4.214 검증 기록은 `bench/README.md`에 있습니다. 아래는 주요 필드 예시입니다.

```json
{"tool":"mc6bench","tool_sha":"…","phase":"M","block":3,"boot_seq":2,"kernel":"5.4.214-team-vi",
 "boot_id":"…","config_sha256":"…","image_sha256":"…","tainted":0,"vcpus":8,"clk_tck":100,
 "workload":"wa","g_app":1024,"g_if":1028,"rate_pps":50000,"batch":32,"payload":128,"seed":12345,
 "warmup_s":10,"measure_s":60,"t0_ns":…,"t1_ns":…,
 "cpu_ticks":{"user":…,"nice":…,"system":…,"idle":…,"iowait":…,"irq":…,"softirq":…,"steal":…},
 "tx_ok":…,"tx_fail":…,"tx_actual_pps":…,"tx_thread_cpu_pct":…,
 "rx_in_window":…,"rx_drain":…,"lost":…,"dup":…,"bad":…,"loss_pct":…,"p99_us":…,"lat_samples":…,
 "join_fail":0,"so_meminfo_optmem":…,"C_ns_per_pkt":…,"valid":true,"invalid_reason":null}
```

---

## 9. 저장소 구성 (env-config, 제안)

```
env-config/
├── V0/ VL/ VI/ VIS/        각 이미지: config, SHA, 빌드·설치 로그, 커밋
├── procedure/README.md     이 문서
├── bench/                  mc6bench 소스, netns 스크립트, 자동 실행 스크립트
├── results/P1 P2 P3 M/     원시 JSONL, R0.csv, 무효 기록
└── analysis/               통계 스크립트, 그래프
```

---

## 10. 11/19 리뷰에서 확정할 것

1. vCPU 8 → 4 여부 (2.3)
2. sendmmsg 배치 크기 (3.2, P3 결과로)
3. VI 테이블 확인 방법 (3.4, 이슈 #2-16)
4. 측정 호스트 (2.1)
5. 실제 제출 마감일에 맞춘 일정 (대표 확인)
