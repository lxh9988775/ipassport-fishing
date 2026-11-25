#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_cover_v4.py —— 上架封面 3.0：主角特写 + 钩子副标 + 浓对比深水。

依据《竿影浮标 v4 更新执行任务书》第一部分。只学社区头部封面的设计语言
（主角特写占中心 / 标题压上三分之一 / 钩子短句 / 浓对比），素材 100% 用
本项目 assets/sprites/png/ 自有像素，不抠图、不描图、不 AI 重画。

与 v3 的差别（针对 v3 的三点不足）：
  1. v3 鱼是散摆的 —— v4 用一条大个体鳜鱼（金黄、带鳍刺、咬钩姿态）居中做主角；
  2. v3 副标是陈述句「钓鱼图鉴」 —— v4 换钩子「每条鱼，都记得你」；
  3. v3 通篇浅蓝偏平 —— v4 把天空压成深蓝、水面压成墨蓝，主角与浮漂高光跳出。

先在 240x320 原生像素拼场景，NEAREST 放大 4.8x 到 1152x1536，再在放大画布上
写文字：画面保像素网格，文字保持锐利。封面文字走独立字体渲染（PIL TTF），
不依赖板载字库；板载字库只覆盖固件内 UI 文案，封面不进固件。

用法：
    python tools/make_cover_v4.py                # -> assets/cover/fishing-cover-v4-3x4.png
    python tools/make_cover_v4.py --compare      # 另出 旧/v3/v4 三栏对比图
    python tools/make_cover_v4.py --hero qiyu    # 主角换旗鱼（备选对比用）
