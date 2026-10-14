#!/usr/bin/env python3
"""把 mage-game/assets/*.png 转成 ESP32-C3 可编译的 C 数组（LVGL v9 图片源）。

【2026-10-14 修复用户问题②「人物怪物有黑色的底图」】
旧版把透明像素按背景色 0x0d0b1a 预乘烘焙进 RGB565 —— 这是错误结论：
LVGL v9.5 原生支持 LV_COLOR_FORMAT_RGB565A8（RGB565 色平面 + 独立 A8 透明度平面，
16bit 色深下的标准真透明格式）。本版改用 RGB565A8 真透明，黑底从根上消除。

数据布局（LVGL v9.5 lv_image_buf_get_alpha 的实现口径）：
  RGB565 色平面：w*h*2 字节（小端，行主序，stride = w*2）
  A8 透明度平面：紧跟其后 w*h 字节（行主序，无 stride）
  header.cf = LV_COLOR_FORMAT_RGB565A8, header.stride = w*2, data_size = w*h*3

策略（8MB Flash / 240x320 屏的现实约束）：
  - 全屏背景（title/town/battle_bg）不透明 → 纯 RGB565（省 1/3 内存）。
  - 怪物/主角/伙伴立绘需要透明 → RGB565A8，尺寸 1:1（禁缩放铁律）。
  - 怪物立绘由 tools/gen_mage_monsters.py 从 HTML 原版绘制函数 1:1 生成，
    自动发现 mage-game/assets/sp_*.png（排除 *_k.png 旧素材与 sp_hero.png 旧头图）。

输出：assets/mage_sprites.c + assets/mage_sprites.h
"""
import os
import glob
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "mage-game", "assets")
OUT_C = os.path.join(ROOT, "assets", "mage_sprites.c")
OUT_H = os.path.join(ROOT, "assets", "mage_sprites.h")

# ---- 不透明全屏背景：纯 RGB565，尺寸必须与屏幕 1:1 ----
OPAQUE = {
    "title":     ("title_art.png", 240, 320),
    "town":      ("town_art.png", 240, 320),
    "battle_bg": ("battle_bg_k.png", 240, 320),
}

# ---- 固定透明立绘（主角 / 伙伴猫），显示尺寸与旧版一致（用户已认可） ----
FIXED_ALPHA = {
    "sp_hero": ("sp_hero_k.png", 36, 46),
    "cat_partner": ("sp_cat_k.png", 29, 32),
}

# ---- 图标（面板头像，不透明） ----
ICONS = {
    "hero_icon":   ("hero.png", 80, 80),
    "cat_icon":    ("catspirit.png", 56, 56),
    "goblin_icon": ("goblin.png", 56, 56),
}

# 自动发现怪物立绘：sp_*.png，排除 *_k.png（旧预乘素材）、sp_hero.png（旧头图）、接触表
def discover_monsters():
    out = []
    for p in sorted(glob.glob(os.path.join(SRC, "sp_*.png"))):
        base = os.path.basename(p)
        if base.endswith("_k.png") or base == "sp_hero.png":
            continue
        key = base[:-4]  # 去掉 .png
        out.append((key, base))
    return out


def load_rgba(path, tw, th, cover_crop=False):
    """读 PNG -> (rgb像素列表, alpha列表)，缩放到 tw x th。"""
    im = Image.open(path)
    if im.mode not in ("RGBA", "LA") and "transparency" not in im.info:
        im = im.convert("RGB").convert("RGBA")
    im = im.convert("RGBA")
    if cover_crop:
        sw_, sh_ = im.size
        win = int(round(sh_ * (tw / th)))
        if win > sw_:
            win = sw_
        left = (sw_ - win) // 2
        im = im.crop((left, 0, left + win, sh_)).resize((tw, th), Image.LANCZOS)
    else:
        if im.size != (tw, th):
            im = im.resize((tw, th), Image.LANCZOS)
    px = list(im.getdata())  # (r,g,b,a) 元组
    return px


def c_array_of(vals, per_line=16):
    out = []
    for i in range(0, len(vals), per_line):
        chunk = vals[i:i + per_line]
        out.append("    " + ",".join(str(v) for v in chunk) + ",")
    return "\n".join(out)


