#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""切片 4 套人物等级立绘 -> mage-game/assets/sp_hero_t0..t3.png (36x46 透明底，
对齐现有 sp_hero 的显示尺寸，gen_mage_sprites 的 discover_monsters 会自动烘焙)。

等级映射（与 hero_tiers 概念一致，对齐 RANKS 职级）：
  t0 见习学徒   rank 0-1
  t1 魔法师     rank 2-3
  t2 大魔法师   rank 4-5
  t3 魔法帝     rank 6
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.path.join(ROOT, "generated-images")
DEST = os.path.join(ROOT, "mage-game", "assets")
SIZE = (36, 46)

HERO = [
    ("sp_hero_t0", "A_single_centered_full_body_ch_2026-10-28T01-17-19.png"),  # 见习学徒
    ("sp_hero_t1", "A_single_centered_full_body_ch_2026-10-28T01-18-25.png"),  # 魔法师
    ("sp_hero_t2", "A_single_centered_full_body_ch_2026-10-28T01-18-41.png"),  # 大魔法师
    ("sp_hero_t3", "A_single_centered_full_body_ch_2026-10-28T01-18-50.png"),  # 魔法帝
]


def main():
    os.makedirs(DEST, exist_ok=True)
    for name, src in HERO:
        im = Image.open(os.path.join(BASE, src)).convert("RGBA")
        w, h = im.size
        scale = min(SIZE[0] / w, SIZE[1] / h)
        tw, th = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
        im = im.resize((tw, th), Image.LANCZOS)
        canvas = Image.new("RGBA", SIZE, (0, 0, 0, 0))
        canvas.paste(im, ((SIZE[0] - tw) // 2, (SIZE[1] - th) // 2), im)
        out = os.path.join(DEST, name + ".png")
        canvas.save(out)
        print("saved", out, canvas.size)
    print("DONE 4 hero sprites")


if __name__ == "__main__":
    main()
