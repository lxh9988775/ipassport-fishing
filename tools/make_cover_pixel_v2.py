#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_cover_pixel_v2.py —— 用游戏内真实像素素材拼一版 3:4 上架封面。

思路：先在 240x320（正好 3:4）的"游戏原生分辨率"画布上拼好画面，
再用 NEAREST 整数倍放大到 1152x1536，保证像素网格干净、不糊边。

素材全部来自 assets/sprites/png/（= 固件里真正跑的同一批图），
所以封面和实机画面风格完全一致，不存在"图不对版"。

用法：
    python tools/make_cover_pixel_v2.py [输出路径]
"""
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, "assets", "sprites", "png")
OUT_DEFAULT = os.path.join(ROOT, "assets", "cover", "fishing-cover-v2-pixel-3x4.png")

W, H = 240, 320          # 原生分辨率（3:4）
SCALE = 4.8              # 240*4.8=1152, 320*4.8=1536

ROD_COLOR = (92, 62, 36)
LINE_COLOR = (238, 240, 232)


def load(name):
    return Image.open(os.path.join(PNG, name + ".png"))


def clean_alpha(im, min_px=3, keep_largest=False):
    """去掉主体之外的孤立像素（AI 出图偶尔留的杂点/触须残渣）。"""
    a = im.getchannel("A")
    solid = a.point(lambda v: 255 if v > 96 else 0)
    keep = Image.new("L", im.size, 0)
    comps = []
    seen = set()
    px = solid.load()
    kp = keep.load()
    for y in range(im.height):
        for x in range(im.width):
            if px[x, y] == 0 or (x, y) in seen:
                continue
            stack = [(x, y)]
            seen.add((x, y))
            comp = []
            while stack:
                cx, cy = stack.pop()
                comp.append((cx, cy))
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < im.width and 0 <= ny < im.height \
                                and px[nx, ny] and (nx, ny) not in seen:
                            seen.add((nx, ny))
                            stack.append((nx, ny))
            comps.append(comp)
    if comps:
        keep_comps = [max(comps, key=len)] if keep_largest else                      [c for c in comps if len(c) >= min_px]
        for comp in keep_comps:
            for cx, cy in comp:
                kp[cx, cy] = 255
    r, g, b, _ = im.split()
    return Image.merge("RGBA", (r, g, b, keep))


def paste_alpha(base, name, x, y, zoom=1, clean=False):
    """把 RGBA sprite 按其 alpha 贴到 base 的 (x, y)（左上角）。zoom 为整数倍放大。"""
    im = load(name)
    if clean:
        im = clean_alpha(im, keep_largest=True)
    if zoom != 1:
        im = im.resize((im.width * zoom, im.height * zoom), Image.NEAREST)
    base.paste(im, (x, y), im)


def bezier(p0, p1, p2, steps):
    pts = []
    for i in range(steps + 1):
        t = i / steps
        x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t ** 2 * p2[0]
        y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t ** 2 * p2[1]
        pts.append((x, y))
    return pts


def draw_rod(d):
    """画面左下角伸出（钓者在画外）的一根手竿 + 从竿尖垂到浮漂的鱼线。"""
    tip = (72, 74)
    butt = (-18, 318)
    # 竿身：中间略弯一点，比直线自然
    pts = bezier(butt, (28, 196), tip, 40)
    d.line(pts, fill=ROD_COLOR, width=3)
    d.line([(p[0] + 1, p[1]) for p in pts], fill=(150, 110, 66), width=1)

    # 鱼线：从竿尖垂到浮漂，带自然弧垂
    float_top = (198, 190)
    prev = tip
    for p in bezier(tip, (140, 158), float_top, 34)[1:]:
        d.line([prev, p], fill=LINE_COLOR, width=1)
        prev = p


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT

    base = load("bg_pond").convert("RGBA")
    d = ImageDraw.Draw(base)

    draw_rod(d)

    # 浮漂（下半截入水）+ 落点涟漪
    paste_alpha(base, "prop_wave", 184, 202)
    paste_alpha(base, "prop_float", 192, 186)

    # 主角：一条正跃出水面（2 倍像素，做视觉焦点）
    paste_alpha(base, "fish_liyu", 74, 136, zoom=2, clean=True)

    # 出水的水花：贴在水线上，刚好接住鱼的下缘
    paste_alpha(base, "prop_wave", 68, 200, zoom=2)
    paste_alpha(base, "prop_wave", 118, 206)

    # 水下的远景鱼，让水下不空
    paste_alpha(base, "fish_jiyu", 20, 252)
    paste_alpha(base, "fish_luyu", 178, 262)

    final = base.convert("RGB").resize(
        (int(W * SCALE), int(H * SCALE)), Image.NEAREST
    )
    final.save(out_path)
    w, h = final.size
    print("saved %s  %dx%d  is3x4=%s" % (out_path, w, h, w * 4 == h * 3))


if __name__ == "__main__":
    main()
