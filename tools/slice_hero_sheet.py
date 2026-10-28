#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「同一角色四阶成长表」横图切成 4 张 36x46 透明切片（sp_hero_t0..t3）。
做法：按 1/4 宽切列 -> 每列取不透明像素紧致 bbox -> 等比缩放进 36x46 居中。
这样即使生成图里某阶角色偏大/偏侧，上板尺寸也统一、主角居中。
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEET = os.path.join(ROOT, "generated-images",
                     "Retro_16_bit_JRPG_pixel_art_ch_2026-10-28T06-18-15.png")
DEST = os.path.join(ROOT, "mage-game", "assets")
SIZE = (36, 46)
RANKS = ["sp_hero_t0", "sp_hero_t1", "sp_hero_t2", "sp_hero_t3"]  # 见习学徒/魔法师/大魔法师/魔法帝


def tight_crop(col):
    """对单列 RGBA 图取不透明 bbox，返回紧致裁剪图（去透明边）。"""
    alpha = col.split()[-1]
    bbox = alpha.getbbox()
    if bbox is None:
        return col
    return col.crop(bbox)


def fit_center(im, size):
    w, h = im.size
    scale = min(size[0] / w, size[1] / h)
    tw, th = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    im = im.resize((tw, th), Image.LANCZOS)
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    canvas.paste(im, ((size[0] - tw) // 2, (size[1] - th) // 2), im)
    return canvas


def main():
    os.makedirs(DEST, exist_ok=True)
    sheet = Image.open(SHEET).convert("RGBA")
    W, H = sheet.size
    col_w = W // 4
    for i, name in enumerate(RANKS):
        col = sheet.crop((i * col_w, 0, (i + 1) * col_w, H))
        col = tight_crop(col)
        out = fit_center(col, SIZE)
        path = os.path.join(DEST, name + ".png")
        out.save(path)
        print("saved", path, "src_crop", col.size, "->", SIZE)
    print("DONE 4 hero sprites from sheet")


if __name__ == "__main__":
    main()
