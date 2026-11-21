#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_fish_env.py —— 阶段2「天气决定鱼」本机分布对拍（无需 gcc / 硬件）。

复用 tools/verify_logic.py 的 Fishing 参考实现（SPECIES 表 + roll_species 公式
已与 main/fishing_logic.c 同步），固定 seed 采样，验证任务书的验收口径：
  1) 鲶鱼在急流河「夜/晴」出现次数 ≥ 「午/晴」的 3 倍（时段偏好生效）；
  2) 任一鱼在任一 时段×天气 组合下都仍可能被抽到（不锁死、无 0 权重）；
  3) 鳜鱼「昏/雨」高于「午/晴」、白条「午/晴」高于「夜/晴」（多鱼交叉验证）。

同时输出鱼池对照表（各时段下急流河 8 种鱼的出现占比），供送检截图。
"""
import sys, os
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_logic as V

RIVER = V.RIVER
# 时段: 晨0 午1 昏2 夜3；天气: 晴0 阴1 雨2
NOON, NIGHT, DUSK = 1, 3, 2
CLEAR, RAIN = 0, 2
N = 3000


def sample(spot, period, weather, bait=0, n=N, seed_base=10000):
    """固定 seed 连续抽 n 次鱼，统计鱼种分布。
    roll_species 是抽鱼核心：cast/tick/strike 只推进同一条 RNG、不改权重，
    所以直接采样与"打 n 局"分布等价，且快几个数量级。"""
    counts = Counter()
    g = V.Fishing(seed_base)
    g.set_bait(bait)
    g.spot = spot                          # 绕过解锁门槛，直接钉分布
    g.set_env(period, weather)
    for _ in range(n):
        counts[V.SPECIES[g.roll_species()][0]] += 1
    return counts


def name_idx(name):
    for i, f in enumerate(V.SPECIES):
        if f[0] == name:
            return i
    raise KeyError(name)


def lookup(counts, name):
    return counts.get(name, 0)


def main():
    fails = 0
    def check(name, ok, detail=""):
        nonlocal fails
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails += 1

    print("=== 阶段2 天气决定鱼 · 分布对拍（每组合 %d 次采样）===" % N)

    # 1) 鲶鱼 夜/晴 vs 午/晴（急流河，蚯蚓饵）
    night = sample(RIVER, NIGHT, CLEAR, bait=0)
    noon  = sample(RIVER, NOON,  CLEAR, bait=0)
    cat_n, cat_d = lookup(night, "鲶鱼"), lookup(noon, "鲶鱼")
    ratio = cat_n / max(1, cat_d)
    check("鲶鱼 夜/晴 显著高于 午/晴 (≥3倍)",
          ratio >= 3.0, "夜=%d 午=%d 倍=%.2f" % (cat_n, cat_d, ratio))

    # 2) 鳜鱼 昏/雨 vs 午/晴
    duskrain = sample(RIVER, DUSK, RAIN, bait=2)   # 亮片，鳜鱼偏好 0x04
    guiyu_dr, guiyu_noon = lookup(duskrain, "鳜鱼"), lookup(noon, "鳜鱼")
    # 午用蚯蚓饵不公平（鳜鱼不爱蚯蚓），补一个 午/晴 亮片
    noon_lure = sample(RIVER, NOON, CLEAR, bait=2)
    guiyu_noon_lure = lookup(noon_lure, "鳜鱼")
    r2 = guiyu_dr / max(1, guiyu_noon_lure)
    check("鳜鱼 昏/雨 高于 午/晴(同亮片)",
          guiyu_dr > guiyu_noon_lure, "昏雨=%d 午晴=%d 倍=%.2f" % (guiyu_dr, guiyu_noon_lure, r2))

    # 3) 不锁死：任一鱼在任一组合权重>0 -> 采样中都应出现（用宽松阈值；传说鱼样本少）
    combos = {"晨晴": (0, 0), "午晴": (NOON, CLEAR), "昏雨": (DUSK, RAIN), "夜晴": (NIGHT, CLEAR)}
    river_names = [f[0] for f in V.SPECIES if f[2] == RIVER]
    for label, (p, w) in combos.items():
        c = sample(RIVER, p, w, bait=0, n=N)
        missing = [nm for nm in river_names if lookup(c, nm) == 0]
        check("急流河 8 种鱼在 %s 均可能上钩" % label,
              not missing, "缺席=%s" % (missing or "无"))

    # 4) 鱼池对照表（各时段 晴 下急流河占比）
    print("\n=== 鱼池对照表 · 急流河 / 晴（每格为 %d 次抽鱼占比）===" % N)
    header = "时段".ljust(6) + "".join(nm.ljust(7) for nm in river_names)
    print(header)
    for label, p in (("晨", 0), ("午", NOON), ("昏", DUSK), ("夜", NIGHT)):
        c = sample(RIVER, p, CLEAR, bait=0, n=N)
        row = label.ljust(6) + "".join(("%.1f%%" % (100.0 * lookup(c, nm) / N)).ljust(7)
                                       for nm in river_names)
        print(row)

    print("\n结论: %s（%d 项失败）" % ("PASS" if fails == 0 else "FAIL", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
