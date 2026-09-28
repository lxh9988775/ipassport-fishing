#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_logic.py - 钓鱼逻辑层「参考实现」即时校验（无需 gcc / 无需硬件）

为什么有这个文件？
  仓库的权威单测是 tests/fishing_logic_test.c（C 版，需 gcc 或 ESP-IDF）。
  但很多人的 Windows 没装 gcc，这里用纯 Python 复刻同一套算法，
  验证"规则正确性"不变量，让你在动手编译固件前就能确认逻辑没问题。

  算法与 main/fishing_logic.c 一一对应（xorshift32 + 同样的参数表），
  所以这里的结论可以直接映射到 C 层。

运行：
  python tools/verify_logic.py
"""

import random


# ---------- 与 C 层一致的常量 ----------
class Bait:
    WORM, BREAD, LURE = 0, 1, 2
    COUNT = 3


class Spot:
    POND, RIVER, SEA = 0, 1, 2
    COUNT = 3


class State:
    IDLE, WAITING, BITE, CATCH, MISS, MENU = range(6)


# 钓点参数表：[wait_min, wait_max, bite_window, mult*100]
SPOT_CFG = [
    (1500, 4000, 1400, 100),  # POND
    (2500, 6000, 1100, 150),  # RIVER
    (4000, 9000,  800, 250),  # SEA
]

# 饵料权重 [小鱼, 中鱼, 大鱼]
BAIT_WEIGHTS = [
    [3, 1, 0],  # WORM
    [2, 2, 1],  # BREAD
    [1, 2, 3],  # LURE
]

SIZE_BASE = [10, 25, 60]


class Fishing:
    def __init__(self, seed):
        self.rng = seed if seed else 0x9E3779B9
        self.state = State.IDLE
        self.score = 0
        self.high = 0
        self.bait = Bait.WORM
        self.spot = Spot.POND
        self.enter = 0
        self.last = 0
        self.wait_target = 0
        self.bite_window = 0

    def _next(self):
        x = self.rng
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        self.rng = x & 0xFFFFFFFF
        return self.rng

    def _range(self, n):
        return self._next() % n if n > 0 else 0

    def init(self, seed):
        self.__init__(seed)

    def set_bait(self, b):
        if 0 <= b < Bait.COUNT:
            self.bait = b

    def set_spot(self, s):
        if 0 <= s < Spot.COUNT:
            self.spot = s

    def cast(self):
        if self.state not in (State.IDLE, State.MENU):
            return 0
        if self.state == State.MENU:
            self.state = State.IDLE
        cfg = SPOT_CFG[self.spot]
        self.wait_target = cfg[0] + self._range(cfg[1] - cfg[0] + 1)
        self.state = State.WAITING
        self.enter = self.last
        return self.wait_target

    def tick(self, now):
        self.last = now
        evt = 0
        if self.state == State.WAITING:
            if now - self.enter >= self.wait_target:
                self.bite_window = SPOT_CFG[self.spot][2]
                self.state = State.BITE
                self.enter = now
                evt = 1  # BITE_START
        elif self.state == State.BITE:
            if now - self.enter >= self.bite_window:
                self.state = State.MISS
                self.enter = now
                evt = 2  # BITE_TIMEOUT
        elif self.state in (State.CATCH, State.MISS):
            if now - self.enter >= 1500:
                self.state = State.IDLE
                self.enter = now
                evt = 3  # CAST_READY
        return evt

    def strike(self):
        if self.state != State.BITE:
            return None
        elapsed = self.last - self.enter
        if elapsed < 0 or elapsed > self.bite_window:
            return None
        w = BAIT_WEIGHTS[self.bait]
        total = w[0] + w[1] + w[2]
        pick = self._range(total)
        size = 1 if pick < w[0] else (2 if pick < w[0] + w[1] else 3)
        base = SIZE_BASE[size - 1]
        mult = SPOT_CFG[self.spot][3]
        score = (base * mult + 50) // 100
        self.score += score
        if self.score > self.high:
            self.high = self.score
        self.state = State.CATCH
        self.enter = self.last
        return {"size": size, "base": base, "score": score}


def advance_to_bite(f, step=10, cap=120000):
    # 从对象当前时钟（f.last）延续推进，而不是从 0 开始，
    # 这样在"连续多轮"测试中不会因时钟错位而卡死。
    now = f.last
    while now < f.last + cap:
        now += step
        if f.tick(now) == 1:
            return now
    return None


def main():
    fails = 0

    def check(name, cond, extra=""):
        nonlocal fails
        if cond:
            print(f"[OK] {name} {extra}")
        else:
            print(f"[FAIL] {name} {extra}")
            fails += 1

    # 1) 确定性
    a = Fishing(42); a.set_bait(Bait.WORM); a.set_spot(Spot.POND); w1 = a.cast()
    b = Fishing(42); b.set_bait(Bait.WORM); b.set_spot(Spot.POND); w2 = b.cast()
    check("确定性: 同种子等待时长一致", w1 == w2, f"({w1} ms)")

    # 2) 抛竿 -> 咬钩
    f = Fishing(7); f.set_spot(Spot.POND); f.cast()
    t = advance_to_bite(f)
    check("抛竿后进入咬钩窗口", t is not None and f.state == State.BITE)

    # 3) 窗口内提竿成功
    f.tick(t + 50); r = f.strike()
    check("窗口内提竿成功", r is not None and r["score"] > 0,
          f"(大小={r['size']} 得分={r['score']})" if r else "")

    # 4) 窗口外提竿无效
    f2 = Fishing(7); f2.set_spot(Spot.POND); f2.cast()
    t2 = advance_to_bite(f2)
    f2.tick(t2 + f2.bite_window + 100)  # 超时
    r2 = f2.strike()
    check("窗口外提竿无效(跑鱼)", r2 is None)

    # 5) 计分倍率 SEA>=POND
    fp = Fishing(1); fp.set_bait(Bait.WORM); fp.set_spot(Spot.POND); fp.cast()
    tp = advance_to_bite(fp); fp.tick(tp + 50); rp = fp.strike()
    fs = Fishing(1); fs.set_bait(Bait.WORM); fs.set_spot(Spot.SEA); fs.cast()
    ts = advance_to_bite(fs); fs.tick(ts + 50); rs = fs.strike()
    check("计分倍率 SEA>=POND", rp and rs and rs["score"] >= rp["score"],
          f"(POND={rp['score']} SEA={rs['score']})" if rp and rs else "")

    # 6) 最高分缓存（连续多轮，验证 CATCH->IDLE 复位）
    fh = Fishing(3); fh.set_spot(Spot.SEA)
    for _ in range(5):
        assert fh.cast() > 0, "cast 需在 IDLE 状态"
        th = advance_to_bite(fh)
        fh.tick(th + 50); fh.strike()
        fh.tick(th + 50 + 2000)  # 推进过 CATCH 结算窗口，回到 IDLE
    check("最高分缓存>0", fh.high > 0, f"({fh.high})")

    # 7) 饵料分布趋势
    N = 4000
    wb = lb = 0
    for _ in range(N):
        fw = Fishing(99); fw.set_bait(Bait.WORM); fw.set_spot(Spot.SEA); fw.cast()
        tw = advance_to_bite(fw); fw.tick(tw + 50); rw = fw.strike()
        if rw and rw["size"] == 3:
            wb += 1
    for _ in range(N):
        fl = Fishing(99); fl.set_bait(Bait.LURE); fl.set_spot(Spot.SEA); fl.cast()
        tl = advance_to_bite(fl); fl.tick(tl + 50); rl = fl.strike()
        if rl and rl["size"] == 3:
            lb += 1
    wp, lp_ = wb / N, lb / N
    check("LURE大鱼比例>WORM", lp_ > wp, f"(WORM={wp:.2f} LURE={lp_:.2f})")

    print()
    if fails == 0:
        print("ALL CHECKS PASSED ✓")
    else:
        print(f"{fails} CHECK(S) FAILED ✗")
    return fails


if __name__ == "__main__":
    raise SystemExit(main())
