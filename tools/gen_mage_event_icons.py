#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成探索事件面板图标（宝箱/陷阱/泉水/药草），1:1 对应 HTML drawEventPanel 的图形。

输出到 mage-game/assets/sp_event_*.png（56x56 透明 RGBA），由 gen_mage_sprites.py
自动发现并烘进 mage_sprites.c（RGB565A8 真透明）。
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "mage-game", "assets")
S = 56


def new_img():
    return Image.new("RGBA", (S, S), (0, 0, 0, 0))


def chest():
    im = new_img()
    d = ImageDraw.Draw(im)
    # 箱体
    d.rectangle([10, 22, 46, 50], fill=(0x7a, 0x4a, 0x22, 255))
    # 顶盖
    d.rectangle([10, 22, 46, 28], fill=(0x9a, 0x66, 0x36, 255))
    # 金边带
    d.rectangle([10, 34, 46, 37], fill=(0xff, 0xd9, 0x3d, 255))
    # 锁孔
    d.rectangle([26, 38, 30, 44], fill=(0x3a, 0x24, 0x10, 255))
    # 微光
    d.ellipse([25, 9, 31, 15], fill=(0xff, 0xf3, 0xb0, 255))
    return im


def trap():
    im = new_img()
    d = ImageDraw.Draw(im)
    # 底座
    d.rectangle([8, 46, 48, 52], fill=(0x5a, 0x4a, 0x3a, 255))
    # 三根尖刺
    gray = (0xb8, 0xc4, 0xd4, 255)
    for cx in (20, 30, 40):
        d.polygon([(cx - 6, 46), (cx, 30), (cx + 6, 46)], fill=gray)
    return im


def spring():
    im = new_img()
    d = ImageDraw.Draw(im)
    cx, cy = 28, 38
    d.ellipse([cx - 13, cy - 13, cx + 13, cy + 13], fill=(0x3a, 0x7a, 0xc9, 255))
    d.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], fill=(0x6f, 0xc3, 0xf2, 255))
    # 涟漪
    d.ellipse([cx - 6, cy - 3, cx + 6, cy + 3], outline=(0xdf, 0xf6, 0xff, 220), width=1)
    return im


def herb():
    im = new_img()
    d = ImageDraw.Draw(im)
    # 茎
    d.rectangle([27, 16, 29, 46], fill=(0x3f, 0x8f, 0x3a, 255))
    # 左右叶
    leaf = (0x6f, 0xcf, 0x6f, 255)
    d.polygon([(29, 20), (14, 26), (29, 30)], fill=leaf)
    d.polygon([(29, 28), (44, 32), (29, 38)], fill=leaf)
    # 浆果
    for (bx, by) in [(22, 40), (34, 42), (30, 47)]:
        d.ellipse([bx - 2, by - 2, bx + 2, by + 2], fill=(0x8f, 0xe8, 0x8f, 255))
    return im


def main():
    jobs = {
        "sp_event_chest": chest,
        "sp_event_trap": trap,
        "sp_event_spring": spring,
        "sp_event_herb": herb,
    }
    for name, fn in jobs.items():
        im = fn()
        path = os.path.join(OUT, name + ".png")
        im.save(path)
        print("生成", name + ".png", im.size)
    print("完成：", len(jobs), "张事件图标 ->", OUT)


if __name__ == "__main__":
    main()
