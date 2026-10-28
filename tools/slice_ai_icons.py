#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 AI 生成的 25 个图标切出，存为 mage-game/assets/sp_icon_<name>.png (44x44 透明底)。

源图：generated-images/ 下 5 张 3x2 网格样张 + 5 张干净单图（去水印版）。
输出：mage-game/assets/sp_icon_<name>.png
之后跑 tools/gen_mage_sprites.py 重新烘焙进 assets/mage_sprites.c
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.path.join(ROOT, "generated-images")
DEST = os.path.join(ROOT, "mage-game", "assets")
SIZE = 44

SHEETS = {
    "A": "A_3_by_2_grid_of_six_medieval__2026-10-27T09-39-50.png",
    "B": "A_3_by_2_grid_of_six_medieval__2026-10-28T00-11-02.png",
    "C": "IMPORTANT_layout_rule__strictl_2026-10-28T00-15-53.png",
    "D": "IMPORTANT_layout_rule__strictl_2026-10-28T00-16-06.png",
    "E": "Two_medieval_fantasy_RPG_game__2026-10-28T00-11-45.png",
}
# (sheet, col, row, 中文名)
GRID = [
    ("A", 0, 0, "背包"), ("B", 0, 0, "休息"), ("B", 1, 0, "魔力"), ("B", 2, 0, "兽骨"), ("A", 2, 1, "魔法书"),
    ("B", 0, 1, "金币"), ("B", 1, 1, "晋升"), ("B", 2, 1, "魔晶"), ("C", 0, 0, "炼金"), ("C", 1, 0, "出征"),
    ("C", 2, 0, "设置"), ("C", 0, 1, "锻造"), ("C", 1, 1, "生命"), ("C", 2, 1, "月光草"), ("D", 0, 0, "地图"),
    ("D", 1, 0, "法球"), ("D", 2, 0, "伙伴"), ("D", 0, 1, "长袍"), ("D", 1, 1, "卷轴"), ("D", 2, 1, "封印"),
    ("E", 1, 0, "护甲"), ("A", 2, 0, "法杖"), ("A", 1, 0, "攻击"), ("A", 0, 1, "药水"), ("E", 0, 0, "资质"),
]
SP = {
    "背包": "bag", "休息": "bed", "魔力": "bolt", "兽骨": "bone", "魔法书": "book", "金币": "coin",
    "晋升": "crown", "魔晶": "crystal", "炼金": "flask", "出征": "gate", "设置": "gear", "锻造": "hammer",
    "生命": "heart", "月光草": "herb", "地图": "map", "法球": "orb", "伙伴": "paw", "长袍": "robe",
    "卷轴": "scroll", "封印": "seal", "护甲": "shield", "法杖": "staff", "攻击": "sword", "药水": "potion", "资质": "star",
}
OVERRIDE = {
    "魔法书": "A_single_centered_game_UI_icon_2026-10-28T00-19-22.png",
    "魔晶": "A_single_centered_game_UI_icon_2026-10-28T00-19-34.png",
    "月光草": "A_single_centered_game_UI_icon_2026-10-28T00-19-46.png",
    "封印": "A_single_centered_game_UI_icon_2026-10-28T00-20-00.png",
    "护甲": "A_single_centered_game_UI_icon_2026-10-28T00-20-12.png",
}
_cache = {}


def sheet_img(n):
    if n not in _cache:
        _cache[n] = Image.open(os.path.join(BASE, SHEETS[n])).convert("RGB")
    return _cache[n]


def cell(n, col, row):
    im = sheet_img(n)
    W, H = im.size
    cw, ch = (W // 2, H) if n == "E" else (W // 3, H // 2)
    return im.crop((col * cw, row * ch, col * cw + cw, row * ch + ch))


def is_navy(r, g, b):
    return r < 62 and g < 72 and b < 104 and abs(r - g) < 26


def extract(region):
    W, H = region.size
    px = region.load()
    minx, miny, maxx, maxy = W, H, 0, 0
    f = False
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            if not is_navy(r, g, b):
                if not f:
                    minx = maxx = x
                    miny = maxy = y
                    f = True
                else:
                    minx = min(minx, x)
                    maxx = max(maxx, x)
                    miny = min(miny, y)
                    maxy = max(maxy, y)
    if f and (maxx - minx) > 20 and (maxy - miny) > 20:
        i = 4
        return region.crop((max(0, minx - i), max(0, miny - i),
                             min(W, maxx + i), min(H, maxy + i)))
    return region


def get(name, sheet, col, row):
    if name in OVERRIDE:
        im = Image.open(os.path.join(BASE, OVERRIDE[name])).convert("RGBA")
        W, H = im.size
        m = int(min(W, H) * 0.12)
        im = im.crop((m, m, W - m, H - m))
    else:
        im = extract(cell(sheet, col, row)).convert("RGBA")
    # 去深蓝面板底 -> 透明（图标本身深色描边，浅色内部保留）
    px = im.load()
    W, H = im.size
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            if is_navy(r, g, b):
                px[x, y] = (r, g, b, 0)
    bbox = im.getbbox()
    if bbox:
        im = im.crop(bbox)
    w, h = im.size
    s = max(w, h)
    sq = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sq.paste(im, ((s - w) // 2, (s - h) // 2))
    return sq.resize((SIZE, SIZE), Image.LANCZOS)


if __name__ == "__main__":
    os.makedirs(DEST, exist_ok=True)
    for (sheet, col, row, name) in GRID:
        sp = SP[name]
        icon = get(name, sheet, col, row)
        out = os.path.join(DEST, "sp_icon_%s.png" % sp)
        icon.save(out)
        print("saved", out, icon.size)
    print("DONE 25 icons")
