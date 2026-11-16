#!/usr/bin/env python3
"""공식 측정 분석 (절차서 7절).

블록 b의 감소율 d_b = 100 * (1 - C(test,b) / C(base,b)), 95% CI = mean ± t(0.975, n-1) * sd / sqrt(n).
반복 단위는 블록이다. 같은 블록·버전·조건에 유효 실행이 여러 개면 오류로 멈춘다(사전 규칙상 1회).

사용법:
  ./stats.py results/M/*.jsonl                       # 사전 고정 판정 전부
  ./stats.py results/M/*.jsonl --csv summary.csv     # 조건별 표도 저장
"""
import argparse, csv, json, math, statistics, sys
from collections import defaultdict

# t(0.975, df) for df = 1..30
T975 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
        2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
        2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]

VERSION_OF = {"team-v0": "V0", "team-vl": "VL", "team-vi": "VI", "team-vis": "VIS"}

# (이름, workload, g_app, base, test, 목표 하한 %)
JUDGEMENTS = [
    ("주 판정",          "wa", 1024, "V0", "VI",  20.0),
    ("보조 판정",        "wb", 1024, "V0", "VIS", None),
    ("S 추가 효과",      "wb", 1024, "VI", "VIS", None),
    ("공통 변경 부담",   "wa", 1024, "V0", "VL",  None),
]


def version(kernel):
    for k, v in VERSION_OF.items():
        if kernel.endswith(k):
            return v
    return kernel


def load(paths):
    runs = defaultdict(list)  # (block, ver, wl, g) -> [C]
    rows = []
    for p in paths:
        with open(p) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                r = json.loads(line)
                if r.get("phase") != "M" or not r.get("valid"):
                    continue
                key = (int(r["block"]), version(r["kernel"]), r["workload"], int(r["g_app"]))
                runs[key].append(float(r["C_ns_per_pkt"]))
                rows.append(r)
    for k, v in runs.items():
        if len(v) != 1:
            sys.exit(f"오류: {k} 유효 실행이 {len(v)}개 (블록당 1개여야 함)")
    return {k: v[0] for k, v in runs.items()}, rows


def paired(C, wl, g, base, test):
    blocks = sorted({b for (b, v, w, gg) in C if w == wl and gg == g})
    d = []
    for b in blocks:
        cb, ct = C.get((b, base, wl, g)), C.get((b, test, wl, g))
        if cb is None or ct is None:
            continue
        d.append(100.0 * (1.0 - ct / cb))
    return d


def ci(d):
    n = len(d)
    if n < 2:
        return None
    m, s = statistics.mean(d), statistics.stdev(d)
    h = T975[n - 2] * s / math.sqrt(n)
    return m, m - h, m + h, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--csv")
    ap.add_argument("--blocks", type=int, default=10, help="완전한 블록 수(사전 고정)")
    a = ap.parse_args()
    C, rows = load(a.files)

    for name, wl, g, base, test, goal in JUDGEMENTS:
        d = paired(C, wl, g, base, test)
        print(f"\n[{name}] {wl.upper()} G={g}  {base} → {test}")
        if len(d) < a.blocks:
            print(f"  완전한 블록 {len(d)}/{a.blocks} — 판정 보류")
            if len(d) < 2:
                continue
        r = ci(d)
        m, lo, hi, h = r
        print("  블록별 감소율: " + ", ".join(f"{x:.1f}" for x in d))
        print(f"  평균 {m:.2f}%  95% CI [{lo:.2f}, {hi:.2f}]  반폭 {h:.2f}%p  "
              f"{'(정밀도 목표 달성)' if h <= 5 else '(정밀도 목표 미달)'}")
        if goal is not None and len(d) >= a.blocks:
            print(f"  → 목표(하한 ≥ {goal:.0f}%): {'달성' if lo >= goal else '미달'}")

    # 소규모 회귀: G 1·4 에서 실험군이 V0 대비 5% 넘게 느려지는지
    print("\n[소규모 회귀] (음수 = 실험군이 느림, 기준: CI 하한 > -5%)")
    for wl, test in (("wa", "VL"), ("wa", "VI"), ("wb", "VI"), ("wb", "VIS")):
        for g in (1, 4):
            d = paired(C, wl, g, "V0", test)
            r = ci(d)
            if r:
                print(f"  {wl.upper()} G={g:<4} V0→{test:<3} 평균 {r[0]:6.2f}%  CI [{r[1]:6.2f}, {r[2]:6.2f}]  "
                      f"{'OK' if r[1] > -5 else '회귀 의심'}")

    if a.csv:
        agg = defaultdict(list)
        for (b, v, w, g), c in C.items():
            agg[(v, w, g)].append(c)
        with open(a.csv, "w", newline="") as f:
            wr = csv.writer(f)
            wr.writerow(["version", "workload", "g_app", "n_blocks", "C_mean_ns", "C_sd_ns"])
            for (v, w, g), cs in sorted(agg.items(), key=lambda x: (x[0][1], x[0][2], x[0][0])):
                wr.writerow([v, w, g, len(cs), f"{statistics.mean(cs):.2f}",
                             f"{statistics.stdev(cs):.2f}" if len(cs) > 1 else ""])
        print(f"\nsaved {a.csv}")


if __name__ == "__main__":
    main()
