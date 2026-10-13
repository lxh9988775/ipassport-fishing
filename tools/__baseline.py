#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时脚本：当前参数的基线胜率矩阵（4 lag × 4 rarity × 3 rod），跑完即删。"""
import importlib.util, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tb", os.path.join(HERE, "tune_balance.py"))
tb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tb)   # 会连带加载 vl（verify_logic），保持默认参数
vl = tb.vl

print("当前 RODS:", vl.RODS)
print("当前 SPOTS:", vl.SPOTS)

FULL_LAGS = {"0ms": 1, "80ms": 5, "160ms": 9, "240ms": 13}
NAMES = {0: "常见", 1: "少见", 2: "稀有", 3: "传说"}

for rod in (0, 1, 2):
    print("\n===== %s (bar_h=%d) =====" % (vl.RODS[rod][0], vl.RODS[rod][1]))
    print("{:<6}".format("稀有度") + "".join("%9s" % k for k in FULL_LAGS))
    for r, idxs in tb.REPRESENT.items():
        row = []
        for lname, lg in FULL_LAGS.items():
            pol = tb.policy_lag(lg)
            rates = []
            for i in idxs:
                wins = 0
                for s in range(20):
                    g = vl.Fishing(s * 7919 + i * 31 + 13)
                    g.rod = rod
                    g.cur = i
                    f = vl.SPECIES[i]
                    g.cur_len = f[5] + (f[6] - f[5]) // 2
                    g.reel_enter()
                    g.state = vl.REELING
                    g.enter = g.last
                    now = g.last
                    while now < g.last + 30000 and g.state == vl.REELING:
                        g.holding = pol(g)
                        now += 20
                        g.tick(now)
                    if g.state == vl.CATCH:
                        wins += 1
                rates.append(wins / 20)
            row.append(sum(rates) / len(rates))
        print("{:<6}".format(NAMES[r]) + "".join("%9.0f%%" % (v * 100) for v in row))
