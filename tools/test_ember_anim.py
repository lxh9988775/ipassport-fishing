#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M1「火会喘气」动画数值镜像 —— 固件 ember_anim.c 的 host 侧对拍基准。

不依赖 LVGL / ESP-IDF，纯数值。验证三件事（V3 方案 M1 门槛）：
  1) 火势 fire 按夜型衰减，下限 6，10 秒可见变化；
  2) 每帧 alpha 混合像素数 < 10000（小图块面积账）；
  3) 呼吸环 3.2s 周期，8 档切档，无判定语义。

FPGA 侧的 C 实现须与本镜像逐项对齐；本脚本输出即为 diag 期望值。
"""
import math

# --- 与 V3 方案 / ember_facts.json 同源的常量 ---
FIRE_MIN, FIRE_MAX, FIRE_START = 6.0, 100.0, 62.0
DECAY = {"静夜": 0.55, "风夜": 0.95, "雨夜": 0.80, "访客夜": 0.50}  # 点/秒
BREATH_PERIOD_MS = 3200
BREATH_N = 8
ANIM_TICK_MS = 100          # 10Hz 动画 tick；只在有变化时才 invalidate
TRANSITION_MS = 2000        # 火色过渡窗，到位即停

# --- 图块面积：与 main/ember.c 实际接线、gen_ember_assets.py 实际生成的图块一一对应 ---
# M1 只启用 4 类小图块；光晕/焰尖连续补间属换色过渡(M2)，本阶段未生成未启用。
AREA_BREATH = 40 * 40       # 呼吸环 1 个（切 8 档 src）
N_SPARK, AREA_SPARK = 6, 4 * 4
N_MOTE,  AREA_MOTE  = 8, 2 * 2
N_RAIN,  AREA_RAIN  = 12, 2 * 10

def frame_blend_px(n_spark, n_mote, n_rain):
    """一帧真正发生 alpha 混合的像素数（LVGL 只混合活跃/移动的小对象）。"""
    return AREA_BREATH + n_spark * AREA_SPARK + n_mote * AREA_MOTE + n_rain * AREA_RAIN

def fire_after(weather, seconds, start=FIRE_START):
    return max(FIRE_MIN, start - DECAY[weather] * seconds)

def breath_frame(t_ms):
    """3.2s 一周，8 档往复：0→7→0（sin 映射到档位）。无命中区。"""
    ph = (t_ms % BREATH_PERIOD_MS) / BREATH_PERIOD_MS
    return int(round((0.5 - 0.5 * math.cos(ph * 2 * math.pi)) * (BREATH_N - 1)))

def main():
    ok = True
    print("== M1 动画数值镜像 ==")

    # 1) 火势衰减 + 10s 可见变化
    for w, d in DECAY.items():
        f0, f10 = fire_after(w, 0), fire_after(w, 10)
        delta = f0 - f10
        print(f"衰减 {w:4s} {d:.2f}/s : {f0:.1f} -> {f10:.1f} (10s 降 {delta:.1f})")
        assert delta > 4.0, f"{w} 10s 变化不足 4 点，肉眼不明显"
        assert f10 >= FIRE_MIN
    # 下限：长时间也不灭
    assert fire_after("风夜", 1000) == FIRE_MIN
    print("火势衰减: PASS（10s 均 >4 点可见变化，下限 6 不灭）")

    # 2) 每帧混合面积账（最坏情况：火星/萤火/雨丝全开）
    worst = frame_blend_px(N_SPARK, N_MOTE, N_RAIN)
    print_px = worst
    print(f"\n最坏帧混合: 呼吸环{AREA_BREATH} "
          f"+ 火星{N_SPARK}*{AREA_SPARK} + 萤火{N_MOTE}*{AREA_MOTE} + 雨丝{N_RAIN}*{AREA_RAIN} "
          f"= {worst} px")
    if worst >= 10000:
        print("  超 10000！需削减粒子数")
        ok = False
    else:
        print(f"  < 10000 ✓（余量 {10000-worst} px）")

    # 常态帧（静夜：无雨，火星6萤火8）
    normal = frame_blend_px(6, 8, 0)
    print(f"静夜常态帧: {normal} px（diag 期望值）")
    if normal >= 10000:
        ok = False

    # 3) 呼吸环周期与档位
    print("\n呼吸环 3.2s 档位序列:")
    seq = [breath_frame(i * ANIM_TICK_MS) for i in range(0, 33)]
    print("  ", seq)
    assert seq[0] == 0 and seq[16] == 7, "半周期应到右端"
    assert 0 <= min(seq) <= max(seq) <= BREATH_N - 1
    print(f"呼吸环: PASS（{BREATH_PERIOD_MS}ms 周期，0..{BREATH_N-1} 档往复，无判定语义）")

    print("\nRESULT:", "PASS" if ok else "FAIL", "| diag blend/frame worst =", print_px)
    return 0 if ok else 1

if __name__ == "__main__":
    raise SystemExit(main())
