#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_pet_logic.py — 电子宠物纯逻辑层的 Python 参考实现冒烟验证。

本机无 gcc 时的兜底（与 verify_logic.py 对钓鱼层的角色相同）：
用 Python 1:1 复刻 main/pet_logic.c 的常量与规则，跑关键场景断言。
注意：它验证的是【数值设计与规则】，C 代码本身的编译验证仍需 ESP-IDF 构建。

v4（2026-10-29）新增：多宠物槽（3 槽）存档布局 + 名字 + 引导标志的
序列化往返，以及 v1/v2/v3 旧档迁移到 v4（短读补 0 + 版本分支）。
"""
import struct

MIN = 60 * 1000
HUNGER_MS = 40 * MIN
CLEAN_MS = 60 * MIN
FUN_MS = 30 * MIN
ENERGY_MS = 45 * MIN
SLEEP_ENERGY_MS = 3 * MIN

PET_MAGIC = 0x50455431
PET_COSTUME_MAX = 8
PET_STICKER_MAX = 16
PET_SLOT_MAX = 3

# v4 二进制布局（与 pet_logic.c 完全一致，小端，手动读写）
SLOT_BYTES = 36
SAVE_HDR = 8
SAVE_SIZE = SAVE_HDR + PET_SLOT_MAX * SLOT_BYTES   # 8 + 3*36 = 116


def put_u8(b, off, v):
    b[off] = v & 0xFF


def put_u16(b, off, v):
    b[off] = v & 0xFF
    b[off + 1] = (v >> 8) & 0xFF


def get_u8(b, off):
    return b[off]


def get_u16(b, off):
    return b[off] | (b[off + 1] << 8)


# ----------------------------------------------------------------------
# 单宠衰减/动作复刻（与 v3 相同，验证数值设计未被破坏）
# ----------------------------------------------------------------------
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


# ----------------------------------------------------------------------
# v4 存档序列化/迁移 1:1 复刻（用于断言）
# ----------------------------------------------------------------------
class Slot:
    def __init__(self):
        self.h = self.c = self.f = self.e = 80
        self.sleeping = 0
        self.xp = 0
        self.care = self.feed = self.clean = self.play = self.sleep = 0
        self.sticker = 0
        self.costume = 0
        self.name = "团子"


def pack_v4(slots, slot_active=0, intro_done=0):
    b = bytearray(SAVE_SIZE)
    b[0:4] = struct.pack("<I", PET_MAGIC)
    put_u8(b, 4, 4)
    put_u8(b, 5, slot_active)
    put_u8(b, 6, intro_done)
    put_u8(b, 7, 0)
    for i in range(PET_SLOT_MAX):
        s = slots[i]
        base = SAVE_HDR + i * SLOT_BYTES
        put_u8(b, base + 0, s.h)
        put_u8(b, base + 1, s.c)
        put_u8(b, base + 2, s.f)
        put_u8(b, base + 3, s.e)
        put_u8(b, base + 4, s.sleeping)
        put_u16(b, base + 5, s.xp)
        put_u16(b, base + 7, s.care)
        put_u16(b, base + 9, s.feed)
        put_u16(b, base + 11, s.clean)
        put_u16(b, base + 13, s.play)
        put_u16(b, base + 15, s.sleep)
        put_u16(b, base + 17, s.sticker)
        put_u8(b, base + 19, s.costume)
        nb = s.name.encode("utf-8")[:15]
        b[base + 20: base + 20 + len(nb)] = nb
    return bytes(b)


def apply_v4(buf):
    """1:1 复刻 pet_save_apply。返回 (rc, slots[3], slot_active, intro_done)。"""
    if not buf or len(buf) <= 0:
        return (-2, None, 0, 0)
    if len(buf) < 5:
        return (-2, None, 0, 0)
    magic = struct.unpack("<I", buf[0:4])[0]
    ver = buf[4]
    if magic != PET_MAGIC:
        return (-1, None, 0, 0)
    slots = [Slot() for _ in range(PET_SLOT_MAX)]
    # pet_init：槽 0 发初次见面贴纸
    slots[0].sticker |= (1 << 0)

    if ver == 4:
        if len(buf) < SAVE_SIZE:
            return (-2, None, 0, 0)
        for i in range(PET_SLOT_MAX):
            base = SAVE_HDR + i * SLOT_BYTES
            s = slots[i]
            s.h = min(100, get_u8(buf, base + 0))
            s.c = min(100, get_u8(buf, base + 1))
            s.f = min(100, get_u8(buf, base + 2))
            s.e = min(100, get_u8(buf, base + 3))
            s.sleeping = get_u8(buf, base + 4)
            s.xp = get_u16(buf, base + 5)
            s.care = get_u16(buf, base + 7)
            s.feed = get_u16(buf, base + 9)
            s.clean = get_u16(buf, base + 11)
            s.play = get_u16(buf, base + 13)
            s.sleep = get_u16(buf, base + 15)
            s.sticker = get_u16(buf, base + 17)
            cv = get_u8(buf, base + 19)
            s.costume = 0 if cv > PET_COSTUME_MAX else cv
            nm = bytes(buf[base + 20: base + 36]).split(b"\x00")[0]
            s.name = nm.decode("utf-8", "replace")
        sa = get_u8(buf, 5)
        if sa >= PET_SLOT_MAX:
            sa = 0
        intro = get_u8(buf, 6)
        return (0, slots, sa, intro)

    if ver in (1, 2, 3):
        ob = bytearray(28)
        n = min(len(buf), 28)
        ob[:n] = buf[:n]
        s = slots[0]
        s.h = min(100, get_u8(ob, 5))
        s.c = min(100, get_u8(ob, 6))
        s.f = min(100, get_u8(ob, 7))
        s.e = min(100, get_u8(ob, 8))
        s.sleeping = get_u8(ob, 9)
        s.xp = get_u16(ob, 10)
        s.care = get_u16(ob, 12)
        s.feed = get_u16(ob, 14)
        s.clean = get_u16(ob, 16)
        s.play = get_u16(ob, 18)
        s.sleep = get_u16(ob, 20)
        s.sticker = get_u16(ob, 22)
        cv = get_u8(ob, 24)
        s.costume = 0 if cv > PET_COSTUME_MAX else cv
        s.sticker |= (1 << 0)   # 确保初次见面
        # 槽 1、2 保持默认（名字"团子"，无贴纸）
        return (1, slots, 0, 1)

    return (1, slots, 0, 0)


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
    assert p5.play() == "OK" and p5.e == 40 and p5.f == 97, (p5.e, p5.f)

    # 场景 6：升级节奏（60 XP 一级）
    p6 = Pet()
    for _ in range(12):
        p6.h = 0
        p6.feed()
    assert p6.xp == 60      # 升到 2 级

    print("[PASS] 单宠衰减/动作/升级 6/6 场景全过（pet_tick 逻辑未改动）")

    # ------------------------------------------------------------------
    # 场景 7：v4 多宠物存档往返（3 槽 + 名字 + 引导标志）
    # ------------------------------------------------------------------
    slots = [Slot() for _ in range(PET_SLOT_MAX)]
    slots[0].h, slots[0].c, slots[0].f, slots[0].e = 66, 70, 72, 88
    slots[0].xp = 120
    slots[0].sticker = (1 << 0) | (1 << 7)   # FIRST + GAME_CATCH
    slots[0].costume = 4
    slots[0].name = "团子"
    slots[1].h = 50
    slots[1].sticker = (1 << 0)
    slots[1].costume = 2
    slots[1].name = "豆豆"
    slots[2].h = 30
    slots[2].name = "毛毛"

    buf = pack_v4(slots, slot_active=1, intro_done=1)
    assert len(buf) == SAVE_SIZE, len(buf)
    rc, out, sa, intro = apply_v4(buf)
    assert rc == 0, rc
    assert sa == 1 and intro == 1
    assert out[0].h == 66 and out[0].costume == 4 and out[0].name == "团子"
    assert out[0].xp == 120 and out[0].sticker == ((1 << 0) | (1 << 7))
    assert out[1].name == "豆豆" and out[1].costume == 2 and out[1].h == 50
    assert out[2].name == "毛毛" and out[2].h == 30
    # 名字 UTF-8 正确往返（团子 = e5 9b 9b e5 ad 90）
    assert out[0].name.encode("utf-8") == b"\xe5\x9b\xa2\xe5\xad\x90", out[0].name
    print("[PASS] v4 三宠物存档往返 + 名字 UTF-8 + 槽/引导标志 全过")

    # 场景 8：v3 旧档（25 字节单宠）短读迁移 → 槽0恢复、槽1/2默认、引导跳过
    # v3 布局（小端）：magic u32, ver u8, h,c,f,e,sleeping, xp u16, care u16,
    #   feed u16, clean u16, play u16, sleep u16, sticker u16, costume u8 = 25B
    old = struct.pack("<IBBBBBBHHHHHHHB",
                      PET_MAGIC, 3, 66, 70, 70, 70, 0,
                      120, 30, 7, 8, 9, 10, 0x00FF, 4)
    assert len(old) == 25
    rc, out, sa, intro = apply_v4(old)
    assert rc == 1, rc
    assert sa == 0 and intro == 1
    assert out[0].h == 66 and out[0].costume == 4
    assert out[0].sticker == (0x00FF | (1 << 0))   # 旧贴纸位图 + 初次见面
    assert out[1].name == "团子" and out[1].sticker == 0          # 槽1 默认（无贴纸）
    assert out[2].name == "团子" and out[2].sticker == 0          # 槽2 默认（无贴纸）
    print("[PASS] v3→v4 旧档迁移：槽0恢复、槽1/2默认、引导标志置真 全过")

    # 场景 9：v1 老档（无分动作计数）迁移 → 计数归零、只留 FIRST 贴纸
    old1 = struct.pack("<IBBBBBBHH",
                       PET_MAGIC, 1, 80, 80, 80, 90, 0, 0, 10)
    rc, out, sa, intro = apply_v4(old1)
    assert rc == 1 and sa == 0 and intro == 1
    assert out[0].h == 80 and out[0].care == 10
    assert out[0].feed == 0 and out[0].play == 0   # v1 无分动作计数
    assert out[0].sticker == (1 << 0)
    print("[PASS] v1→v4 老档迁移：分动作计数归零、只留 FIRST 贴纸 全过")

    # 场景 10：非法 magic / 过短长度拒绝
    assert apply_v4(b"\x00\x00\x00\x00\x04" + b"\x00" * 100)[0] == -1   # bad magic
    assert apply_v4(b"") == (-2, None, 0, 0)
    assert apply_v4(b"\x31\x54\x45\x50\x04") == (-2, None, 0, 0)        # len<5
    print("[PASS] 非法存档：坏 magic / 长度不足 正确拒绝 全过")

    print("\n[ALL PASS] pet 逻辑冒烟验证（v4）：单宠 6 + 多宠存档 4 + 拒绝 1 = 11 场景全过")


if __name__ == "__main__":
    main()
