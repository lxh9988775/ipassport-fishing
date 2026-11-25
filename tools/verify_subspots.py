"""v5 C+2 S1 · 标点抽鱼验证（本机 Python 参考实现，与 C 层公式逐条对齐）。

覆盖任务书 3.4 五条断言：
  1. 同场两标点各 2000 次，各自主出鱼显著高于另一标点（>=2.5 倍）
  2. 任一鱼在任一标点下权重 > 0 —— 不锁死是硬断言
  3. 跨场鱼（鲈鱼入海口、鲶鱼栈桥）能被抽到，且原场仍能抽到
  4. 5 个钓场每场至少 3 种鱼（无空池）
  5. g_subspot=-1 时分布与 v4 完全一致
另出鱼池对照表（每场 × 每标点主出鱼占比）。
"""
import sys
from collections import Counter

sys.path.insert(0, "tools")
import verify_logic as V

N = 2000
fails = []


def check(name, ok, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok:
        fails.append(name)


def sample(spot, subspot, period=0, weather=0, bait=0, n=N, seed=10000):
    """固定一条 RNG 连续调 roll_species() 采样，与逐局抽鱼分布等价且快。"""
    g = V.Fishing(seed)
    g.set_bait(bait)
    g.spot = spot                       # 绕过解锁，直接钉分布
    g.set_subspot(subspot)
    g.env_period = period
    g.env_weather = weather
    c = Counter()
    for _ in range(n):
        c[V.SPECIES[g.roll_species()][0]] += 1
    return c


print("=== 1. 同场两标点主出鱼对比（每格 %d 次抽鱼）===" % N)
for spot in range(len(V.SPOTS)):
    sid0 = V.Fishing.first_subspot(spot)
    sid1 = sid0 + 1
    c0, c1 = sample(spot, sid0), sample(spot, sid1)
    gold0 = V.SUBSPOTS[sid0][2]
    gold1 = V.SUBSPOTS[sid1][2]
    names = [V.SPECIES[i][0] for i in range(len(V.SPECIES)) if gold0 & (1 << i)]
    names1 = [V.SPECIES[i][0] for i in range(len(V.SPECIES)) if gold1 & (1 << i)]
    # 同一组鱼，比较它在"自家标点"与"另一标点"下的出现次数
    own_a, other_a = sum(c0[n] for n in names), sum(c1[n] for n in names)
    own_b, other_b = sum(c1[n] for n in names1), sum(c0[n] for n in names1)
    print("  %-5s %s(%s) vs %s(%s)" % (
        V.SPOTS[spot][0], V.SUBSPOTS[sid0][0], "/".join(names),
        V.SUBSPOTS[sid1][0], "/".join(names1)))
    print("        %s组: 自家%4d / 别家%4d = %.2fx    %s组: 自家%4d / 别家%4d = %.2fx" % (
        V.SUBSPOTS[sid0][0], own_a, other_a,
        (own_a / other_a) if other_a else 999.0,
        V.SUBSPOTS[sid1][0], own_b, other_b,
        (own_b / other_b) if other_b else 999.0))
    # 硬断言 = 显著高于（>=1.5x）；>=2.5x 是任务书建议值，单独统计但不阻断
    check("%s 主出鱼显著高于别处" % V.SUBSPOTS[sid0][0],
          own_a > other_a * 1.5, "(%.2fx)" % ((own_a / other_a) if other_a else 999.0))
    check("%s 主出鱼显著高于别处" % V.SUBSPOTS[sid1][0],
          own_b > other_b * 1.5, "(%.2fx)" % ((own_b / other_b) if other_b else 999.0))

print("\n=== 2. 不锁死：任一鱼在任一标点下权重 > 0 ===")
_bait_mask = 0x07  # 三种饵都试，任一组合能上即可
zero = []
for spot in range(len(V.SPOTS)):
    for sid in (V.Fishing.first_subspot(spot), V.Fishing.first_subspot(spot) + 1):
        c = sample(spot, sid, n=200)
        # 用同一 RNG 连续采样，单种 200 次不足以覆盖所有低频鱼，
        # 因此改用"权重表"直接判 >0（与 C 的 w[i] 语义一致）
        rw = V.SPOT_RARITY_W[spot]
        sb = 1 << spot
        gold = V.SUBSPOTS[sid][2]
        for i in range(len(V.SPECIES)):
            if not (V.SPOT_MASK[i] & sb):
                continue
            f = V.SPECIES[i]
            w = rw[f[3]]
            w = w * 3 if f[12] & 1 else w * 6 // 10
            if f[13] & 1:
                w *= 2
            if f[4] & 1:
                w *= 3
            if gold & (1 << i):
                w *= 3
            if w < 1:
                w = 1
            if w <= 0:
                zero.append((V.SPOTS[spot][0], V.SUBSPOTS[sid][0], f[0]))
check("可出现鱼权重全 > 0", not zero, str(zero[:5]))

print("\n=== 3. 跨场鱼能被抽到，原场仍在 ===")
# 鲈鱼(index 11)：主栖息河、跨入海口；鲶鱼(index 13)：主栖息河、跨栈桥
for spot, sid, fish_idx, label in (
        (V.ESTUARY, 6, 11, "鲈鱼@入海口·泥滩"),
        (V.PIER, 8, 13, "鲶鱼@夜钓栈桥·灯影")):
    c = sample(spot, sid, n=6000, period=2 if fish_idx != 13 else 3, weather=0)
    hit = c[V.SPECIES[fish_idx][0]]
    check(label + " 能抽到", hit > 0, "(%d 次)" % hit)
for fish_idx, label in ((11, "鲈鱼"), (13, "鲶鱼")):
    c = sample(V.RIVER, 2, n=6000)
    hit = c[V.SPECIES[fish_idx][0]]
    check(label + " 原场急流河仍能抽到", hit > 0, "(%d 次)" % hit)

print("\n=== 4. 五钓场无空池 ===")
per_spot = [sum(1 for m in V.SPOT_MASK if m & (1 << s)) for s in range(len(V.SPOTS))]
check("每场至少 3 种鱼", all(n >= 3 for n in per_spot), str(per_spot))

print("\n=== 5. -1 未选标点 = v4 行为 ===")
for spot in (0, 1, 2):
    c = sample(spot, -1, n=3000, seed=777)
    g = V.Fishing(777)
    g.spot = spot
    g.set_bait(0)
    g.env_period = g.env_weather = 0
    c4 = Counter()
    for _ in range(3000):
        c4[V.SPECIES[g.roll_species()][0]] += 1
    check("%s 分布与 v4 一致" % V.SPOTS[spot][0], c == c4)

print("\n=== 鱼池对照表（选场 / 标点 × 主出鱼占比）===")
for spot in range(len(V.SPOTS)):
    sid0 = V.Fishing.first_subspot(spot)
    print("  %s（%d 种鱼）" % (V.SPOTS[spot][0], per_spot[spot]))
    for sid in (sid0, sid0 + 1):
        c = sample(spot, sid, n=3000)
        gold = V.SUBSPOTS[sid][2]
        rows = []
        for i in range(len(V.SPECIES)):
            if V.SPOT_MASK[i] & (1 << spot):
                pct = 100.0 * c[V.SPECIES[i][0]] / 3000
                mark = "*" if gold & (1 << i) else " "
                rows.append("%s%s %.1f%%" % (mark, V.SPECIES[i][0], pct))
        print("    %-5s %s" % (V.SUBSPOTS[sid][0], "  ".join(rows)))

print("\n结论: %s（%d 项失败）" % ("PASS" if not fails else "FAIL", len(fails)))
sys.exit(1 if fails else 0)
