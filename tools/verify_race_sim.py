#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""赛跑对战平衡对拍模拟：与 main/pet.c refresh_game case 0 / game_input case 0 同式同参。

依据执行书 3.3 定稿参数（试玩 build_pet_race_playable.py 的 C 端镜像）：
  狂按 7 次/秒 + 会跳   → 赢全部四档
  中速 4 次/秒 + 会跳   → 赢粘人精、输小傲娇
  完全不按              → 必输
"""
import math
import random

LEN = 120000          # mm
STRIDE_ALT, STRIDE_SAME, CAP = 220, 30, 1000
DECAY_U = 900         # 每秒掉 900 定点单位
SPD_BASE, SPD_MOM, SPD_STUMBLE = 8000, 30000, 7000
JUMP_MS, STUMBLE_MS, COUNTDOWN = 460, 500, 3200
ROCK = [26000, 55000, 86000]
CAR = [18000, 44000, 73000]
CAR_MOM, RB_BONUS = 350, 4000
CATCHUP_MM, CATCHUP_K = 10000, 720
WAVE_BASE, WAVE_AMP = 930, 100
WANDER_BASE, WANDER_AMP = 820, 360
SLOWSTART_K, SLOWSTART_MS = 620, 2000
OPP_SPD = [21000, 25000, 17000, 22000]
OPP_JUMP = [620, 900, 950, 450]
OPP_NAME = ["粘人精", "小傲娇", "慢性子", "好奇宝宝"]
DT = 50               # 与固件 UI_REFRESH_MS 一致


def race(opp, taps_per_sec, can_jump, seed=42):
    """返回 (玩家米, 兔子米)。taps_per_sec=0 表示完全不按。"""
    random.seed(seed)
    mom = dist = rb = 0
    jump_t = rjump_t = stumble_t = elapsed = 0
    last_leg = -1
    prev_me = prev_rb = 0
    obs_hit = [0] * 3
    obs_rhit = [0] * 3
    car_got = [0] * 3
    car_rgot = [0] * 3
    tap_interval = 1000.0 / taps_per_sec if taps_per_sec else 0
    next_tap = tap_interval

    for ms in range(COUNTDOWN, COUNTDOWN + 60000, DT):
        elapsed += DT
        # —— 输入模拟 ——
        if taps_per_sec and ms >= next_tap:
            next_tap += tap_interval
            leg = (last_leg + 1) % 2          # 严格交替
            mom = min(CAP, mom + (STRIDE_ALT if leg != last_leg else STRIDE_SAME))
            last_leg = leg
        if can_jump and jump_t <= 0:
            spd = SPD_STUMBLE if stumble_t > 0 else SPD_BASE + SPD_MOM * mom // CAP
            for d in ROCK:
                if not any(dist >= d for d in ROCK[:ROCK.index(d)]) and 0 < d - dist <= spd * 0.15:
                    jump_t = JUMP_MS
                    break
        # —— 推进（同 C 端 stepRace） ——
        jump_t = max(0, jump_t - DT)
        rjump_t = max(0, rjump_t - DT)
        stumble_t = max(0, stumble_t - DT)
        prev_me, prev_rb = dist, rb
        mom = max(0, mom - DECAY_U * DT // 1000)
        spd = SPD_STUMBLE if stumble_t > 0 else SPD_BASE + SPD_MOM * mom // CAP
        dist += spd * DT // 1000
        tf = elapsed / 1000.0
        rv = OPP_SPD[opp] * (WAVE_BASE + int(WAVE_AMP * math.sin(tf * 2.6 + opp))) // 1000
        if opp == 3:
            rv = rv * (WANDER_BASE + int(WANDER_AMP * abs(math.sin(tf * 1.7)))) // 1000
        if opp == 0 and rb - dist > CATCHUP_MM:
            rv = rv * CATCHUP_K // 1000
        if opp == 2 and elapsed < SLOWSTART_MS:
            rv = rv * SLOWSTART_K // 1000
        rb += rv * DT // 1000
        for i in range(3):
            if not obs_hit[i] and prev_me < ROCK[i] <= dist:
                obs_hit[i] = 1
                if jump_t <= 0:
                    stumble_t = STUMBLE_MS
                    mom //= 2
            if not obs_rhit[i] and prev_rb < ROCK[i] <= rb:
                obs_rhit[i] = 1
                if random.randint(0, 999) >= OPP_JUMP[opp]:
                    rjump_t = 500
            if not car_got[i] and prev_me < CAR[i] <= dist:
                car_got[i] = 1
                mom = min(CAP, mom + CAR_MOM)
            if not car_rgot[i] and prev_rb < CAR[i] <= rb:
                car_rgot[i] = 1
                rb += RB_BONUS
        if dist >= LEN or rb >= LEN:
            return min(dist, LEN), min(rb, LEN)
    return min(dist, LEN), min(rb, LEN)


def main():
    print("=== 对拍 1：狂按 7 次/秒 + 会跳 → 应赢全部四档 ===")
    ok1 = True
    for opp in range(4):
        me, rb = race(opp, 7, True)
        win = me >= rb
        ok1 &= win
        print(f"  vs {OPP_NAME[opp]}: 你 {me//1000}m / 兔 {rb//1000}m → {'赢' if win else '输'}")
    print("=== 对拍 2：中速 4 次/秒 + 会跳 → 赢粘人精、输小傲娇 ===")
    me0, rb0 = race(0, 4, True)
    me1, rb1 = race(1, 4, True)
    w0, w1 = me0 >= rb0, me1 >= rb1
    print(f"  vs 粘人精: 你 {me0//1000}m / 兔 {rb0//1000}m → {'赢' if w0 else '输'}")
    print(f"  vs 小傲娇: 你 {me1//1000}m / 兔 {rb1//1000}m → {'赢' if w1 else '输'}")
    ok2 = w0 and not w1
    print("=== 对拍 3：完全不按 → 必输 ===")
    ok3 = True
    for opp in range(4):
        me, rb = race(opp, 0, False)
        lose = me < rb
        ok3 &= lose
        print(f"  vs {OPP_NAME[opp]}: 你 {me//1000}m / 兔 {rb//1000}m → {'输(正确)' if lose else '赢(异常)'}")
    print()
    verdict = ok1 and ok2 and ok3
    print("对拍结论:", "全部通过 ✅" if verdict else "存在偏差 ❌（禁止提交，回 S2 整改）")
    return 0 if verdict else 1


if __name__ == "__main__":
    raise SystemExit(main())
