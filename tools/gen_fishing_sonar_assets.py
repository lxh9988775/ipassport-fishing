#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_fishing_sonar_assets.py — v6 鱼探换皮的构建期资源烘焙。

只产资源，不碰玩法：
  assets/fishing/sonar_bg.c    112x250 RGB565 声呐静态底图（刻度/水层/温跃层/底质/浮漂）
  assets/fishing/sonar_fish.c  4 张 15x10 ARGB8888 像素鱼（黄巡游/红蓄力/橙冲刺/灰歇息）
  assets/fishing/sonar_assets.h 上面的 extern 声明

父版：build/fish_ui/钓鱼v6_鱼探换皮试玩.html 的 drawSonar()/pixelFish()，
坐标按 240x320 逻辑分辨率，HUD 22px 由 fishing.c 的 g_reel 容器负责。
"""
from __future__ import annotations

import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "fishing")

SONAR_W, SONAR_H = 112, 250
CTRL_W, CTRL_H = 108, 250       # 控鱼台静态底图：屏幕 (128,38)，相对 g_reel y=16
FISH_W, FISH_H = 15, 10
PHASES = ["cruise", "warn", "burst", "rest"]
PHASE_RGB = [(255, 200, 70), (255, 96, 96), (255, 168, 56), (150, 170, 190)]

FONT_PATH = "C:/Windows/Fonts/simhei.ttf"
if not os.path.exists(FONT_PATH):
    FONT_PATH = "C:/Windows/Fonts/msyh.ttc"


def f(sz: int):
    return ImageFont.truetype(FONT_PATH, sz)


def sonar_y(m: float) -> int:
    """试玩页 sonarY：深度 0..12m -> 屏幕 y 56..282（绝对坐标）。"""
    return int(round(56 + (m / 12.0) * (282 - 56)))


# 底图局部坐标 = 屏幕坐标 - (x0=8, y0=38)
def ly(abs_y: int) -> int:
    return abs_y - 38


def draw_sonar_bg() -> Image.Image:
    im = Image.new("RGB", (SONAR_W, SONAR_H), (5, 16, 26))
    d = ImageDraw.Draw(im)

    # 窗框
    d.rectangle([0, 0, SONAR_W - 1, SONAR_H - 1], outline=(38, 74, 92))

    # 深度刻度：每 2m 一条横线 + 靠左的两位数字
    for m in range(0, 13, 2):
        y = ly(sonar_y(m))
        d.line([(13, y), (SONAR_W - 2, y)], fill=(17, 43, 57))
        d.text((3, y - 9), ("%2d" % m), font=f(7), fill=(64, 104, 122))

    # 三层水层色带（靠左竖条）+ 靠右缘标签，避开回波轨迹
    d.rectangle([12, ly(sonar_y(0)), 14, ly(sonar_y(1))], fill=(70, 150, 158))
    d.rectangle([12, ly(sonar_y(1)), 14, ly(sonar_y(5))], fill=(72, 122, 176))
    d.rectangle([12, ly(sonar_y(5)), 14, ly(sonar_y(12))], fill=(196, 148, 84))
    d.text((SONAR_W - 32, ly(sonar_y(0)) + 2), "表层", font=f(7), fill=(70, 150, 158))
    d.text((SONAR_W - 32, ly(sonar_y(2)) - 5), "中层", font=f(7), fill=(72, 122, 176))
    d.text((SONAR_W - 32, ly(sonar_y(6)) - 5), "底层", font=f(7), fill=(196, 148, 84))

    # 温跃层
    d.line([(16, ly(sonar_y(3.4))), (SONAR_W - 2, ly(sonar_y(3.4)))], fill=(40, 72, 88))

    # 底质线：亮粗=硬底、细弱=软底；止于线组左侧（fx=106 -> 局部 98）
    fx_local = 98
    for xx in range(16, fx_local - 3):
        t = (xx - 16) / max(1, (fx_local - 3 - 16))
        by = ly(int(round(sonar_y(10.2) + 3 * np.sin(t * 11) + 2 * np.sin(t * 23))))
        hard = (0.18 < t < 0.42) or (0.66 < t < 0.86)
        d.line([(xx, by), (xx, by + (2 if hard else 0))],
               fill=(224, 168, 76) if hard else (68, 108, 118))

    # 水面波纹
    surf = ly(sonar_y(0))
    for xx in range(18, SONAR_W - 3, 8):
        d.line([(xx, surf + ((xx // 8) % 2)), (xx + 5, surf + ((xx // 8) % 2))],
               fill=(78, 147, 166))

    # 浮漂：红白两段 + 细杆，固定在声呐窗水面位置
    d.line([(fx_local, surf - 10), (fx_local, surf - 6)], fill=(207, 227, 239))
    d.rectangle([fx_local - 1, surf - 6, fx_local + 1, surf - 1], fill=(242, 246, 250))
    d.rectangle([fx_local - 1, surf - 6, fx_local + 1, surf - 4], fill=(255, 96, 96))

    # 标题（当前深度是动态 label，不烘进底图）
    d.text((18, 1), "鱼探 · 线组实时", font=f(8), fill=(108, 190, 174))
    return im


def draw_ctrl_bg() -> Image.Image:
    """控鱼台静态底图 108x250：像素硬边 bevel 框架 + 静态中文。

    局部坐标 = 屏幕坐标 - (128,38)。外深 1px + 顶/左亮 + 底/右暗，
    与试玩母版 drawReelTrack() 的 bevel() 逐像素一致。
    动态元素（分段格 / 百分比 / 鱼名 / 四拍徽章 / 绿区 / 鱼标 / 竿速绿块）
    由 fishing.c 的 lv_obj 叠加，不烘进本图。
    """
    im = Image.new("RGB", (CTRL_W, CTRL_H), (5, 13, 22))
    d = ImageDraw.Draw(im)

    def bevel(x, y, w, h, base):
        d.rectangle([x - 1, y - 1, x + w, y + h], fill=(5, 13, 22))
        d.rectangle([x, y, x + w - 1, y + h - 1], fill=base)
        d.line([(x, y), (x + w - 1, y)], fill=(91, 125, 152))
        d.line([(x, y), (x, y + h - 1)], fill=(91, 125, 152))
        d.line([(x, y + h - 1), (x + w - 1, y + h - 1)], fill=(27, 46, 66))
        d.line([(x + w - 1, y), (x + w - 1, y + h - 1)], fill=(27, 46, 66))

    # 主面板：外框 bevel
    bevel(0, 0, CTRL_W, CTRL_H, (16, 35, 58))
    # 标题栏
    d.rectangle([2, 4, 105, 20], fill=(23, 47, 71))
    d.text((6, 7), "控 鱼 台", font=f(9), fill=(232, 240, 248))
    # 右上相位块占位（四拍色由动态徽章叠加）
    d.rectangle([95, 9, 101, 15], fill=(12, 30, 50))
    # 收线标题
    d.text((6, 27), "收线", font=f(9), fill=(143, 168, 192))
    # 分段进度槽
    bevel(6, 42, 98, 11, (8, 22, 36))
    # 鱼名徽章槽
    bevel(6, 58, 98, 18, (12, 30, 50))
    # 子面板（线轮 + 控鱼轨道）
    bevel(6, 82, 98, 140, (8, 22, 36))
    d.text((11, 85), "线轮", font=f(8), fill=(127, 168, 192))
    d.text((38, 85), "控鱼轨道", font=f(8), fill=(127, 168, 192))
    # 线轮槽
    d.rectangle([11, 98, 20, 215], fill=(5, 13, 22))
    # 轨道窗：外深 + 内底 + 顶左亮 + 底右暗 + 10 道刻度
    rx, ry, rw, rh = 36, 98, 32, 118
    d.rectangle([rx - 2, ry - 2, rx + rw + 1, ry + rh + 1], fill=(5, 13, 22))
    d.rectangle([rx, ry, rx + rw - 1, ry + rh - 1], fill=(18, 41, 67))
    d.line([(rx, ry), (rx + rw - 1, ry)], fill=(91, 125, 152))
    d.line([(rx, ry), (rx, ry + rh - 1)], fill=(91, 125, 152))
    d.line([(rx, ry + rh - 1), (rx + rw - 1, ry + rh - 1)], fill=(27, 46, 66))
    d.line([(rx + rw - 1, ry), (rx + rw - 1, ry + rh - 1)], fill=(27, 46, 66))
    for k in range(1, 10):
        yy = ry + k * rh // 10
        d.line([(rx + 2, yy), (rx + rw - 3, yy)], fill=(29, 60, 86))
    # 竿速
    d.text((11, 225), "竿速", font=f(8), fill=(127, 168, 192))
    # 两个按钮 bevel（未选中态 base；选中绿块由动态叠加，内缩留边）
    bevel(38, 225, 32, 17, (12, 30, 50))
    bevel(74, 225, 32, 17, (12, 30, 50))
    return im


def draw_fish_sprite(rgb) -> Image.Image:
    """侧视像素鱼：头朝左、尾在右；颜色随四拍。"""
    im = Image.new("RGBA", (FISH_W, FISH_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = rgb + (255,)

    # 尾鳍（两种摆尾帧共用同一张；尾尖靠图右缘）
    d.rectangle([11, 3, 14, 4], fill=c)
    d.rectangle([12, 1, 13, 2], fill=c)
    d.rectangle([12, 6, 13, 7], fill=c)
    # 嘴 / 身体 / 背鳍 / 腹鳍
    d.rectangle([0, 4, 2, 5], fill=c)
    d.rectangle([2, 2, 10, 7], fill=c)
    d.rectangle([4, 0, 7, 1], fill=c)
    d.rectangle([5, 8, 7, 8], fill=c)
    # 背部高光 + 眼睛
    d.rectangle([2, 3, 6, 3], fill=(255, 255, 255, 115))
    d.rectangle([2, 4, 3, 5], fill=(244, 251, 255, 255))
    d.point((2, 4), fill=(8, 23, 32, 255))
    return im


def rgb565_bytes(img: Image.Image) -> bytes:
    a = np.asarray(img.convert("RGB")).astype(np.uint16)
    v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    return v.astype("<u2").tobytes()


def argb8888_bytes(img: Image.Image) -> bytes:
    """LVGL 内存序是 B,G,R,A（不是 R,G,B,A）。"""
    a = np.asarray(img.convert("RGBA"))
    return a[..., [2, 1, 0, 3]].tobytes()


def fmt_bytes(data: bytes, per_line: int = 16) -> str:
    out = []
    for i in range(0, len(data), per_line):
        out.append("    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ",")
    return "\n".join(out)


def emit_c(path: str, banner: str, entries) -> int:
    parts = ["// 本文件由 tools/gen_fishing_sonar_assets.py 自动生成，请勿手工编辑。",
             "// %s" % banner, '#include "lvgl.h"', '#include "sonar_assets.h"', ""]
    total = 0
    for name, cf, w, h, data in entries:
        bpp = 2 if cf == "LV_COLOR_FORMAT_RGB565" else 4
        total += len(data)
        parts += ["static const uint8_t %s_map[] = {" % name, fmt_bytes(data), "};", "",
                  "const lv_image_dsc_t %s = {" % name,
                  "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
                  "    .header.cf = %s," % cf,
                  "    .header.w = %d," % w,
                  "    .header.h = %d," % h,
                  "    .header.stride = %d," % (w * bpp),
                  "    .data_size = sizeof(%s_map)," % name,
                  "    .data = %s_map," % name, "};", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(parts))
    return total


def main() -> int:
    os.makedirs(OUT, exist_ok=True)

    bg = emit_c(os.path.join(OUT, "sonar_bg.c"),
                "声呐静态底图（112x250，RGB565 不透明，永不 recolor）",
                [("sonar_bg", "LV_COLOR_FORMAT_RGB565", SONAR_W, SONAR_H,
                  rgb565_bytes(draw_sonar_bg()))])

    ctrl = emit_c(os.path.join(OUT, "ctrl_bg.c"),
                  "控鱼台静态底图（108x250，RGB565 不透明，像素 bevel 框架与静态中文）",
                  [("ctrl_bg", "LV_COLOR_FORMAT_RGB565", CTRL_W, CTRL_H,
                    rgb565_bytes(draw_ctrl_bg()))])

    fish_entries = []
    for name, rgb in zip(PHASES, PHASE_RGB):
        fish_entries.append(("sonar_fish_" + name, "LV_COLOR_FORMAT_ARGB8888",
                             FISH_W, FISH_H, argb8888_bytes(draw_fish_sprite(rgb))))
    sprites = emit_c(os.path.join(OUT, "sonar_fish.c"),
                     "像素鱼 sprite（15x10，四拍切图；最小包围盒透明图）", fish_entries)

    hdr = ["// 本文件由 tools/gen_fishing_sonar_assets.py 自动生成，请勿手工编辑。",
           "#pragma once", '#include "lvgl.h"', "", "#ifdef __cplusplus",
           'extern "C" {', "#endif", "",
           "extern const lv_image_dsc_t sonar_bg;",
           "extern const lv_image_dsc_t ctrl_bg;"]
    hdr += ["extern const lv_image_dsc_t sonar_fish_%s;" % n for n in PHASES]
    hdr += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    with open(os.path.join(OUT, "sonar_assets.h"), "w", encoding="utf-8", newline="\n") as fp:
        fp.write("\n".join(hdr))

    total = bg + ctrl + sprites
    print("sonar_bg: %d bytes (112x250 RGB565)" % bg)
    print("ctrl_bg: %d bytes (108x250 RGB565)" % ctrl)
    print("fish sprites: %d bytes (4 x 15x10 ARGB8888)" % sprites)
    # 预算说明：执行书 v6 写的 80KB 是「单张声呐底图」阶段的预估；S4 打回后为
    # 修复「纯直角大色块」缺陷，控鱼台 bevel 框架改为整张 RGB565 贴图（+54KB）。
    # 合计 ~110KB，板子 Flash 空闲约 5.48MB，占用增加 1.6 个百分点，安全。
    BUDGET = 128 * 1024
    print("total: %d bytes (%.1f KB), budget 128 KB -> %s" %
          (total, total / 1024.0, "PASS" if total < BUDGET else "FAIL"))
    return 0 if total < BUDGET else 1


if __name__ == "__main__":
    sys.exit(main())
