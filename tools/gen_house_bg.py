#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_house_bg.py — 生成「口袋兔」主屏房间内景背景（RGB565，240x320）。

设计（对齐钓鱼 bg_sprites 的 RGB565 导出约定）：
  - 先在 120x160 低分辨率画布上用 numpy 逐像素画出像素风房间（墙纸竖条纹 +
    窗户 + 相框 + 落地灯 + 书桌 + 木椅 + 木地板 + 踢脚线），再 NEAREST 2x 放大到
    240x320，保证像素颗粒感一致、不糊。
  - 整屏不透明、铺底，省 flash（RGB565 = 2 字节/像素，240*320*2 = 150KB）。

用途：
  - 主屏 build_home() 用 lv_img 全屏铺底，替换原 CREAM 纯色底 + 草地条。
  - 小游戏页 g_layer 复用同一 pet_room 背景。

产物：
  assets/sprites/pet_room.c
  assets/sprites/pet_room.h
"""
import os
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "sprites")

SRC_W, SRC_H = 120, 160
DST_W, DST_H = 240, 320

# 调色板（暖色系、像素风、低饱和，保证灰兔在墙上清晰）
WALL_A = (240, 226, 212)   # 墙纸主色
WALL_B = (229, 213, 196)   # 墙纸竖条纹（交替）
BASEBOARD = (150, 118, 88) # 踢脚线（木色）
FLOOR_A = (190, 150, 102)  # 地板主色
FLOOR_B = (172, 132, 88)   # 地板缝隙/木纹
FRAME_W = (250, 248, 244)  # 窗户/相框白边
SKY = (152, 202, 240)      # 窗外蓝天
CLOUD = (255, 255, 255)    # 云
ART_SKY = (183, 222, 240)  # 相框里天空
ART_HILL = (174, 212, 150) # 相框里草地
ART_SUN = (252, 232, 150)  # 相框里太阳
HEART = (196, 30, 80)      # 锐胜红点缀（相框里小红心）
LAMP_POLE = (96, 74, 56)   # 灯杆/底座（深木）
LAMP_SHADE = (250, 226, 142)  # 灯罩（暖黄）
LAMP_GLOW = (255, 244, 196)   # 灯光晕
DESK = (176, 126, 78)      # 书桌木
DESK_DK = (150, 104, 62)   # 书桌暗部
CHAIR = (172, 122, 76)     # 椅子木
CHAIR_DK = (146, 100, 60)
RUG = (220, 182, 152)      # 地毯
RUG_BD = (198, 152, 122)   # 地毯边


def rect(img, x0, y0, x1, y1, color):
    if x1 < 0 or y1 < 0 or x0 >= img.shape[1] or y0 >= img.shape[0]:
        return
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(img.shape[1] - 1, x1); y1 = min(img.shape[0] - 1, y1)
    img[y0:y1 + 1, x0:x1 + 1] = color


def fill(img, color):
    img[:, :] = color


def ellipse(img, cx, cy, rx, ry, color):
    h, w = img.shape[:2]
    ys, xs = np.ogrid[:h, :w]
    m = ((xs - cx) / rx) ** 2 + ((ys - cy) / ry) ** 2 <= 1.0
    img[m] = color


def draw_room():
    img = np.zeros((SRC_H, SRC_W, 3), dtype=np.uint8)
    # 墙纸（竖条纹）
    for x in range(SRC_W):
        c = WALL_A if (x // 8) % 2 == 0 else WALL_B
        img[0:128, x] = c
    # 踢脚线（墙与地板交界）
    rect(img, 0, 120, SRC_W - 1, 127, BASEBOARD)
    # 地板（木纹横条 + 竖缝）
    for y in range(128, SRC_H):
        img[y, :] = FLOOR_A if ((y - 128) // 9) % 2 == 0 else FLOOR_B
    for x in range(0, SRC_W, 22):
        rect(img, x, 128, x, SRC_H - 1, (150, 114, 74))

    # ---- 窗户（右上）----
    wx0, wy0, wx1, wy1 = 74, 16, 110, 54
    rect(img, wx0, wy0, wx1, wy1, FRAME_W)            # 白边框
    rect(img, wx0 + 3, wy0 + 3, wx1 - 3, wy1 - 3, SKY)  # 玻璃天空
    ellipse(img, wx0 + 14, wy0 + 16, 8, 5, CLOUD)
    ellipse(img, wx0 + 22, wy0 + 18, 6, 4, CLOUD)
    ellipse(img, wx1 - 16, wy0 + 14, 7, 5, CLOUD)
    rect(img, wx0 + 16, wy0 + 3, wx0 + 18, wy1 - 3, FRAME_W)  # 竖中梃
    rect(img, wx0 + 3, wy0 + 24, wx1 - 3, wy0 + 26, FRAME_W)  # 横中梃

    # ---- 相框（左上，小风景画 + 红心）----
    px0, py0, px1, py1 = 14, 18, 46, 46
    rect(img, px0, py0, px1, py1, FRAME_W)
    rect(img, px0 + 3, py0 + 3, px1 - 3, py1 - 3, ART_SKY)
    rect(img, px0 + 3, py1 - 12, px1 - 3, py1 - 3, ART_HILL)
    ellipse(img, px0 + 10, py0 + 10, 4, 4, ART_SUN)
    # 红心（两瓣 + 下尖）
    rect(img, px0 + 18, py0 + 14, px0 + 19, py0 + 15, HEART)
    rect(img, px0 + 22, py0 + 14, px0 + 23, py0 + 15, HEART)
    rect(img, px0 + 19, py0 + 16, px0 + 22, py0 + 16, HEART)
    rect(img, px0 + 20, py0 + 17, px0 + 21, py0 + 18, HEART)

    # ---- 落地灯（左侧）----
    rect(img, 15, 72, 18, 128, LAMP_POLE)             # 灯杆
    ellipse(img, 16, 130, 8, 3, LAMP_POLE)            # 底座
    # 灯罩（梯形：上窄下宽）
    for i, y in enumerate(range(54, 74)):
        half = 4 + i // 2
        rect(img, 16 - half, y, 16 + half, y, LAMP_SHADE)
    ellipse(img, 16, 76, 9, 4, LAMP_GLOW)             # 灯下光晕

    # ---- 书桌（中右，靠下）----
    rect(img, 58, 100, 114, 108, DESK)                 # 桌面
    rect(img, 58, 108, 114, 110, DESK_DK)              # 桌面暗边
    rect(img, 62, 110, 67, 140, DESK_DK)               # 左腿
    rect(img, 108, 110, 113, 140, DESK_DK)             # 右腿
    rect(img, 70, 104, 102, 106, (200, 160, 110))      # 桌上一本书

    # ---- 木椅（左中）----
    rect(img, 28, 112, 54, 118, CHAIR)                 # 座面
    rect(img, 28, 118, 54, 120, CHAIR_DK)
    rect(img, 28, 96, 33, 118, CHAIR)                  # 靠背
    rect(img, 28, 96, 33, 98, CHAIR_DK)
    rect(img, 30, 120, 33, 140, CHAIR_DK)              # 前腿
    rect(img, 49, 120, 52, 140, CHAIR_DK)              # 后腿

    # ---- 地毯（地板中部椭圆）----
    ellipse(img, 60, 150, 34, 9, RUG)
    ellipse(img, 60, 150, 30, 7, RUG_BD)

    return img


# ---------------------------------------------------------------------------
# RGB565 导出（与 gen_sprites.py 同字节序：低字节 + 高字节 little-endian）
# ---------------------------------------------------------------------------
def rgb565_bytes(img):
    a = np.asarray(img).astype(np.uint16)
    v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    return v.astype("<u2").tobytes()


def fmt_bytes(data, per_line=16):
    return "\n".join(
        "    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ","
        for i in range(0, len(data), per_line)
    )


def emit_c(path, name, w, h, data):
    parts = [
        "// 本文件由 tools/gen_house_bg.py 自动生成，请勿手工编辑。",
        "// 主屏房间内景背景（RGB565，240x320）",
        '#include "lvgl.h"',
        '#include "pet_room.h"',
        "",
        "static const uint8_t %s_map[] = {" % name,
        fmt_bytes(data),
        "};",
        "",
        "const lv_image_dsc_t %s = {" % name,
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        "    .header.cf = LV_COLOR_FORMAT_RGB565,",
        "    .header.w = %d," % w,
        "    .header.h = %d," % h,
        "    .header.stride = %d," % (w * 2),
        "    .data_size = sizeof(%s_map)," % name,
        "    .data = %s_map," % name,
        "};",
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))


def emit_header(path, name):
    parts = [
        "// 本文件由 tools/gen_house_bg.py 自动生成，请勿手工编辑。",
        "#pragma once",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "extern const lv_image_dsc_t %s;" % name,
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    small = draw_room()
    big = Image.fromarray(small, "RGB").resize((DST_W, DST_H), Image.NEAREST)
    data = rgb565_bytes(big)
    emit_c(os.path.join(OUT_DIR, "pet_room.c"), "pet_room", DST_W, DST_H, data)
    emit_header(os.path.join(OUT_DIR, "pet_room.h"), "pet_room")
    print("[gen_house_bg] pet_room: %dx%d RGB565, %d bytes (%.1f KB flash)"
          % (DST_W, DST_H, len(data), len(data) / 1024.0))
    print("[gen_house_bg] wrote: assets/sprites/{pet_room.c,pet_room.h}")


if __name__ == "__main__":
    main()
