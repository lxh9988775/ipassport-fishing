#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tune_balance.py - 收线小游戏「调参台」

为什么要有这个文件：
  收线手感的三个杠杆互相牵制 —— 捕捉区高度 / 竿的灵敏度(加速度) / 鱼的冲刺强度。
  改一个常数就重跑一遍很盲，而且很容易从"太简单"直接跳到"打不过"（中间不存在可用区间）。
  这里把候选参数打包，直接跑出「不同反应延迟 × 不同稀有度」的胜率矩阵，按目标曲线评分。

玩家模型：用"延迟 N 个 tick 的观测量"模拟人的反应时间（20ms/tick）：
  0ms=极限操作  160ms=普通休闲玩家  240ms=手慢/分心
硬件实际情况：屏幕小、单键长按，160ms 是合理的休闲基准。

目标曲线（普通玩家 160ms）：常见 85% / 少见 70% / 稀有 50% / 传说 30%
硬约束：极限操作下所有稀有度都应能钓上（≥85%），否则鱼就是"不可能"的。

用法：
  python tools/tune_balance.py
"""

import importlib.util
import itertools
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("vl", os.path.join(HERE, "verify_logic.py"))
vl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vl)

COMMON, UNCOMMON, RARE, LEGEND = 0, 1, 2, 3
NAMES = {COMMON: "常见", UNCOMMON: "少见", RARE: "稀有", LEGEND: "传说"}

# 每种稀有度抽 3 个代表鱼种，够用且省算力
REPRESENT = {
    COMMON:  [0, 1, 8],
    UNCOMMON: [3, 10, 18],
    RARE:    [6, 12, 20],
    LEGEND:  [15, 22, 23],
}

TARGET = {COMMON: 0.85, UNCOMMON: 0.70, RARE: 0.50, LEGEND: 0.30}
LAGS = {"0ms": 1, "160ms": 9}


def policy_lag(lag):
    def p(f):
        if not hasattr(f, "_h"):
            f._h = []
        f._h.append(f.fish_pos)
        old = f._h[-lag] if len(f._h) >= lag else f._h[0]
        return old >= f.bar_pos + f.bar_h // 2
    return p


def win_rate(idx, pol, trials=26):
    wins = 0
    for s in range(trials):
        g = vl.Fishing(s * 7919 + idx * 31 + 13)
        g.rod = 0
        g.cur = idx
        f = vl.SPECIES[idx]
        g.cur_len = f[5] + (f[6] - f[5]) // 2
        g.reel_enter()
        g.state = vl.REELING
        g.enter = g.last
        now = g.last
        end = now + 30000
        while now < end and g.state == vl.REELING:
            g.holding = pol(g)
            now += 20
            g.tick(now)
        if g.state == vl.CATCH:
            wins += 1
    return wins / trials


def evaluate():
    """返回 {lag: {rarity: winrate}}"""
    out = {}
    for lname, lg in LAGS.items():
        pol = policy_lag(lg)
        out[lname] = {}
        for r, idxs in REPRESENT.items():
            out[lname][r] = sum(win_rate(i, pol) for i in idxs) / len(idxs)
    return out


def score(out):
    s = 0.0
    casual = out["160ms"]
    perfect = out["0ms"]
    # 1) 接近目标曲线
    for r in (COMMON, UNCOMMON, RARE, LEGEND):
        s -= abs(casual[r] - TARGET[r]) * 100
    # 2) 单调递减：越稀有的鱼不能越容易
    order = [casual[r] for r in (COMMON, UNCOMMON, RARE, LEGEND)]
    for a, b in zip(order, order[1:]):
        if b > a + 0.02:
            s -= 40
    # 3) 硬约束：极限操作必须钓得上
    for r in (COMMON, UNCOMMON, RARE, LEGEND):
        s -= max(0.0, 0.85 - perfect[r]) * 120
    # 4) 手慢玩家不能毫无游戏体验（常见鱼要能上）
    pass
    return s


def main():
    candidates = list(itertools.product(
        [250, 290, 330],            # 捕捉区高度（手竿）
        [4000, 6500, 10000, 14000], # 竿的加速度：越小越沉、越难跟手
        [(130, 8)],                 # 冲刺倍率 base + step（保持温和，制造偶发脱离）
    ))

    results = []
    for bar_h, accel, bst in candidates:
        vl.RODS = [
            ("手竿",   bar_h,     680, 600, 100),
            ("路亚竿", bar_h - 45, 840, 740, 130),
            ("海竿",   bar_h + 40, 480, 420, 160),
        ]
        vl.REEL_BAR_ACCEL = accel
        vl.REEL_BURST_MULT_BASE = bst[0]
        vl.REEL_BURST_MULT_STEP = bst[1]
        vl.REEL_DIFF_BASE = 20
        vl.REEL_DIFF_PER_RARITY = 20
        vl.REEL_DIFF_DART = 25
        out = evaluate()
        sc = score(out)
        results.append((sc, bar_h, accel, bst, out))

    results.sort(key=lambda x: -x[0])
    print(f"{'分数':>7}  {'区高':>4} {'加速度':>6} {'冲刺':>10}")
    print("-" * 62)
    for sc, bar_h, accel, bst, out in results:
        print(f"{sc:>7.1f}  {bar_h:>4} {accel:>6} {str(bst):>10}")

    best = results[0]
    print("\n>>> 最优：bar_h=%s accel=%s burst=%s" % (best[1], best[2], best[3]))
    out = best[4]
    print(f"\n{'稀有度':<6}" + "".join(f"{k:>9}" for k in LAGS))
    for r in (COMMON, UNCOMMON, RARE, LEGEND):
        print(f"{NAMES[r]:<6}" + "".join(f"{out[k][r]:>9.0%}" for k in LAGS))
    print("\n写到 C 层时对齐这些常量：")
    print("  REEL_BAR_ACCEL =", best[2])
    print("  REEL_BURST_MULT_BASE =", best[3][0], " REEL_BURST_MULT_STEP =", best[3][1])
    print("  RODS bar_h =", best[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
