#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 main/fishing_logic.c 读取 24 鱼的 period_pref/weather_pref 真值，
同步到 tools/verify_logic.py 的 SPECIES 元组最后两个字段。
保证 C 与 Python 参考实现的抽鱼分布始终一一对应。改完 C 偏好跑一次即可。"""
import re, io

c = io.open("main/fishing_logic.c", encoding="utf-8").read()
s = c.index("static const fish_species_t SPECIES"); e = c.index("\n};", s)
seg = c[s:e]
BM = {"FISH_PERF_MORNING":1,"FISH_PERF_NOON":2,"FISH_PERF_DUSK":4,"FISH_PERF_NIGHT":8,
      "FISH_WPREF_CLEAR":1,"FISH_WPREF_CLOUDY":2,"FISH_WPREF_RAIN":4}
pref = {}
lines = seg.split("\n")
for i, ln in enumerate(lines):
    if re.match(r"^\s+\d+", ln):
        mm = re.search(r'\{"([^"]+)"', lines[i-1]) or re.search(r'\{"([^"]+)"', lines[i-2])
        toks = re.findall(r"FISH_(?:PERF|WPREF)_\w+", ln)
        pref[mm.group(1)] = (sum(BM[t] for t in toks if "PERF" in t),
                            sum(BM[t] for t in toks if "WPREF" in t))

p = "tools/verify_logic.py"
t = io.open(p, encoding="utf-8").read()
out, seen = [], set()
for ln in t.split("\n"):
    m = re.match(r'^(\s+\("([^"]+)".*),\s*(\d+),\s*(\d+)\),\s*$', ln)
    if m and m.group(2) in pref:
        per, wea = pref[m.group(2)]
        out.append("%s, %d, %d)," % (m.group(1), per, wea))
        seen.add(m.group(2))
    else:
        out.append(ln)
io.open(p, "w", encoding="utf-8", newline="\n").write("\n".join(out))
print("synced", len(seen))
assert set(pref) == seen, "MISSING %s" % (set(pref)-seen)