"""
from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, "assets", "sprites", "png")
COVER_DIR = os.path.join(ROOT, "assets", "cover")
PUBLISH_DIR = os.path.join(ROOT, "assets", "publish")

W, H = 240, 320
SCALE = 4.8                      # 1152 x 1536

TITLE = "竿影浮标"
HOOK = "每条鱼，都记得你"          # 首选钩子，呼应③钓鱼履历
TAGLINE = "27 种鱼 · 5 大钓场 · 10 处标点"

HERO_DEFAULT = "fish_guiyu"      # 鳜鱼：金黄带刺、放大辨识度最高
HERO_ALT = {"qiyu": "fish_qiyu", "qingyu": "fish_qingyu"}

ROD_COLOR = (74, 48, 26)
ROD_HILITE = (150, 108, 58)
LINE_COLOR = (236, 240, 226)
ROD_TIP = (60, 118)
FLOAT_TOP = (206, 178)


def load(name: str) -> Image.Image:
    return Image.open(os.path.join(PNG, name + ".png")).convert("RGBA")


def clean_alpha(im: Image.Image) -> Image.Image:
    """只保留最大连通块，去掉 AI 出图偶尔留下的孤立杂点。"""
    solid = im.getchannel("A").point(lambda v: 255 if v > 96 else 0)
    px = solid.load()
    seen: set = set()
    comps = []
    for y in range(im.height):
        for x in range(im.width):
            if px[x, y] == 0 or (x, y) in seen:
                continue
            stack, comp = [(x, y)], []
            seen.add((x, y))
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
    keep = Image.new("L", im.size, 0)
    if comps:
        kp = keep.load()
        for cx, cy in max(comps, key=len):
            kp[cx, cy] = 255
    r, g, b, _ = im.split()
    return Image.merge("RGBA", (r, g, b, keep))


def paste_alpha(base: Image.Image, name: str, x: int, y: int, zoom: int = 1,
                flip: bool = False, clean: bool = False) -> None:
    im = load(name)
    if clean:
        im = clean_alpha(im)
    if flip:
        im = im.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    if zoom != 1:
        im = im.resize((im.width * zoom, im.height * zoom), Image.NEAREST)
    base.paste(im, (x, y), im)


def bezier(p0, p1, p2, steps):
    pts = []
    for i in range(steps + 1):
        t = i / steps
        pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t ** 2 * p2[0],
                    (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t ** 2 * p2[1]))
    return pts


def draw_rod(d: ImageDraw.ImageDraw) -> None:
    pts = bezier((-16, 322), (26, 210), ROD_TIP, 44)
    d.line(pts, fill=ROD_COLOR, width=3)
    d.line([(p[0] + 1, p[1]) for p in pts], fill=ROD_HILITE, width=1)
    prev = ROD_TIP
    for p in bezier(ROD_TIP, (150, 154), FLOAT_TOP, 36)[1:]:
        d.line([prev, p], fill=LINE_COLOR, width=1)
        prev = p


def deepen(base: Image.Image) -> Image.Image:
    """把浅蓝白天压成深蓝天空 + 墨蓝水面，做出浓对比氛围底。
    逐像素映射，不插值，保住像素块感。"""
    px = base.load()
    for y in range(H):
        # 天空(约 y<176) 压深蓝，越往上越深；水面(y>=176) 压墨蓝，越往下越深
        for x in range(W):
            r, g, b = px[x, y][:3]
            if y < 176:
                t = y / 176.0                      # 0 顶 -> 1 水线
                tr, tg, tb = (10 + 8 * t, 42 + 36 * t, 82 + 40 * t)
                k = 0.72 - 0.18 * t                # 顶部压得更狠
                nr = int(r * (1 - k) + tr * k)
                ng = int(g * (1 - k) + tg * k)
                nb = int(b * (1 - k) + tb * k)
            else:
                t = (y - 176) / (H - 176.0)
                tr, tg, tb = (8 - 3 * t, 52 - 24 * t, 84 - 32 * t)
                k = 0.58 + 0.22 * t
                nr = int(r * (1 - k) + tr * k)
                ng = int(g * (1 - k) + tg * k)
                nb = int(b * (1 - k) + tb * k)
            px[x, y] = (max(0, min(255, nr)), max(0, min(255, ng)),
                        max(0, min(255, nb)))
    return base


def build_scene(hero: str) -> Image.Image:
    base = load("bg_pond").convert("RGB")
    base = deepen(base)
    d = ImageDraw.Draw(base)

    draw_rod(d)

    # 浮漂（红白高光，右下入水）+ 落点涟漪
    paste_alpha(base, "prop_wave", 194, 204)
    paste_alpha(base, "prop_float", 206, 180)

    # 主角鱼：3 倍像素居中偏上，水平翻转成头朝右上（咬钩姿态），下缘压出水花
    paste_alpha(base, hero, 78, 118, zoom=3, flip=True, clean=True)
    paste_alpha(base, "prop_wave", 74, 196, zoom=2)
    paste_alpha(base, "prop_wave", 128, 202)

    # 水下远景两条陪衬鱼，压暗退后，不抢主角
    for nm, x, y in (("fish_jiyu", 20, 252), ("fish_bianyu", 176, 262)):
        im = load(nm)
        r, g, b, a = im.split()
        im = Image.merge("RGBA", (r.point(lambda v: v * 2 // 3),
                                  g.point(lambda v: v * 2 // 3),
                                  b.point(lambda v: v * 2 // 3), a))
        base.paste(im, (x, y), im)
    return base


def find_font(bold: bool = True) -> str:
    cands = (["C:/Windows/Fonts/msyhbd.ttc"] if bold else []) + [
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
        os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")]
    for p in cands:
        if os.path.exists(p):
            return p
    raise SystemExit("找不到可用的中文字体")


def centered(d, text, font, y, fill, stroke_w, stroke_fill, shadow=None, size=(0, 0)):
    bb = d.textbbox((0, 0), text, font=font, stroke_width=stroke_w)
    tw = bb[2] - bb[0]
    x = (size[0] - tw) // 2 - bb[0]
    if shadow:
        d.text((x + shadow[0], y + shadow[1]), text, font=font,
               fill=shadow[2], stroke_width=stroke_w, stroke_fill=shadow[2])
    d.text((x, y), text, font=font, fill=fill,
           stroke_width=stroke_w, stroke_fill=stroke_fill)
    return y + (bb[3] - bb[1])


def overlay_text(img: Image.Image) -> Image.Image:
    wp, hp = img.size
    f_title = ImageFont.truetype(find_font(True), int(wp * 0.205))
    f_hook = ImageFont.truetype(find_font(True), int(wp * 0.062))
    f_tag = ImageFont.truetype(find_font(True), int(wp * 0.046))

    ink, stroke, shadow = (255, 252, 236), (10, 34, 62), (3, 14, 30)
    hook_ink = (255, 214, 120)

    # 底部卖点条：先垫半透明深色底，压在墨蓝水面上也读得清
    ty = int(hp * 0.925)
    tmp = ImageDraw.Draw(img)
    bb = tmp.textbbox((0, 0), TAGLINE, font=f_tag, stroke_width=5)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    pad = 22
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(ov).rounded_rectangle(
        [(wp - tw) // 2 - pad, ty - pad // 2, (wp + tw) // 2 + pad, ty + th + pad // 2],
        radius=16, fill=(6, 22, 40, 165))
    img = Image.alpha_composite(img.convert("RGBA"), ov).convert("RGB")
    d = ImageDraw.Draw(img)

    y = int(hp * 0.055)
    y = centered(d, TITLE, f_title, y, ink, 9, stroke, shadow=(8, 10, shadow), size=(wp, hp))
    y = centered(d, HOOK, f_hook, y + 20, hook_ink, 5, (72, 30, 12),
                 shadow=(4, 6, shadow), size=(wp, hp))
    centered(d, TAGLINE, f_tag, ty, (255, 255, 250), 5, stroke,
             shadow=(3, 4, shadow), size=(wp, hp))
    return img


def make(hero: str = HERO_DEFAULT) -> Image.Image:
    scene = build_scene(hero).convert("RGB").resize(
        (int(W * SCALE), int(H * SCALE)), Image.NEAREST)
    return overlay_text(scene)


def make_compare(paths, out_path) -> None:
    Z = 0.30
    ims = []
    for label, p in paths:
        im = Image.open(p).convert("RGB")
        im = im.resize((int(im.width * Z), int(im.height * Z)), Image.LANCZOS)
        ims.append((label, im))
    gap, top, pad = 26, 54, 24
    w = pad * 2 + sum(im.width for _, im in ims) + gap * (len(ims) - 1)
    h = top + max(im.height for _, im in ims) + pad
    sheet = Image.new("RGB", (w, h), (238, 240, 245))
    sd = ImageDraw.Draw(sheet)
    fl = ImageFont.truetype(find_font(True), 26)
    x = pad
    for label, im in ims:
        sheet.paste(im, (x, top))
        sd.rectangle([x - 1, top - 1, x + im.width, top + im.height], outline=(196, 200, 210))
        sd.text((x + 2, top - 36), label, font=fl, fill=(30, 36, 48))
        x += im.width + gap
    sheet.save(out_path)
    print("saved", out_path, sheet.size)


def main() -> int:
    os.makedirs(COVER_DIR, exist_ok=True)
    hero = HERO_DEFAULT
    if "--hero" in sys.argv:
        hero = HERO_ALT.get(sys.argv[sys.argv.index("--hero") + 1], HERO_DEFAULT)

    out = os.path.join(COVER_DIR, "fishing-cover-v5-3x4.png")
    img = make(hero)
    img.save(out)
    w, h = img.size
    print("saved %s  %dx%d  is3x4=%s" % (out, w, h, w * 4 == h * 3))

    if "--compare" in sys.argv:
        old = os.path.join(PUBLISH_DIR, "fishing-cover-3x4.png")
        v3 = os.path.join(COVER_DIR, "fishing-cover-v3-3x4.png")
        v4 = os.path.join(COVER_DIR, "fishing-cover-v4-3x4.png")
        v5 = os.path.join(COVER_DIR, "fishing-cover-v5-3x4.png")
        make_compare([("v4(24种/3场)", v4), ("v5(27种/5场/10标点)", v5)],
                     os.path.join(COVER_DIR, "fishing-cover-v5-compare.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
