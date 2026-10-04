#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_pet_logic.py — 电子宠物纯逻辑层的 Python 参考实现冒烟验证。

本机无 gcc 时的兜底（与 verify_logic.py 对钓鱼层的角色相同）：
用 Python 1:1 复刻 main/pet_logic.c 的常量与规则，跑关键场景断言。
注意：它验证的是【数值设计与规则】，C 代码本身的编译验证仍需 ESP-IDF 构建。
"""
MIN = 60 * 1000
HUNGER_MS = 40 * MIN
CLEAN_MS = 60 * MIN
FUN_MS = 30 * MIN
ENERGY_MS = 45 * MIN
SLEEP_ENERGY_MS = 3 * MIN


class Pet:
    def __init__(self):
        self.h, self.c, self.f, self.e = 80, 80, 70, 90
        self.sleeping = False
        self.xp = 0
        self.acc = {"h": 0, "c": 0, "f": 0, "e": 0}

    def _decay(self, key, val, dt, rate):
        if val <= 0:
            self.acc[key] = 0
            return 0
        self.acc[key] += dt
        while self.acc[key] >= rate and val > 0:
            self.acc[key] -= rate
            val -= 1
        if val <= 0:
            self.acc[key] = 0
        return val

    def tick(self, dt):
        if self.sleeping:
            self.acc["e"] += dt
            while self.acc["e"] >= SLEEP_ENERGY_MS and self.e < 100:
                self.acc["e"] -= SLEEP_ENERGY_MS
                self.e += 1
            woke = False
            if self.e >= 100:
                self.sleeping = False
                self.acc = {"h": 0, "c": 0, "f": 0, "e": 0}
                woke = True
            self.h = self._decay("h", self.h, dt, HUNGER_MS * 2)
            return woke
        self.h = self._decay("h", self.h, dt, HUNGER_MS)
        self.c = self._decay("c", self.c, dt, CLEAN_MS)
        self.f = self._decay("f", self.f, dt, FUN_MS)
        self.e = self._decay("e", self.e, dt, ENERGY_MS)
        return False

    def feed(self):
        if self.h >= 98:
            return "FULL"
        self.h = min(100, self.h + 30)
        self.f = min(100, self.f + 2)
        self.xp += 5
        return "OK"

    def play(self):
        if self.e < 15:
            return "TIRED"
        self.f = min(100, self.f + 25)
        self.e = max(0, self.e - 10)
        self.h = max(0, self.h - 5)
        self.xp += 8
        return "OK"


def main():
    # 场景 1：醒着 1 小时，衰减符合"慢"的设计
    p = Pet()
    p.tick(60 * MIN)
    assert (p.h, p.c, p.f, p.e) == (79, 79, 68, 89), (p.h, p.c, p.f, p.e)

    # 场景 2：离机 3 天（醒着），最惨也只掉到 0，不会"死"
    p2 = Pet()
    p2.tick(72 * 60 * MIN)
    assert p2.h == 0 and p2.f == 0, (p2.h, p2.f)

    # 场景 3：睡觉 30 分钟，精力 +10
    p3 = Pet()
    p3.e = 10
    p3.sleeping = True
    woke = p3.tick(30 * MIN)
    assert not woke and p3.e == 20, p3.e

    # 场景 4：睡 5 小时睡饱自动醒
    p4 = Pet()
    p4.e = 0
    p4.sleeping = True
    woke = p4.tick(5 * 60 * MIN)
    assert woke and p4.e == 100 and not p4.sleeping

    # 场景 5：喂食/陪玩规则
    p5 = Pet()
    p5.h = 90
    assert p5.feed() == "OK" and p5.h == 100
    assert p5.feed() == "FULL" and p5.h == 100
    p5.e = 10
    assert p5.play() == "TIRED"
    p5.e = 50
    # 喂食已带玩乐 +2（70→72），陪玩 +25 → 97
    assert p5.play() == "OK" and p5.e == 40 and p5.f == 97, (p5.e, p5.f)

    # 场景 6：升级节奏（60 XP 一级）
    p6 = Pet()
    for _ in range(12):     # 12 × 5 = 60 XP
        p6.h = 0
        p6.feed()
    assert p6.xp == 60      # 升到 2 级

    print("[PASS] pet 逻辑冒烟验证 6/6 场景全过")


if __name__ == "__main__":
    main()
