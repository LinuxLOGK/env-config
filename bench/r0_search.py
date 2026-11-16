#!/usr/bin/env python3
"""R0 탐색 (절차서 4.2) — V0에서 조건별 안정 최대 송신률을 찾는다.

안정 기준: 손실 <= 0.01%, p99 <= 10ms, 실제 송신률이 목표의 99% 이상 (mc6bench 의 stable·valid 필드).
절차: 20초 시험으로 안정 하한/불안정 상한 찾기(최대 4회) → 20초 이진 탐색(최대 6회) → 60초 x 2회 확인.
송신 스레드 CPU >= 90% 이면 '발생기 한계'로 표시한다.

사용법 (V0로 부팅한 VM에서, root):
  sudo ./r0_search.py --workload wa --groups 1024 --start 50000 --out results/P2/r0.jsonl
  sudo ./r0_search.py --all --start 50000 --out results/P2/r0.jsonl      # 12조건 전부
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SCALES = [1, 4, 16, 64, 256, 1024]


def trial(args, wl, g, rate, secs, tag):
    cmd = [os.path.join(HERE, "run-one.sh"), "--out", args.out]
    if args.expect_release:
        cmd += ["--expect-release", args.expect_release]
    cmd += ["--", "--workload", wl, "--groups", str(g), "--rate", str(int(rate)),
            "--batch", str(args.batch), "--warmup", "10", "--measure", str(secs),
            "--meta", "phase=P2", "--meta", f"r0_step={tag}"]
    subprocess.run(cmd, check=False, stderr=subprocess.DEVNULL)
    with open(args.out) as f:
        last = json.loads(f.readlines()[-1])
    ok = bool(last.get("valid")) and bool(last.get("stable"))
    gen_limited = last.get("tx_thread_cpu_pct", 0) >= 90.0
    print(f"  [{tag}] {wl} G={g} rate={int(rate)} {secs}s -> "
          f"{'STABLE' if ok else 'unstable'} loss={last.get('loss_pct')}% p99={last.get('p99_us')}us "
          f"txcpu={last.get('tx_thread_cpu_pct')}% reason={last.get('invalid_reason')}", flush=True)
    return ok, gen_limited


def search(args, wl, g):
    lo, hi, gen_lim = None, None, False
    rate = args.start
    # 1) 구간 찾기 (최대 4회)
    for i in range(4):
        ok, gl = trial(args, wl, g, rate, 20, f"bracket{i}")
        gen_lim |= gl
        if ok:
            lo = rate
            if hi is not None:
                break
            rate *= 2
        else:
            hi = rate
            if lo is not None:
                break
            rate /= 2
    if lo is None:
        return {"workload": wl, "g_app": g, "r0": None, "status": "no_stable_rate_found", "gen_limited": gen_lim}
    if hi is None:
        status_hi = "search_upper_bound_stable"
    else:
        status_hi = None
        # 2) 이진 탐색 (최대 6회, 상대 폭 2% 이하면 멈춤)
        for i in range(6):
            if (hi - lo) / lo <= 0.02:
                break
            mid = (lo + hi) / 2
            ok, gl = trial(args, wl, g, mid, 20, f"bisect{i}")
            gen_lim |= gl
            if ok:
                lo = mid
            else:
                hi = mid
    # 3) 60초 x 2 확인. 실패하면 5%씩 낮춰 최대 2번 더
    cand = lo
    for attempt in range(3):
        oks = [trial(args, wl, g, cand, 60, f"confirm{attempt}.{k}")[0] for k in range(2)]
        if all(oks):
            return {"workload": wl, "g_app": g, "r0": int(cand), "r_official": int(cand * 0.5),
                    "status": status_hi or "confirmed", "gen_limited": gen_lim}
        cand *= 0.95
    return {"workload": wl, "g_app": g, "r0": None, "status": "confirm_failed", "gen_limited": gen_lim}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workload", choices=["wa", "wb"])
    ap.add_argument("--groups", type=int)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--start", type=float, default=50000)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--expect-release", default="")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    conds = [(w, g) for w in ("wa", "wb") for g in SCALES] if args.all else [(args.workload, args.groups)]
    if not args.all and (not args.workload or not args.groups):
        ap.error("--workload 와 --groups, 또는 --all")
    summary = []
    for wl, g in conds:
        print(f"== R0 search {wl} G={g}", flush=True)
        r = search(args, wl, g)
        print(f"== result {json.dumps(r)}", flush=True)
        summary.append(r)
    path = os.path.splitext(args.out)[0] + "_R0.json"
    with open(path, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"saved {path}")


if __name__ == "__main__":
    main()
