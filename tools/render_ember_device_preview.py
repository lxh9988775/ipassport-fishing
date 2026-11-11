#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 assets/ember/ember_assets.c 反解出真机上板效果预览（验证用）。

反解的是即将编译进固件的 C 数组本身，不是源图，所以能真实反映：
  * 右侧白线是否已消除
  * UI 贴图（HUD 大数字 / 四灯 / 键提示 / 覆盖屏文字）是否可见
  * 精灵透明层与背景合成是否正确、有无红蓝翻转
产物写入 build/ember_compare/device_*.png。
"""
import os, re, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C = os.path.join(ROOT, "assets", "ember", "ember_assets.c")
OUT = os.path.join(ROOT, "build", "ember_compare")
W, H = 240, 320
SP_W, SP_H = 190, 320

src = open(C, encoding="utf-8").read()


def check_symbols():
    """CI 曾因 dsc 名多拼一层前缀导致 undefined reference；本地先卡住。"""
    defs = set(re.findall(r"const lv_img_dsc_t (\w+)(?:\[[^\]]*\])?\s*=", src))
    hdr = open(os.path.join(ROOT, "assets", "ember", "ember_assets.h"),
               encoding="utf-8").read()
    fw = open(os.path.join(ROOT, "main", "ember.c"), encoding="utf-8").read()
    need_h = set(re.findall(r"extern const lv_img_dsc_t (\w+)", hdr))
    need_fw = set(re.findall(r"extern const lv_img_dsc_t (\w+)", fw))
    miss_h, miss_fw = need_h - defs, need_fw - defs
    print("dsc 定义 %d 个 | 头文件 extern %d 个 | 固件 extern %d 个"
          % (len(defs), len(need_h), len(need_fw)))
    if miss_h:
        print("头文件声明但 C 里没有:", sorted(miss_h))
    if miss_fw:
        print("固件 extern 但 C 里没有:", sorted(miss_fw))
    if miss_h or miss_fw:
        print("RESULT: SYMBOL MISMATCH ❌")
        sys.exit(1)
    print("RESULT: SYMBOLS OK ✅")


check_symbols()


def get_arr(name, dtype):
    m = re.search(r"static const %s %s\[\d*\] = \{(.*?)\n\};" % (dtype, name), src, re.S)
    if not m:
        raise RuntimeError("array not found: " + name)
    toks = re.findall(r"0x[0-9A-Fa-f]+|\b0\b", m.group(1))
    return np.array([int(t, 16) if t.startswith("0x") else 0 for t in toks],
                    dtype=np.uint32 if dtype == "uint32_t" else np.uint16)


def rgb565(flat, w, h):
    v = flat.reshape(h, w).astype(np.uint32)
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.uint8)


def argb8888(flat, w, h):
    v = flat.reshape(h, w).astype(np.uint32)
    return np.stack([(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, (v >> 24) & 0xFF],
                    -1).astype(np.uint8)


def over(base, layer, x=0, y=0):
    """alpha composite RGBA layer onto RGB base."""
    out = base.astype(np.float32).copy()
    h, w = layer.shape[:2]
    a = (layer[:, :, 3].astype(np.float32) / 255.0)[..., None]
    region = out[y:y+h, x:x+w]
    out[y:y+h, x:x+w] = region * (1 - a) + layer[:, :, :3].astype(np.float32) * a
    return np.clip(out, 0, 255).astype(np.uint8)


bg = rgb565(get_arr("ember_bg_map", "uint16_t"), W, H)
spirit = argb8888(get_arr("ember_spirit_map_0", "uint32_t"), SP_W, SP_H)
card = rgb565(get_arr("ember_card_map_0", "uint16_t"), W, H)
wood = [rgb565(get_arr("ember_wood_%d" % i, "uint16_t"), 96, 54) for i in range(6)]
hint = [rgb565(get_arr("ember_hint_%d" % i, "uint16_t"), 240, 30) for i in range(9)]
lamp = [argb8888(get_arr("ember_lamp_%d" % i, "uint32_t"), 160, 40) for i in range(16)]
sel = [argb8888(get_arr("ember_sel_%d" % i, "uint32_t"), 160, 40) for i in range(4)]
ovl = [rgb565(get_arr("ember_ovl_%d" % i, "uint16_t"), 240, 80) for i in range(4)]

os.makedirs(OUT, exist_ok=True)


def paste_rgb(base, layer, x, y):
    """不透明 RGB565 层：直接覆盖拷贝，不做混合（与真机 LVGL 行为一致）。"""
    out = base.copy()
    h, w = layer.shape[:2]
    out[y:y+h, x:x+w] = layer
    return out


def scene():
    return over(bg, spirit, 0, 0)


def main_screen(wood_n=5, bits=0, sel_i=0, hint_i=1):
    im = scene()
    im = paste_rgb(im, wood[wood_n], 8, 5)
    im = over(im, lamp[bits], 40, 247)
    im = over(im, sel[sel_i], 40, 247)
    im = paste_rgb(im, hint[hint_i], 0, 288)
    return im


def overlay_screen(kind, hint_i):
    """覆盖屏：文字条 + 底部提示条，中间完整透出夜森林与火灵。"""
    im = scene()
    im = paste_rgb(im, ovl[kind], 0, 100)
    im = paste_rgb(im, hint[hint_i], 0, 288)
    return im


shots = {
    "device_S1_tonight": main_screen(5, 0, 0, 1),
    "device_S2_add":     main_screen(3, 3, 1, 2),
    "device_S2_empty":   main_screen(0, 15, 3, 8),
    "device_S0_first":   overlay_screen(0, 0),
    "device_S3_phase":   overlay_screen(1, 3),
    "device_S7_borrow":  overlay_screen(3, 7),
    "device_S4_card":    card,
}
for name, arr in shots.items():
    Image.fromarray(arr, "RGB").save(os.path.join(OUT, name + ".png"))

# 2x4 网格
font = ImageFont.truetype(os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf"), 13)
grid = Image.new("RGB", (W * 4 + 50, H * 2 + 60), (24, 24, 24))
d = ImageDraw.Draw(grid)
for i, (name, arr) in enumerate(shots.items()):
    x = 10 + (i % 4) * (W + 10)
    y = 26 + (i // 4) * (H + 30)
    grid.paste(Image.fromarray(arr, "RGB"), (x, y))
    d.text((x, y - 18), name, font=font, fill=(255, 220, 150))
grid.save(os.path.join(OUT, "device_grid.png"))

# 白边校验：右 4 列不应再有近白像素
right = bg[:, -4:, :]
white = int(((right > 225).all(axis=2)).sum())
print("bg right 4 cols near-white pixels:", white, "(期望 0)")
print("bg right edge sample y=160:", tuple(int(v) for v in bg[160, 239]))
print("spirit nonzero alpha px:", int((spirit[:, :, 3] > 8).sum()))
print("previews:", ", ".join(shots.keys()))
print("grid:", os.path.join(OUT, "device_grid.png"))
