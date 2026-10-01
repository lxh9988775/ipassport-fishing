#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tune_balance.py - 收线小游戏「调参台」

为什么要有这个文件：
  收线手感由两组旋钮共同决定，改一个常数就重跑一遍很盲，而且很容易从
  "太简单"直接跳到"打不过"（中间不存在可用区间），所以这里把候选参数打包，
  直接跑出「不同反应延迟 × 不同稀有度」的胜率矩阵，按目标曲线评分。

旋钮分两组：
  A. 手感维度 —— 捕捉区高度 / 竿的加速度 / 鱼的冲刺强度。
     决定"跟不跟手"，改大了就是"按下去追不上"。
  B. 容错维度 —— 鱼的游速 / 脱离后进度掉多快 / 起始进度留多少余量。
     决定"失误几次才跑鱼"。0.1.0 之前只扫了 A 组，结果怎么调都是
     「常见随便钓、稀有传说完全钓不上」—— 因为容错被 B 组锁死了，
     A 组再优化也补不回来。两组的量级必须一起看。

玩家模型：用"延迟 N 个 tick 的观测量"模拟人的反应时间（20ms/tick）：
  0ms=极限操作  80ms=熟练  160ms=普通休闲玩家  240ms=手慢/分心
硬件实际情况：屏幕小、单键长按，160ms 是合理的休闲基准。

目标曲线（普通玩家 160ms）：常见 95% / 少见 85% / 稀有 62% / 传说 45%
硬约束：极限操作下所有稀有度都应能钓上（≥90%），否则鱼就是"不可能"的。
软约束：手慢玩家（240ms）常见鱼也要能上（≥85%），不然新手直接劝退。

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

TARGET = {COMMON: 0.95, UNCOMMON: 0.85, RARE: 0.62, LEGEND: 0.45}
TRIALS = 20

# 扫参时只跑两档省时间；选出最优后再用四档完整复核
SCAN_LAGS = {"0ms": 1, "160ms": 9}
FULL_LAGS = {"0ms": 1, "80ms": 5, "160ms": 9, "240ms": 13}


def policy_lag(lag):
    """延迟 lag 个 tick 才看到鱼的位置 —— 模拟人的反应时间。

    坐标系与 fishing_logic.c / verify_logic.py 一致：bar_pos 越大越靠屏幕下方，
    按住 = 抬竿 = bar_pos 减小。所以「鱼在捕捉区中心上方就按住」等价于
    fish_pos 小于区中心。
    ⚠ 这个不等号曾经跟着写反的物理一起反过：两边同时反，胜率矩阵看着正常，
    但物理一改正就立刻退化成"全 0%"（模型和物理互相拆台）。main() 里的
    selftest() 就是为这种"双反"准备的。
    """
    def p(f):
        if not hasattr(f, "_h"):
            f._h = []
        f._h.append(f.fish_pos)
        old = f._h[-lag] if len(f._h) >= lag else f._h[0]
        return old <= f.bar_pos + f.bar_h // 2
    return p


def selftest():
    """护栏：0 延迟 = 极限操作，必须接近全胜。
    低于阈值说明玩家模型和物理方向不自洽（历史上真发生过），此时所有胜率
    都不可信，必须直接停下来而不是输出一堆漂亮的假数据。"""
    apply_rods(290)
    rate = win_rate(REPRESENT[COMMON][0], policy_lag(1), trials=8)
    if rate < 0.9:
        print("!! 自检失败：极限操作胜率仅 %.0f%%（应接近 100%%）。" % (rate * 100))
        print("   玩家模型与收线物理的方向很可能不一致，先核对 verify_logic.py 的 tgt_v 符号。")
        return False
    return True


def win_rate(idx, pol, trials=TRIALS):
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


def evaluate(lags=None):
    """返回 {lag: {rarity: winrate}}"""
    lags = lags or SCAN_LAGS
    out = {}
    for lname, lg in lags.items():
        pol = policy_lag(lg)
        out[lname] = {}
        for r, idxs in REPRESENT.items():
            out[lname][r] = sum(win_rate(i, pol) for i in idxs) / len(idxs)
    return out


def score(out):
    s = 0.0
    casual = out["160ms"]
    perfect = out["0ms"]
    # 1) 接近目标曲线（普通玩家）
    for r in (COMMON, UNCOMMON, RARE, LEGEND):
        s -= abs(casual[r] - TARGET[r]) * 100
    # 2) 单调递减：越稀有的鱼不能越容易
    order = [casual[r] for r in (COMMON, UNCOMMON, RARE, LEGEND)]
    for a, b in zip(order, order[1:]):
        if b > a + 0.02:
            s -= 40
    # 3) 硬约束：极限操作必须钓得上
    for r in (COMMON, UNCOMMON, RARE, LEGEND):
        s -= max(0.0, 0.90 - perfect[r]) * 150
    # 4) 手慢玩家（240ms）常见鱼也要能上，否则新手直接劝退
    slow = out.get("240ms")
    if slow:
        s -= max(0.0, 0.85 - slow[COMMON]) * 120
    return s


