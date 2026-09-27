#!/usr/bin/env python3
"""录入相似帧拒绝阈值的离线标定（docs/optimization-plan.md P1-B）。

对标 Windows HIGHLY_SIMILARITY 判定：区分两类同人同指帧对——
  same-press:  一次按压内采集的多帧（verify-run-<stamp>-N 目录内两两）
               → 高相似，录入时应被拒绝
  cross-press: 不同按压的帧（跨目录；同人同指）
               → 应入池，构成画廊覆盖

用 C 引擎（tools/egis0575-matcher-test，先 make）对采样帧对打分，
输出两分布统计与建议阈值（cross-press P90，保守值）。

用法：python3 scripts/calibrate-enroll-sim.py [--pairs 200] [--seed 42]
"""

import argparse
import itertools
import pathlib
import random
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "egis0575-matcher-test"
DATA = ROOT / "datasets"

# verify-run-<stamp>-<N>：stamp 新格式 YYYYMMDD-HHMMSS（2026-09-14 起，
# 见 test-enroll-verify.sh），旧格式 HHMMSS（历史数据集）——两种都收
PAT = re.compile (r"^verify-run-(\d{8}-\d{6}|\d{6})-(\d+)$")


def run_score (a: pathlib.Path, b: pathlib.Path) -> int:
    out = subprocess.run ([str (TOOL), "score", str (a), str (b)],
                          capture_output = True, text = True, check = True)
    m = re.search (r"score=(-?\d+)", out.stdout)
    if not m:
        raise RuntimeError (f"无法解析输出: {out.stdout!r}")
    return int (m.group (1))


def collect_presses ():
    """返回 [(tag, [pgm...])]：每个 verify-run 目录一次按压。"""
    presses = []
    for d in sorted (DATA.glob ("verify-run-*")):
        m = PAT.match (d.name)
        if not m:
            continue
        pgms = sorted (d.glob ("probe-*.pgm"))
        if len (pgms) >= 2:
            presses.append ((d.name, pgms))
    return presses


def stats (name, vals):
    if not vals:
        print (f"{name}: （无样本）")
        return
    sv = sorted (vals)

    def pct (p):
        return sv[min (len (sv) - 1, int (round (p / 100 * (len (sv) - 1))))]

    print (f"{name}: n={len(sv)} min={sv[0]} p10={pct(10)} p25={pct(25)} "
           f"p50={pct(50)} p75={pct(75)} p90={pct(90)} max={sv[-1]}")
    # 简易文本直方图（20 分一档）
    lo, hi = min (0, sv[0]), sv[-1]
    if hi == lo:
        return
    buckets = [0] * 20
    for v in vals:
        buckets[min (19, (v - lo) * 20 // (hi - lo + 1))] += 1
    for i, c in enumerate (buckets):
        if c:
            print (f"  {lo + i * (hi - lo + 1) // 20:4d}-{lo + (i + 1) * (hi - lo + 1) // 20:4d}: {'#' * c} ({c})")


def main ():
    ap = argparse.ArgumentParser ()
    ap.add_argument ("--pairs", type = int, default = 200,
                     help = "各类分布的采样对数上限")
    ap.add_argument ("--seed", type = int, default = 42)
    args = ap.parse_args ()

    if not TOOL.exists ():
        sys.exit ("请先构建工具：cd tools && make")

    presses = collect_presses ()
    if len (presses) < 2:
        sys.exit ("需要 ≥2 个含多帧 probe 的 verify-run 目录（先跑 "
                  "test-enroll-verify.sh verify 收集）")

    rng = random.Random (args.seed)
    total = sum (len (p) for _, p in presses)
    print (f"语料：{len(presses)} 次按压 / {total} 帧\n")

    # same-press：组内两两（每按压本身就是同一次放置）。与 cross-press 一样
    # 按 --pairs 采样封顶——单次按压帧多时全量组合会爆炸（每对一个子进程调用）
    same_pairs = []
    for _, pgms in presses:
        same_pairs.extend (itertools.combinations (pgms, 2))
    rng.shuffle (same_pairs)
    same_pairs = same_pairs[:args.pairs]
    # cross-press（确定同指）：同一会话时间戳前缀内跨按压采样。
    # 跨会话的对可能混有异指（历史冒充测试语料），单独归入参考分布。
    by_session = {}
    for tag, pgms in presses:
        by_session.setdefault (tag.rsplit ("-", 1)[0], []).append (pgms)
    cross_pairs, mixed_pairs = [], []
    for sess, groups in by_session.items ():
        if len (groups) < 2:
            continue
        for p1, p2 in itertools.combinations (groups, 2):
            cross_pairs.append ((rng.choice (p1), rng.choice (p2)))
    all_press_pairs = list (itertools.combinations (presses, 2))
    rng.shuffle (all_press_pairs)
    for (t1, p1), (t2, p2) in all_press_pairs[:args.pairs]:
        if t1.rsplit ("-", 1)[0] != t2.rsplit ("-", 1)[0]:
            mixed_pairs.append ((rng.choice (p1), rng.choice (p2)))
    rng.shuffle (cross_pairs)
    cross_pairs = cross_pairs[:args.pairs]

    print (f"打分：same-press {len(same_pairs)} 对 + cross-press(同指) {len(cross_pairs)} 对"
           f" + 跨会话参考 {len(mixed_pairs)} 对 …\n")
    same_scores = [run_score (a, b) for a, b in same_pairs]
    cross_scores = [run_score (a, b) for a, b in cross_pairs]
    mixed_scores = [run_score (a, b) for a, b in mixed_pairs]

    stats ("same-press（同按压，应拒绝）", same_scores)
    print ()
    stats ("cross-press（同会话跨按压=同指，应入池）", cross_scores)
    print ()
    stats ("跨会话参考（可能混异指，不用于定阈值）", mixed_scores)
    print ()

    if cross_scores:
        cs = sorted (cross_scores)

        def pct (p):
            return cs[min (len (cs) - 1, int (round (p / 100 * (len (cs) - 1))))]

        same_max_cross = max (cross_scores)
        for cand, label in ((max (same_max_cross + 1, pct (99)), "跨按压最大值之上（零误杀）"),
                            (pct (95), "P95（近零误杀）"),
                            (pct (90), "P90（保守）")):
            print (f"阈值候选 {cand}（{label}）: "
                   f"same-press 被拒 {sum(1 for s in same_scores if s >= cand)}/{len(same_scores)}, "
                   f"cross-press 误杀 {sum(1 for s in cross_scores if s >= cand)}/{len(cross_scores)}")
        print ("\n建议：两分布分离时取 cross-press 最大值之上；分离不足时退 P90。"
               "驱动侧 EGIS0575_ENROLL_SIM_THRESHOLD 可调，0=关闭。")


if __name__ == "__main__":
    main ()