def emit(parts, index, name, pixels, tw, th, fmt):
    """fmt: 'rgb565' 不透明 / 'rgb565a8' 真透明"""
    total_bytes = 0
    if fmt == "rgb565":
        byte_vals = []
        for (r, g, b, a) in pixels:
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            byte_vals.append(v & 0xFF)
            byte_vals.append((v >> 8) & 0xFF)
        cf = "LV_COLOR_FORMAT_RGB565"
        data_size = tw * th * 2
    else:
        # RGB565 色平面（不预乘！保留原始色）+ A8 平面
        byte_vals = []
        alpha_vals = []
        for (r, g, b, a) in pixels:
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            byte_vals.append(v & 0xFF)
            byte_vals.append((v >> 8) & 0xFF)
            alpha_vals.append(a & 0xFF)
        byte_vals.extend(alpha_vals)
        cf = "LV_COLOR_FORMAT_RGB565A8"
        data_size = tw * th * 3

    sym = name
    parts.append(f"/* ---- {sym}  {tw}x{th}  {fmt} ---- */")
    parts.append(f"static const uint8_t {sym}_map[{data_size}] = {{")
    parts.append(c_array_of(byte_vals, per_line=16))
    parts.append("};")
    parts.append("")
    parts.append(f"const lv_image_dsc_t {sym}_dsc = {{")
    parts.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
    parts.append(f"    .header.cf = {cf},")
    parts.append(f"    .header.w = {tw},")
    parts.append(f"    .header.h = {th},")
    parts.append(f"    .header.stride = {tw * 2},")
    parts.append(f"    .data_size = sizeof({sym}_map),")
    parts.append(f"    .data = {sym}_map,")
    parts.append("};")
    parts.append("")
    index.append((sym, tw, th))
    total_bytes = data_size
    return total_bytes


def main():
    parts = []
    parts.append("/* 本文件由 tools/gen_mage_sprites.py 自动生成，请勿手改。")
    parts.append(" * 源素材：mage-game/assets（怪物立绘由 gen_mage_monsters.py 从 HTML 原版")
    parts.append(" *   绘制函数 1:1 生成，其余与 HTML 原版同批图，不新增不重绘）。")
    parts.append(" * 格式：不透明 = LV_COLOR_FORMAT_RGB565；需透明 = LV_COLOR_FORMAT_RGB565A8")
    parts.append(" *   （RGB565 色平面 + 独立 A8 透明度平面，LVGL v9.5 原生支持，真透明无黑底）。")
    parts.append(" */")
    parts.append('#include "mage_sprites.h"')
    parts.append("")

    index = []
    total = 0

    for name, (src_name, tw, th) in OPAQUE.items():
        print(f"背景 {name} <- {src_name} ({tw}x{th}) rgb565")
        px = load_rgba(os.path.join(SRC, src_name), tw, th, cover_crop=(name == "battle_bg"))
        total += emit(parts, index, name, px, tw, th, "rgb565")

    for name, (src_name, tw, th) in FIXED_ALPHA.items():
        print(f"立绘 {name} <- {src_name} ({tw}x{th}) rgb565a8")
        px = load_rgba(os.path.join(SRC, src_name), tw, th)
        total += emit(parts, index, name, px, tw, th, "rgb565a8")

    for name, (src_name, tw, th) in ICONS.items():
        print(f"图标 {name} <- {src_name} ({tw}x{th}) rgb565")
        px = load_rgba(os.path.join(SRC, src_name), tw, th)
        total += emit(parts, index, name, px, tw, th, "rgb565")

    n_mon = 0
    for key, base in discover_monsters():
        path = os.path.join(SRC, base)
        im = Image.open(path)
        tw, th = im.size
        print(f"怪物 {key} <- {base} ({tw}x{th}) rgb565a8")
        px = load_rgba(path, tw, th)
        total += emit(parts, index, key, px, tw, th, "rgb565a8")
        n_mon += 1

    parts.append("const mage_sprite_t mage_sprites[] = {")
    for sym, w, h in index:
        parts.append(f"    {{ \"{sym}\", {w}, {h}, &{sym}_dsc }},")
    parts.append("};")
    parts.append("")
    parts.append(f"const int mage_sprite_count = {len(index)};")
    parts.append("")

    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
        f.write('\n')

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("""/* 本文件由 tools/gen_mage_sprites.py 自动生成，请勿手改。 */
#ifndef MAGE_SPRITES_H
#define MAGE_SPRITES_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char            *name;
    uint16_t               w;
    uint16_t               h;
    const lv_image_dsc_t  *dsc;
} mage_sprite_t;

extern const mage_sprite_t mage_sprites[];
extern const int mage_sprite_count;

#ifdef __cplusplus
}
#endif
#endif
""")

    print(f"\n共 {len(index)} 张贴图（其中怪物+BOSS {n_mon} 张），"
          f"像素数据约 {total / 1024:.0f}KB")
    print(f"输出：{OUT_C}")
    print(f"输出：{OUT_H}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
