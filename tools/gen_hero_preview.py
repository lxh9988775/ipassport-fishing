#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 4 阶立绘对比图（放大预览 + 上板实际 36x46 尺寸），用于烧录前给用户确认。
用法：
  python tools/gen_hero_preview.py before   # 读当前 sp_hero_t*.png（旧）
  python tools/gen_hero_preview.py after    # 读当前 sp_hero_t*.png（新，切片后跑）
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "mage-game", "assets")
PREV = os.path.join(ROOT, "assets", "preview")
NAMES = ["sp_hero_t0", "sp_hero_t1", "sp_hero_t2", "sp_hero_t3"]
LABELS = ["见习学徒", "魔法师", "大魔法师", "魔法帝"]
BIG = 4  # 放大倍数


def font(sz):
    for p in ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf"]:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, sz)
            except Exception:
                pass
    return ImageFont.load_default()


def montage(mode):
    os.makedirs(PREV, exist_ok=True)
    imgs = [Image.open(os.path.join(SRC, n + ".png")).convert("RGBA") for n in NAMES]
    # 放大预览行
    bw, bh = imgs[0].size
    cell_big = (bw * BIG, bh * BIG)
    pad = 14
    title_h = 34
    label_h = 26
    cols = len(imgs)
    W = pad * 2 + cols * cell_big[0] + (cols - 1) * pad
    # 上板实际尺寸行
    real_h = 30
    H = title_h + cell_big[1] + label_h + 18 + real_h + pad * 2
    canvas = Image.new("RGBA", (W, H), (0x0d, 0x0b, 0x1a, 255))
    d = ImageDraw.Draw(canvas)
    f_title = font(22)
    f_lbl = font(16)
    title = "魔法帝之路 · 主角四阶立绘（BEFORE 旧）" if mode == "before" \
        else "魔法帝之路 · 主角四阶立绘（AFTER 新·同一角色成长）"
    d.text((pad, 6), title, font=f_title, fill=(0xff, 0xff, 0xff, 255))
    y = title_h
    for i, im in enumerate(imgs):
        x = pad + i * (cell_big[0] + pad)
        big = im.resize(cell_big, Image.NEAREST)
        canvas.alpha_composite(big, (x, y))
        # label
        d.text((x + 2, y + cell_big[1] + 4), LABELS[i], font=f_lbl, fill=(0xff, 0xff, 0xff, 255))
        # 实际尺寸缩略（放右边说明区太小，这里直接贴 36x46 原样到标签下一行）
    # 实际尺寸行（紧挨放大行下方，居中每个 cell）
    ry = title_h + cell_big[1] + label_h + 10
    for i, im in enumerate(imgs):
        x = pad + i * (cell_big[0] + pad) + (cell_big[0] - bw) // 2
        # 画个浅框衬托透明
        d.rectangle([x - 1, ry - 1, x + bw + 1, ry + bh + 1], outline=(0x55, 0x55, 0x77, 255))
        canvas.alpha_composite(im, (x, ry))
    out = os.path.join(PREV, "hero_compare_%s.png" % mode)
    canvas.convert("RGB").save(out)
    print("saved", out, canvas.size)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "after"
    montage(mode)
