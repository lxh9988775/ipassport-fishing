#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_pet_logic.py — 电子宠物纯逻辑层的 Python 参考实现冒烟验证。

本机无 gcc 时的兜底（与 verify_logic.py 对钓鱼层的角色相同）：
用 Python 1:1 复刻 main/pet_logic.c 的常量与规则，跑关键场景断言。
注意：它验证的是【数值设计与规则】，C 代码本身的编译验证仍需 ESP-IDF 构建。

v3（2026-10-05）新增场景 7：换装 costume 字段的存档往返 + v1/v2 旧档迁移
（按 C 结构体布局 struct.pack/unpack，1:1 复刻 pet_save_apply 的短读+迁移逻辑）。
"""
import struct

MIN = 60 * 1000
HUNGER_MS = 40 * MIN
CLEAN_MS = 60 * MIN
FUN_MS = 30 * MIN
ENERGY_MS = 45 * MIN
SLEEP_ENERGY_MS = 3 * MIN

PET_MAGIC = 0x50455431
PET_COSTUME_MAX = 5


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

    # ------------------------------------------------------------------
    # 场景 7：换装存档（v3 布局：…sticker_mask + costume，尾部对齐到 4）
    # C 结构体：I B B B B B B(=sleeping) H H H H H H H B  → 25B，sizeof=28
    # ------------------------------------------------------------------
    FMT = "<IBBBBBBHHHHHHHB"
    assert struct.calcsize(FMT) == 25

    def pack_save(ver, costume=None, feed=7, mask=0x00FF, h=66):
        body = struct.pack("<IBBBBBBHHHHHHH",
                           PET_MAGIC, ver, h, 70, 70, 70, 0,
                           120, 30, feed, 8, 9, 10, mask)
        if costume is not None:
            body += struct.pack("<B", costume)
        while len(body) % 4:
            body += b"\x00"          # C 尾部对齐
        return body

    def apply_save(buf):
        """1:1 复刻 pet_save_apply：短读补 0 + 版本分支。"""
        if not buf or len(buf) <= 0:
            return None
        s = bytearray(struct.calcsize(FMT))
        s[:min(len(buf), len(s))] = buf[:min(len(buf), len(s))]
        (magic, ver, h, _c, _f, _e, _sl, _xp, _care,
         feed, _cl, _pl, _slp, mask, costume) = struct.unpack(FMT, bytes(s))
        if magic != PET_MAGIC:
            return "INVALID"
        if ver == 3:
            return dict(ver=3, h=h, feed=feed, mask=mask,
                        costume=costume if costume <= PET_COSTUME_MAX else 0)
        if ver in (1, 2):
            r = dict(ver=ver, h=h, costume=0)
            if ver == 2:
                r["feed"], r["mask"] = feed, mask
            else:
                r["feed"], r["mask"] = 0, 1     # v1 只留初次见面贴纸
            return r
        return "NEWER"

    # 7a：v3 往返，装扮 4（围巾）保留
    r = apply_save(pack_save(3, costume=4))
    assert r["ver"] == 3 and r["costume"] == 4 and r["feed"] == 7, r
    # 7b：非法装扮值（>5）按 0 处理
    r = apply_save(pack_save(3, costume=99))
    assert r["costume"] == 0, r
    # 7c：v2 旧档（无 costume 字节）短读迁移 → 装扮归零、计数/贴纸保留
    r = apply_save(pack_save(2))
    assert r["ver"] == 2 and r["costume"] == 0 and r["feed"] == 7 and r["mask"] == 0x00FF, r
    # 7d：v1 老档迁移 → 分动作计数清零、只留 FIRST 贴纸
    r = apply_save(pack_save(1))
    assert r["ver"] == 1 and r["costume"] == 0 and r["feed"] == 0 and r["mask"] == 1, r

    print("[PASS] pet 逻辑冒烟验证 7/7 场景全过（含 v3 换装存档迁移）")


if __name__ == "__main__":
    main()
