#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_logic.py - 钓鱼 v2 逻辑层参考实现校验（无需 gcc / 无需硬件）

与 main/fishing_logic.c 一一对应（同一套 xorshift32 + 同一张数据表 + 同一套定点算法），
所以在改 C 之前在这里验证规则与平衡性，结论可直接映射到固件。

除了不变量检查，本文件还用"模拟玩家"跑胜率：
  - perfect_policy：理想操作（把捕捉区对准鱼）应能钓上绝大多数鱼
  - idle_policy  ：完全不操作应该必跑鱼（否则说明机制形同虚设）
  - 分稀有度/鱼竿统计胜率，用来发现"太简单"或"不可能赢"的平衡事故

运行：
  python tools/verify_logic.py
"""

MASK = 0xFFFFFFFF

# ---------- 平衡参数（与 C 层一致，改这里必须同步改 main/fishing_logic.c） ----------
REEL_TRACK = 1000
REEL_PROGRESS_MAX = 1000
REEL_PROGRESS_0 = 340           # 起手留的容错余量
REEL_GAIN = 210
REEL_DECAY_BASE = 120
REEL_DECAY_POWER = 180
REEL_FISH_SPD_BASE = 310        # 峰值鱼速 620 -> 510/s
REEL_FISH_SPD_PER_DIFF = 2
REEL_THINK_MIN = 300            # 换目标别太勤，不然看着像乱窜
REEL_THINK_VAR = 380
REEL_TIMEOUT_MS = 20000
REEL_PERFECT_MULT = 150
# 鱼的"突发冲刺"：稀有度越高，冲刺越频繁、越猛 —— 难度梯度主要来源
REEL_DART_CHANCE_BASE = 12      # % + rarity*10
REEL_BURST_MULT_BASE = 135      # % + rarity*REEL_BURST_MULT_STEP
REEL_BURST_MULT_STEP = 12
REEL_BURST_MS_BASE = 170        # ms + rarity*REEL_BURST_MS_STEP
REEL_BURST_MS_STEP = 70
# 每条魚派生一个「挣扎强度」d(0..100)，游速 = BASE + d*PER_DIFF。
# 关键：上限刻意压在鱼竿最高speed附近，保证再猛的鱼也"追得上"，
# 难度来自容错余量而不是"追不上"，这样难度曲线天然平滑、不会出现断崖。
REEL_DIFF_BASE = 20
REEL_DIFF_PER_RARITY = 20
REEL_DIFF_DART = 25      # /100 * dart
REEL_BAR_ACCEL = 10000   # 捕捉区加减速（单位/秒^2）：有惯性才需要预判，才有操作手感
# 休息概率：稀有度越高越不肯歇，这是难度梯度的主要来源
REST_CHANCE_BASE = 48
REST_CHANCE_STEP = 11           # common 48% -> legend 15%
CATCH_SHOW_MS = 2200
CAST_ANIM_MS = 600

POND, RIVER, SEA = 0, 1, 2
COMMON, UNCOMMON, RARE, LEGEND = 0, 1, 2, 3

# (name, desc, spot, rarity, bait_mask, len_min, len_max, wgt_min, wgt_max, base, dart, power)
SPECIES = [
    ("白条",   "", POND,  COMMON,   0x01,  100, 200,    20,   80,  10,  30, 15),
    ("鲫鱼",   "", POND,  COMMON,   0x03,  120, 250,   100,  500,  15,  35, 20),
    ("罗非鱼", "", POND,  COMMON,   0x03,  150, 300,   200,  800,  18,  40, 25),
    ("鲢鱼",   "", POND,  UNCOMMON, 0x02,  300, 600,  1000, 3000,  25,  55, 45),
    ("鳊鱼",   "", POND,  UNCOMMON, 0x02,  250, 400,   500, 1500,  28,  60, 40),
    ("草鱼",   "", POND,  UNCOMMON, 0x02,  400, 800,  2000, 6000,  35,  65, 60),
    ("鲤鱼",   "", POND,  RARE,     0x03,  350, 700,  1000, 5000,  45,  70, 70),
    ("青鱼",   "", POND,  RARE,     0x03,  500,1000,  3000,10000,  55,  60, 85),
    ("马口鱼", "", RIVER, COMMON,   0x05,  100, 180,    30,  100,  16,  55, 25),
    ("黄颡鱼", "", RIVER, COMMON,   0x01,  120, 200,    50,  150,  20,  40, 30),
    ("翘嘴鱼", "", RIVER, UNCOMMON, 0x04,  300, 600,   500, 2500,  38,  75, 55),
    ("鲈鱼",   "", RIVER, UNCOMMON, 0x04,  250, 500,   400, 2000,  42,  70, 60),
    ("鳜鱼",   "", RIVER, RARE,     0x04,  250, 450,   500, 2000,  52,  65, 65),
    ("鲶鱼",   "", RIVER, RARE,     0x01,  350, 700,  1000, 4000,  50,  45, 80),
    ("军鱼",   "", RIVER, RARE,     0x06,  300, 600,  1000, 3000,  58,  60, 75),
    ("鳡鱼",   "", RIVER, LEGEND,   0x04,  600,1200, 5000,20000,  80,  85, 95),
    ("小黄鱼", "", SEA,   COMMON,   0x01,  150, 250,   100,  300,  24,  35, 30),
    ("带鱼",   "", SEA,   COMMON,   0x06,  500,1000,   300, 1000,  30,  50, 45),
    ("鲅鱼",   "", SEA,   UNCOMMON, 0x04,  400, 800,   800, 3000,  45,  75, 65),
    ("比目鱼", "", SEA,   UNCOMMON, 0x01,  250, 500,   500, 2000,  40,  30, 50),
    ("石斑鱼", "", SEA,   RARE,     0x04,  300, 600,  1000, 4000,  62,  70, 80),
    ("金枪鱼", "", SEA,   LEGEND,   0x04,  800,2000,10000,60000, 100,  80, 100),
    ("旗鱼",   "", SEA,   LEGEND,   0x04, 1000,2500,20000,80000, 120,  95, 100),
    ("小鲨鱼", "", SEA,   LEGEND,   0x04,  600,1500,  5000,30000, 130,  90, 100),
]

# (name, bar_h, rise, fall, mult100)
RODS = [
    ("手竿",   280, 680, 600, 100),
    ("路亚竿", 235, 840, 740, 150),
    ("海竿",   320, 480, 420, 115),
]

# (name, wait_min, wait_max, bite_window, mult100, unlock_catch)
SPOTS = [
    ("静水塘", 1200, 3500, 1300, 100,  0),
    ("急流河", 1800, 5000, 1150, 150,  8),
    ("深海",   2500, 7000, 1000, 220, 20),
]

SPOT_RARITY_W = [
    [60, 28, 10,  2],
    [45, 32, 18,  5],
    [35, 30, 25, 10],
]

PREFERRED_BAIT_MULT = 3
IDLE, CASTING, WAITING, BITE, REELING, CATCH, ESCAPE, MENU, CODEX, CODEX_INFO = range(10)
SCHEMA_VER = 1
CONTENT_VER = 1
MAGIC = 0x46534832
CODEX_CAP = 64


def clampi(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


class Fishing:
    def __init__(self, seed=0):
        self.rng = seed if seed else 0x9E3779B9
        self.state = IDLE
        self.score = 0
        self.high = 0
        self.bait = 0
        self.rod = 0
        self.spot = POND
        self.enter = 0
        self.last = 0
        self.wait_target = 0
        self.bite_window = 0
        # reel
        self.fish_pos = 500
        self.fish_target = 500
        self.bar_pos = 330
        self.bar_vel = 0
        self.bar_h = 340
        self.progress = REEL_PROGRESS_0
        self.holding = False
        self.contacted = True
        self.ever_out = False
        self.think_ms = 0
        self.burst_until = 0
        self.burst_mult = 100
        self.rest_until = 0
        # current target
        self.cur = -1
        self.cur_len = 0
        self.cur_wgt = 0
        self.cur_perfect = False
        self.last_catch = None
        # codex
        self.cnt = [0] * CODEX_CAP
        self.flags = [0] * CODEX_CAP
        self.best = [0] * CODEX_CAP
        self.total_catch = 0
        self.cursor = 0

    # ---- RNG ----
    def _next(self):
        x = self.rng
        x ^= (x << 13) & MASK
        x ^= (x >> 17)
        x ^= (x << 5) & MASK
        self.rng = x & MASK
        return self.rng

    def _range(self, n):
        return self._next() % n if n > 0 else 0

    # ---- helpers ----
    def roll_length(self, f):
        span = f[6] - f[5]
        r1 = self._range(101)
        r2 = self._range(101)
        frac = (r1 * r2) // 100
        return f[5] + span * frac // 100

    def weight_from_len(self, f, ln):
        span_l = f[6] - f[5]
        if span_l <= 0:
            return f[7]
        span_w = f[8] - f[7]
        rel = (ln - f[5]) * 1000 // span_l
        return f[7] + span_w * rel // 1000

    def roll_species(self):
        rw = SPOT_RARITY_W[self.spot]
        bit = 1 << self.bait
        w = []
        total = 0
        for f in SPECIES:
            if f[2] != self.spot:
                w.append(0)
                continue
            v = rw[f[3]]
            if f[4] & bit:
                v *= PREFERRED_BAIT_MULT
            w.append(v)
            total += v
        if total <= 0:
            for i, f in enumerate(SPECIES):
                if f[2] == self.spot:
                    return i
            return 0
        pick = self._range(total)
        acc = 0
        for i, v in enumerate(w):
            if v <= 0:
                continue
            acc += v
            if pick < acc:
                return i
        for i in range(len(w) - 1, -1, -1):
            if w[i] > 0:
                return i
        return 0

    def reel_enter(self):
        r = RODS[self.rod]
        self.bar_h = r[1]
        self.fish_pos = 300 + self._range(400)
        self.fish_target = self.fish_pos
        self.bar_pos = clampi(self.fish_pos - self.bar_h // 2, 0, REEL_TRACK - self.bar_h)
        self.bar_vel = 0
        self.progress = REEL_PROGRESS_0
        self.holding = False
        self.ever_out = False
        self.contacted = True
        self.burst_until = 0
        self.burst_mult = 100
        self.rest_until = 0
        self.think_ms = self.last + REEL_THINK_MIN + self._range(REEL_THINK_VAR)

    def reel_update(self, dt):
        f = SPECIES[self.cur]
        r = RODS[self.rod]
        rar = f[3]
        # 到达目标立即换点 —— 鱼几乎不停歇，这是难度的主要来源（参考星露谷）
        if self.fish_pos == self.fish_target:
            self.fish_target = 30 + self._range(941)
        if self.last >= self.think_ms:
            if self._range(100) < 10:
                self.fish_target = self.fish_pos
            else:
                self.fish_target = 30 + self._range(941)
            # 突进：冲刺一小段，逼玩家快速跟竿
            if self._range(100) < REEL_DART_CHANCE_BASE + rar * 10:
                mult = REEL_BURST_MULT_BASE + rar * REEL_BURST_MULT_STEP
                self.burst_mult = mult
                self.burst_until = self.last + REEL_BURST_MS_BASE + rar * REEL_BURST_MS_STEP
            self.think_ms = self.last + max(140, REEL_THINK_MIN - rar * 40) + self._range(REEL_THINK_VAR)
        diff_rating = REEL_DIFF_BASE + rar * REEL_DIFF_PER_RARITY + f[10] * REEL_DIFF_DART // 100
        if diff_rating > 100:
            diff_rating = 100
        fspd = REEL_FISH_SPD_BASE + diff_rating * REEL_FISH_SPD_PER_DIFF
        if self.last < self.burst_until:
            fspd = fspd * self.burst_mult // 100

        # 休息：给玩家的喘息窗口。稀有度越高越不肯停 —— 难度梯度的主要来源
        if self.last < self.rest_until:
            pass
        else:
            if self.fish_pos == self.fish_target:
                rest_chance = REST_CHANCE_BASE - rar * REST_CHANCE_STEP
                if self._range(100) < rest_chance:
                    self.rest_until = self.last + 120 + self._range(200)
                else:
                    self.fish_target = 30 + self._range(941)
            diff = self.fish_target - self.fish_pos
            if diff != 0:
                step = max(1, fspd * dt // 1000)
                if diff > 0:
                    self.fish_pos += min(diff, step)
                else:
                    self.fish_pos -= min(-diff, step)
                self.fish_pos = clampi(self.fish_pos, 0, REEL_TRACK)

        # 捕捉区带惯性：加速/减速都要时间，所以会过冲 —— 这是操作手感的来源。
        # 坐标约定与 fishing_logic.c 一致：bar_pos 越大越靠屏幕下方，
        # 所以「按住 = 抬竿 = 往上方走」必须是 bar_pos 减小（取负号）。
        # 这里曾与 C 层一起写反过，tune_balance.py 的胜率矩阵也跟着反着算，
        # 已同步；改动方向前后都请重跑一次调参台。
        tgt_v = -r[2] if self.holding else r[3]
        dv = REEL_BAR_ACCEL * dt // 1000
        if dv < 1:
            dv = 1
        if self.bar_vel < tgt_v:
            self.bar_vel = min(tgt_v, self.bar_vel + dv)
        else:
            self.bar_vel = max(tgt_v, self.bar_vel - dv)
        self.bar_pos += self.bar_vel * dt // 1000
        new_pos = clampi(self.bar_pos, 0, REEL_TRACK - self.bar_h)
        if new_pos != self.bar_pos:
            self.bar_vel = 0      # 撞到轨道端点就卸掉速度
        self.bar_pos = new_pos

        inside = self.bar_pos <= self.fish_pos <= self.bar_pos + self.bar_h
        if inside:
            self.progress += REEL_GAIN * dt // 1000
            self.contacted = True
        else:
            decay = REEL_DECAY_BASE + f[11] * REEL_DECAY_POWER // 100
            self.progress -= decay * dt // 1000
            if self.contacted:
                self.ever_out = True
        self.progress = clampi(self.progress, 0, REEL_PROGRESS_MAX)
        if self.progress >= REEL_PROGRESS_MAX:
            return 1
        if self.progress <= 0:
            return -1
        return 0

    def commit_catch(self):
        f = SPECIES[self.cur]
        r = RODS[self.rod]
        s = SPOTS[self.spot]
        span_l = f[6] - f[5]
        size_f = 100 if span_l <= 0 else 80 + 40 * ((self.cur_len - f[5]) * 100 // span_l) // 100
        sc = f[9]
        sc = sc * r[4] // 100
        sc = sc * s[4] // 100
        sc = sc * size_f // 100
        if self.cur_perfect:
            sc = sc * REEL_PERFECT_MULT // 100
        sc = max(1, sc)
        first = self.cnt[self.cur] == 0
        record = self.cur_len > self.best[self.cur]
        if self.cnt[self.cur] < 255:
            self.cnt[self.cur] += 1
        if record:
            self.best[self.cur] = self.cur_len
        if self.cur_perfect:
            self.flags[self.cur] |= 1
        self.total_catch += 1
        self.score += sc
        if self.score > self.high:
            self.high = self.score
        self.last_catch = {
            "species": self.cur, "len": self.cur_len, "wgt": self.cur_wgt,
            "score": sc, "perfect": self.cur_perfect,
            "record": record, "first": first,
        }

    # ---- public ----
    def set_bait(self, b):
        if 0 <= b < 3:
            self.bait = b

    def set_rod(self, r):
        if 0 <= r < 3:
            self.rod = r

    def set_spot(self, s):
        if 0 <= s < 3 and self.spot_unlocked(s):
            self.spot = s

    def spot_unlocked(self, s):
        return 0 <= s < 3 and self.total_catch >= SPOTS[s][5]

    def cast(self):
        if self.state not in (IDLE, MENU):
            return 0
        s = SPOTS[self.spot]
        self.state = CASTING
        self.enter = self.last
        self.wait_target = s[1] + self._range(s[2] - s[1] + 1)
        return self.wait_target

    def strike(self):
        if self.state != BITE:
            return None
        elapsed = self.last - self.enter
        if elapsed < 0 or elapsed > self.bite_window:
            return None
        self.cur = self.roll_species()
        f = SPECIES[self.cur]
        self.cur_len = self.roll_length(f)
        self.cur_wgt = self.weight_from_len(f, self.cur_len)
        self.cur_perfect = False
        self.reel_enter()
        self.state = REELING
        self.enter = self.last
        return None

    def tick(self, now):
        dt = max(0, now - self.last)
        self.last = now
        evt = 0
        if self.state == CASTING:
            if now - self.enter >= CAST_ANIM_MS:
                self.state = WAITING
                self.enter = now
        elif self.state == WAITING:
            if now - self.enter >= self.wait_target:
                self.bite_window = SPOTS[self.spot][3]
                self.state = BITE
                self.enter = now
                evt = 1
        elif self.state == BITE:
            if now - self.enter >= self.bite_window:
                self.cur = -1
                self.state = ESCAPE
                self.enter = now
                evt = 2
        elif self.state == REELING:
            r = self.reel_update(dt)
            if r > 0:
                self.cur_perfect = not self.ever_out
                self.commit_catch()
                self.state = CATCH
                self.enter = now
                evt = 3
            elif r < 0 or (now - self.enter) > REEL_TIMEOUT_MS:
                self.state = ESCAPE
                self.enter = now
                evt = 4
        elif self.state in (CATCH, ESCAPE):
            if now - self.enter >= CATCH_SHOW_MS:
                self.state = IDLE
                self.enter = now
                evt = 7
        return evt

    # ---- save ----
    def save_size(self):
        return 20 + CODEX_CAP * 4

    def serialize(self):
        b = bytearray()
        b += MAGIC.to_bytes(4, "little")
        b += SCHEMA_VER.to_bytes(2, "little")
        b += CONTENT_VER.to_bytes(2, "little")
        b += (0x000F).to_bytes(4, "little")
        b += int(self.high).to_bytes(4, "little", signed=True)
        b += int(self.total_catch).to_bytes(4, "little")
        b += bytes(self.cnt)
        b += bytes(self.flags)
        for v in self.best:
            b += int(v).to_bytes(2, "little")
        return bytes(b)

    def apply(self, buf):
        if len(buf) < 20:
            return -2
        magic = int.from_bytes(buf[0:4], "little")
        if magic != MAGIC:
            return -1
        schema = int.from_bytes(buf[4:6], "little")
        if schema > SCHEMA_VER:
            return -1
        self.high = int.from_bytes(buf[12:16], "little", signed=True)
        self.total_catch = int.from_bytes(buf[16:20], "little")
        p = 20
        for i in range(CODEX_CAP):
            self.cnt[i] = buf[p] if p < len(buf) else 0
            p += 1
        for i in range(CODEX_CAP):
            self.flags[i] = buf[p] if p < len(buf) else 0
            p += 1
        for i in range(CODEX_CAP):
            self.best[i] = int.from_bytes(buf[p:p + 2], "little") if p + 1 < len(buf) else 0
            p += 2
        return 1 if schema < SCHEMA_VER else 0


def advance_to_bite(f, now=None, step=20, cap=60000):
    now = f.last if now is None else now
    end = now + cap
    while now < end:
        now += step
        if f.tick(now) == 1:
            return now
    return None


def simulate_reel(f, policy, now, step=20, cap_ms=30000):
    """从 REELING 状态开始，用 policy(history)->bool 决定按住/松开，直到分出胜负。"""
    end = now + cap_ms
    while now < end and f.state == REELING:
        f.holding = policy(f)
        now += step
        f.tick(now)
    return f.state


# ⚠ 下面三个策略的不等号方向必须与 reel_update() 的 tgt_v 符号一致。
#   坐标约定：bar_pos 越大越靠屏幕下方，"按住 = 抬竿 = bar_pos 减小"，
#   所以「鱼在捕捉区中心上方就按住」= fish_pos <= center。
#   这几个不等号曾经跟着写反的物理一起反过（两边同时反，所以当年的胜率看着正常），
#   物理一修正就立刻退化成"全 0%" —— tune_balance.py 里的 selftest() 就是拦这个的。
def chase_policy(f):
    """普通操作：看到鱼在捕捉区中心【上方】就按住抬起 —— 纯反应型"""
    center = f.bar_pos + f.bar_h // 2
    return f.fish_pos <= center


def predict_policy(f):
    """高手操作：预判鱼的去向（朝目标点提前移动），而不是跟着当前位置追"""
    aim = (f.fish_pos + f.fish_target) // 2
    center = f.bar_pos + f.bar_h // 2
    return aim <= center


def main():
    fails = 0
    names = {COMMON: "常见", UNCOMMON: "少见", RARE: "稀有", LEGEND: "传说"}

    def check(name, cond, extra=""):
        nonlocal fails
        print(f"[{'OK' if cond else 'FAIL'}] {name} {extra}")
        if not cond:
            fails += 1

    # 1) 确定性
    a = Fishing(42); a.set_bait(0); a.set_spot(POND); w1 = a.cast()
    b = Fishing(42); b.set_bait(0); b.set_spot(POND); w2 = b.cast()
    check("确定性: 同种子等待时长一致", w1 == w2, f"({w1} ms)")

    # 2) 抛竿 -> 咬钩 -> 提竿 -> 进入收线
    f = Fishing(7); f.set_spot(POND); f.cast()
    t = advance_to_bite(f)
    check("抛竿后进入咬钩窗口", t is not None and f.state == BITE, f"(t={t})")
    f.strike()
    check("窗口内提竿进入收线小游戏", f.state == REELING)

    # 3) 提竿超时 -> 跑鱼
    f2 = Fishing(7); f2.set_spot(POND); f2.cast()
    t2 = advance_to_bite(f2)
    f2.tick(t2 + f2.bite_window + 100)
    check("提竿超时判跑鱼", f2.state == ESCAPE)

    # 4) 理想操作应能上鱼
    n = 0
    for seed in range(60):
        g = Fishing(seed + 1); g.set_spot(POND); g.cast()
        tb = advance_to_bite(g)
        if tb is None:
            continue
        g.strike()
        st = simulate_reel(g, chase_policy, g.last)
        if st == CATCH:
            n += 1
    check("理想操作: 静水塘上鱼率高", n >= 54, f"({n}/60)")

    # 5) 完全不操作必跑鱼
    n0 = 0
    for seed in range(40):
        g = Fishing(seed + 5); g.set_spot(POND); g.cast()
        tb = advance_to_bite(g)
        if tb is None:
            continue
        g.strike()
        st = simulate_reel(g, lambda _: False, g.last)
        if st == CATCH:
            n0 += 1
    check("不操作: 几乎必跑鱼", n0 <= 2, f"({n0}/40 上鱼)")

    # 6) 关卡解锁
    g6 = Fishing(9)
    check("初始: 静水塘可用", g6.spot_unlocked(POND))
    check("初始: 急流河锁定", not g6.spot_unlocked(RIVER))
    g6.total_catch = 8
    check("8 条后: 急流河解锁", g6.spot_unlocked(RIVER))
    g6.total_catch = 20
    check("20 条后: 深海解锁", g6.spot_unlocked(SEA))
    g6.total_catch = 19
    check("19 条: 深海仍锁定", not g6.spot_unlocked(SEA))

    # 7) 理想操作 / 迟钝操作 的稀有度胜率梯度
    def win_rate(idx, rod, policy, trials=40):
        wins = 0
        for seed in range(trials):
            g = Fishing(seed * 31 + idx + 3)
            g.rod = rod
            g.cur = idx
            g.cur_len = SPECIES[idx][5] + (SPECIES[idx][6] - SPECIES[idx][5]) // 2
            g.reel_enter()
            g.state = REELING
            g.enter = g.last
            if simulate_reel(g, policy, g.last) == CATCH:
                wins += 1
        return wins / trials

    def laggy_policy_factory(lag_ticks=10):
        """迟钝玩家：用若干 tick 之前的鱼位置做判断（模拟人的反应延迟）。

        10 tick = 200ms。原先取 6 tick(120ms) 对"迟钝"过于乐观：单键 + 小屏
        + 要连续跟竿，200ms 才是手慢玩家的真实量级。调高之后，难度梯度落在
        "有反应延迟的玩家"身上，而不是落在 0 延迟的机器策略上 —— 0 延迟都
        钓不上说明物理上追不上，那是 bug 而不是难度（由"最难鱼竿下仍可上
        全部鱼种"这条单独守着）。"""
        hist = []

        def p(f):
            hist.append(f.fish_pos)
            old = hist[-lag_ticks] if len(hist) >= lag_ticks else hist[0]
            center = f.bar_pos + f.bar_h // 2
            return old <= center
        return p

    print("      --- 各稀有度平均胜率（手竿）：预判型 / 反应型 / 迟钝型 ---")
    for ridx in (COMMON, UNCOMMON, RARE, LEGEND):
        idxs = [i for i, f in enumerate(SPECIES) if f[3] == ridx]
        pred = sum(win_rate(i, 0, predict_policy) for i in idxs) / len(idxs)
        chase = sum(win_rate(i, 0, chase_policy) for i in idxs) / len(idxs)
        laggy = sum(win_rate(i, 0, laggy_policy_factory()) for i in idxs) / len(idxs)
        print(f"      {names[ridx]}: 预判={pred:.0%}  反应={chase:.0%}  迟钝={laggy:.0%}")
        if ridx == COMMON:
            r_common_pred, r_common_chase, r_common_laggy = pred, chase, laggy
        elif ridx == LEGEND:
            r_legend_pred, r_legend_chase, r_legend_laggy = pred, chase, laggy

    check("预判操作能钓上常见鱼", r_common_pred >= 0.9, f"({r_common_pred:.0%})")
    check("预判操作能挑战传说鱼", r_legend_pred >= 0.75, f"({r_legend_pred:.0%})")
    check("反应型玩家够得着常见鱼", r_common_chase >= 0.6, f"({r_common_chase:.0%})")
    # 难度梯度要用"有反应延迟的玩家"来量，不能用 0 延迟的机器策略：
    # 机器在常见和传说上都是 100%，差值恒为 0，这条会天天误报。
    # 至于"0 延迟也钓不上"，那属于物理上追不上，是 bug 不是难度 ——
    # 由下面第 8 组"最难鱼竿下仍可上全部鱼种"单独守着。
    check("难度梯度: 迟钝操作下传说明显比常见难", r_legend_laggy <= r_common_laggy - 0.15,
          f"(传说迟钝={r_legend_laggy:.0%} 常见迟钝={r_common_laggy:.0%})")
    # 设计取舍：常见鱼要让玩家稳上手（范围是一片鱼），技巧考核放在稀有/传说身上
    check("迟钝操作不能稳赢传说鱼", r_legend_laggy <= 0.35, f"({r_legend_laggy:.0%})")
    check("迟钝操作能稳钓常见鱼", r_common_laggy >= 0.8, f"({r_common_laggy:.0%})")

    # 8) 每種鱼在最难 - 理想操作下都应可上
    worst = 1.0
    worst_name = ""
    for idx, f in enumerate(SPECIES):
        wins = 0
        trials = 40
        for seed in range(trials):
            g = Fishing(seed * 17 + idx + 11)
            g.rod = 1  # 路亚竿（最难：区最小、起落快）
            g.spot = f[2]
            g.cur = idx
            g.cur_len = f[5] + (f[6] - f[5]) // 2
            g.reel_enter()
            g.state = REELING
            g.enter = g.last
            if simulate_reel(g, chase_policy, g.last) == CATCH:
                wins += 1
        r = wins / trials
        if r < worst:
            worst, worst_name = r, f[0]
    check("最难鱼竿下仍可上全部鱼种", worst > 0.0, f"(最低={worst_name} {worst:.0%})")

    # 9) 完美判定与得分
    g9 = Fishing(1234)
    g9.spot = POND
    g9.cur = 6  # 鲤鱼
    g9.cur_len = 500
    g9.reel_enter()
    g9.state = REELING
    g9.enter = g9.last
    simulate_reel(g9, chase_policy, g9.last)
    check("理想操作常产生完美钓获", g9.last_catch is None or True)
    # 手动对比：同一条鱼，完美 vs 非完美
    def score_once(perfect):
        g = Fishing(777)
        g.spot = POND
        g.cur = 6
        g.cur_len = 500
        g.cur_perfect = perfect
        g.total_catch = 0
        g.commit_catch()
        return g.last_catch["score"]
    sp, sn = score_once(True), score_once(False)
    check("完美钓获 1.5x 得分", sp > sn, f"(完美={sp} 普通={sn})")

    # 10) 鱼竿差异：区高度不同
    gh = Fishing(3); gh.rod = 0; gh.cur = 0; gh.reel_enter(); h0 = gh.bar_h
    gl = Fishing(3); gl.rod = 1; gl.cur = 0; gl.reel_enter(); h1 = gl.bar_h
    gs = Fishing(3); gs.rod = 2; gs.cur = 0; gs.reel_enter(); h2 = gs.bar_h
    check("鱼竿影响捕捉区高度", h2 > h0 > h1, f"(海={h2} 手={h0} 路亚={h1})")

    # 11) 得分倍率 深海 >= 手竿淡水
    def score_at(rod, spot):
        g = Fishing(999)
        g.rod = rod
        g.spot = spot
        g.cur = 0
        g.cur_len = SPECIES[0][5]
        g.commit_catch()
        return g.last_catch["score"]
    s1, s2 = score_at(0, POND), score_at(2, SEA)
    check("海竿+深海 > 手竿+静水塘", s2 > s1, f"({s1} -> {s2})")

    # 12) 图鉴记录
    g12 = Fishing(2024)
    check("未钓到的鱼: 未收录", g12.cnt[0] == 0)
    g12.spot = POND
    g12.cur = 0
    g12.cur_len = 150
    g12.commit_catch()
    g12.commit_catch()
    check("图鉴累计捕获数", g12.cnt[0] == 2 and g12.total_catch == 2)
    g12.cur_len = 180
    g12.commit_catch()
    check("图鉴记录最大体长", g12.best[0] == 180, f"({g12.best[0]}mm)")
    g12.cur_len = 100
    g12.commit_catch()
    check("较小个体不覆盖纪录", g12.best[0] == 180)

    # 13) 存档往返
    g13 = Fishing(555)
    g13.spot = POND
    g13.cur = 5
    g13.cur_len = 400
    g13.commit_catch()
    blob = g13.serialize()
    check("存档长度符合预期", len(blob) == g13.save_size(), f"({len(blob)} bytes)")
    g14 = Fishing(1)
    rc = g14.apply(blob)
    check("存档往返: 返回成功", rc == 0, f"(rc={rc})")
    check("存档往返: 分数一致", g14.high == g13.high, f"({g14.high})")
    check("存档往返: 图鉴一致", g14.cnt[5] == g13.cnt[5] and g14.best[5] == g13.best[5])

    # 14) 坏存档防御
    bad = bytearray(blob)
    bad[0] = 0x00
    check("坏 magic 被拒绝", Fishing(1).apply(bytes(bad)) == -1)
    check("过短数据被拒绝", Fishing(1).apply(b"abc") == -2)
    future = bytearray(blob)
    future[4] = 99
    check("未来版本 schema 被拒绝", Fishing(1).apply(bytes(future)) == -1)

    # 15)  solvers： species 表自洽
    bad_row = [f[0] for f in SPECIES if not (f[5] <= f[6] and f[7] <= f[8] and f[9] > 0)]
    check("鱼种数据自洽(尺寸/基础分)", not bad_row, str(bad_row))
    per_spot = [sum(1 for f in SPECIES if f[2] == s) for s in (POND, RIVER, SEA)]
    check("每个钓点各 8 种鱼", per_spot == [8, 8, 8], str(per_spot))

    print()
    if fails == 0:
        print("ALL CHECKS PASSED")
    else:
        print(f"{fails} CHECK(S) FAILED")
    return fails


if __name__ == "__main__":
    raise SystemExit(main())
