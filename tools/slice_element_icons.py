#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 AI 生成的 9 个魔法属性图标切出，存为 mage-game/assets/sp_icon_<elem>.png (44x44 透明底)。
复用 slice_ai_icons.clean_bg 做背景抠除（四角色逐像素），与 25 枚菜单图标同一处理管线。
之后跑 tools/gen_mage_sprites.py 重新烘焙进 assets/mage_sprites.c
"""
import os
from PIL import Image
import slice_ai_icons as S

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "generated-images")
DEST = os.path.join(ROOT, "mage-game", "assets")
SIZE = 44

MAP = [
    ("A_single_centered_medieval_fan_2026-10-28T11-53-23.png", "fire"),
    ("A_single_centered_medieval_fan_2026-10-28T11-53-38.png", "water"),
    ("A_single_centered_medieval_fan_2026-10-28T11-53-52.png", "thunder"),
    ("A_single_centered_medieval_fan_2026-10-28T11-54-07.png", "wind"),
    ("A_single_centered_medieval_fan_2026-10-28T12-02-31.png", "earth"),
    ("A_single_centered_medieval_fan_2026-10-28T11-54-30.png", "ice"),
    ("A_single_centered_medieval_fan_2026-10-28T11-54-45.png", "light"),
    ("A_single_centered_medieval_fan_2026-10-28T11-55-01.png", "dark"),
    ("A_single_centered_medieval_fan_2026-10-28T11-55-27.png", "void"),
]


def main():
    os.makedirs(DEST, exist_ok=True)
    for fn, out in MAP:
        im = Image.open(os.path.join(GEN, fn)).convert("RGBA")
        im = S.clean_bg(im)
        bbox = im.getbbox()
        if bbox:
            im = im.crop(bbox)
        w, h = im.size
        s = max(w, h)
        sq = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        sq.paste(im, ((s - w) // 2, (s - h) // 2))
        sq = sq.resize((SIZE, SIZE), Image.LANCZOS)
        sq.save(os.path.join(DEST, "sp_icon_%s.png" % out))
        print("saved", out)


if __name__ == "__main__":
    main()