def apply_rods(bar_h):
    """手竿用被扫的区高；另外两根竿保持与仓库一致的相对关系。"""
    vl.RODS = [
        ("手竿",   bar_h,     680, 600, 100),
        ("路亚竿", bar_h - 45, 840, 740, 150),
        ("海竿",   bar_h + 40, 480, 420, 115),
    ]


def main():
    if not selftest():
        return 1

    # 手感维度 × 容错维度一起扫。容错维度是这次新增的：
    #   鱼速 (base, per_diff) / 掉速 (base, power) / 起始进度
    grid = list(itertools.product(
        [290, 330],                          # 捕捉区高度（手竿）
        [6500, 10000],                       # 竿的加速度：越小越沉、越难跟手
        [(130, 8), (140, 10)],               # 冲刺倍率 base + step
        [(230, 2), (270, 2), (320, 3)],      # 鱼速 base + per_diff
        [(70, 110), (95, 140), (120, 180)],  # 掉速 base + power*scale
        [380, 460],                          # 起始进度
    ))

    results = []
    for bar_h, accel, bst, spd, dec, prog in grid:
        apply_rods(bar_h)
        vl.REEL_BAR_ACCEL = accel
        vl.REEL_BURST_MULT_BASE, vl.REEL_BURST_MULT_STEP = bst
        vl.REEL_FISH_SPD_BASE, vl.REEL_FISH_SPD_PER_DIFF = spd
        vl.REEL_DECAY_BASE, vl.REEL_DECAY_POWER = dec
        vl.REEL_PROGRESS_0 = prog
        vl.REEL_DIFF_BASE = 20
        vl.REEL_DIFF_PER_RARITY = 20
        vl.REEL_DIFF_DART = 25
        out = evaluate()
        results.append((score(out), (bar_h, accel, bst, spd, dec, prog), out))

    results.sort(key=lambda x: -x[0])

    head = "{:>7} {:>5} {:>6} {:>10} {:>12} {:>12} {:>5}".format(
        "分数", "区高", "加速度", "冲刺", "鱼速", "掉速", "起始")
    print(head)
    print("-" * len(head))
    for sc, p, _ in results[:10]:
        bar_h, accel, bst, spd, dec, prog = p
        print("{:>7.1f} {:>5} {:>6} {:>10} {:>12} {:>12} {:>5}".format(
            sc, bar_h, accel, str(bst), str(spd), str(dec), prog))

    best_sc, best_p, _ = results[0]
    bar_h, accel, bst, spd, dec, prog = best_p
    print("\n>>> 最优：bar_h=%s accel=%s burst=%s fish_spd=%s decay=%s progress_0=%s"
          % best_p)

    # 用最优参数跑四档完整矩阵复核
    apply_rods(bar_h)
    vl.REEL_BAR_ACCEL = accel
    vl.REEL_BURST_MULT_BASE, vl.REEL_BURST_MULT_STEP = bst
    vl.REEL_FISH_SPD_BASE, vl.REEL_FISH_SPD_PER_DIFF = spd
    vl.REEL_DECAY_BASE, vl.REEL_DECAY_POWER = dec
    vl.REEL_PROGRESS_0 = prog
    full = evaluate(FULL_LAGS)
    print()
    print("{:<6}".format("稀有度") + "".join("%9s" % k for k in FULL_LAGS))
    for r in (COMMON, UNCOMMON, RARE, LEGEND):
        print("{:<6}".format(NAMES[r]) + "".join("%9.0f%%" % (full[k][r] * 100)
                                                 for k in FULL_LAGS))

    print("\n写到 C 层时对齐这些常量：")
    print("  REEL_FISH_SPD_BASE =", spd[0], " REEL_FISH_SPD_PER_DIFF =", spd[1])
    print("  REEL_DECAY_BASE =", dec[0], " REEL_DECAY_POWER =", dec[1])
    print("  REEL_PROGRESS_0 =", prog)
    print("  REEL_BAR_ACCEL =", accel)
    print("  REEL_BURST_MULT_BASE =", bst[0], " REEL_BURST_MULT_STEP =", bst[1])
    print("  RODS bar_h =", bar_h, "(路亚 %d / 海竿 %d)" % (bar_h - 45, bar_h + 40))
    return 0


if __name__ == "__main__":
    sys.exit(main())
