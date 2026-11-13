# env-config

## Linux 5.4.214 V0 환경 설정 및 기능 검증

개선 전 기준 커널 V0의 공통 빌드 설정, 재현 정보, 시험 도구와 검증 기록을 보관합니다.
V0 빌드·설치·실제 부팅·IPv6 멀티캐스트 1그룹 기능 검증을 완료했습니다. 정식 성능 측정 결과는 아직 없습니다.

### 기준과 환경

| 항목 | 값 |
| --- | --- |
| V0 커밋 | `9f02aa34e179d1b0185a27980dd5bafd0f2e6e59` |
| 커널 버전 / 실행 release | `5.4.214` / `5.4.214-team-v0` |
| 공통 설정 | `common-v5.4.214.config` |
| 설정 SHA-256 | `733c640517c41e80d9cb5ab03e6547945f7e2c0703ebe483785f125883d4eb45` |
| 배포판 / 아키텍처 | Ubuntu 20.04.6 LTS / x86_64 |
| VM 자원 | vCPU 8, RAM 7.8Gi, swap 2.0Gi |
| 툴체인 | GCC 9.4.0, GNU ld 2.34, GNU Make 4.2.1, Python 3.8.10 |
| 기능 검증일 | 2026-11-14 |

[고정한 V0 소스 커밋](https://github.com/LinuxLOGK/linux/commit/9f02aa34e179d1b0185a27980dd5bafd0f2e6e59)을 기준으로 사용합니다.
원본 커널 소스와 설치 바이너리는 이 환경 기록 저장소에 포함하지 않습니다.

### 설정 변경

- 정상 부팅 설정을 바탕으로 `CONFIG_LOCALVERSION`을 빈 문자열로 정리하고 `CONFIG_VETH=y`를 활성화했습니다.
- `IPV6`, `NAMESPACES`, `NET_NS`, `VETH`는 모두 `y`입니다.
- `LOCALVERSION_AUTO`와 `DEBUG_INFO`는 비활성화했습니다.
- 루트 ext4·디스크·initramfs 지원은 유지했고, 빌드·설치에 `LOCALVERSION=-team-v0`를 사용했습니다.
- 설정 변경 후 `make LOCALVERSION=-team-v0 prepare`로 생성 설정을 동기화하고 실제 release를 확인했습니다.
- `config-changes.diff`는 이전 로컬 이름과 경로를 익명화한 참고용 변경 내역입니다.

공통 설정은 실제 검증한 파일과 바이트 단위로 동일합니다. 다른 하드웨어에서는 필요한 부팅 드라이버를 별도로 검토해야 합니다.

### 실제 1그룹 시험 결과

```text
sent_joined=5, sent_unjoined=5
received_joined=5, received_unjoined=0
PASS: joined payload received, unjoined payload absent
```

- 가입 그룹: `ff12::1234`, 미가입 그룹: `ff12::5678`, UDP 포트: `54021`.
- 시험 후 임시 namespace·veth·수신 프로세스·임시 폴더가 남아 있지 않음을 확인했습니다.
- 통과 기준은 가입 payload 1개 이상·미가입 0개입니다. 이번 결과를 성능·손실률·대규모 가입 검증으로 확대하지 않습니다.

### 파일 구성

| 파일 | 내용 |
| --- | --- |
| `common-v5.4.214.config`, `.sha256` | 검증한 공통 설정과 해시 |
| `config-changes.diff` | 익명화한 설정 변경 내역 |
| `metadata.json`, `expected-release.txt`, `toolchain.txt` | 기준 커밋·환경·검증 결과·툴체인 |
| `ipv6-one-group.sh`, `ipv6-one-group.py` | 실제 사용한 1그룹 기능 시험 도구 |
| `logs/build.log`, `logs/install.log`, `logs/test.log` | 공개용으로 익명화한 실행 로그 |
| `logs/host-virtualization.txt` | 호스트 가상화 실행 모드 확인 |
| `SHA256SUMS` | 현재 공개 파일들의 상대경로 기준 체크섬 |

### 파일 검증과 기능 시험 재실행

파일 체크섬은 저장소 루트에서 검증합니다.

```bash
sha256sum -c SHA256SUMS
```

시험은 준비된 V0 Linux 게스트에서 실행합니다. `iproute2`, Python 3, sudo 권한이 필요합니다.

```bash
test "$(uname -r)" = 5.4.214-team-v0 &&
sudo bash ./ipv6-one-group.sh
```

시험은 독립된 임시 네트워크를 만들고 종료 시 정리합니다. 새 실행 로그에는 로컬 식별 정보가 들어갈 수 있으므로 공개 기록은 같은 방식으로 익명화합니다.

### 정식 측정 전 남은 조건

1. 확인한 호스트는 VirtualBox NEM fallback 모드입니다. 정식 측정에 사용할 호스트 실행 환경을 확정해야 합니다.
2. 검증 당시 `net.core.optmem_max=20480`입니다. W_B 전 모든 비교 이미지에 `262144`를 동일 적용하고 실제 시험 환경에서 확인해야 합니다.
3. VI/VIS 고정 커밋·활성화/DEBUG 옵션·측정 부하·지표·반복 횟수·절차를 확정해야 합니다.
4. 이 도구는 `IPV6_JOIN_GROUP`을 사용하고 `IPV6_MULTICAST_ALL=0`을 지정하지 않습니다. W_B용 도구와 구분합니다.
5. 1,024그룹 예비 시험과 정식 V0/VI/VIS 성능 비교는 아직 실행하지 않았습니다.

설치 중 Guest Additions 추가 모듈 빌드 오류 이력은 `logs/install.log`에 보존했습니다. 이후 실제 V0 부팅·SSH·기본 송수신은 검증했지만, 모든 부가 기능을 시험하거나 추가 빌드 오류를 해결했다고 주장하지 않습니다.

### 공개 기록 처리

개인 이름·계정·호스트명·개인 절대경로·boot ID·파일시스템 UUID·개인 문서 링크·이전 개인 커널 식별 정보는 제거하거나 명시적인 자리표시자로 치환했습니다.
`logs/`와 diff는 익명화 사본이며, 원본 그대로의 로그라는 의미는 아닙니다. 커밋·공통 설정·시험 코드·실제 송수신 수·오류 내용 등 기술적 근거는 유지했습니다.
이전 개인 커널 파일의 해시 목록은 공개 대상에서 제외했습니다. 공개 파일 변경에 맞춰 `SHA256SUMS`를 다시 생성했습니다.
