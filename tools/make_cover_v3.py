#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_cover_v3.py —— 上架封面 2.0：给像素场景加上标题，商店里一眼认出是钓鱼。

为什么重做：v2 封面只有像素场景、一个字都没有，在商店列表里就是一片蓝色，
看不出玩法。这一版保留"用游戏内真实像素素材"的做法（风格和实机完全一致），
补上三件东西：
  1. 标题「竿影浮标」——占住原本空着的上半天空；
  2. 副标「钓鱼图鉴」；
  3. 底部一行卖点（鱼种数 / 钓场数 / 图鉴收集）。

画面先在 240x320 拼像素，NEAREST 放大 4.8x 到 1152x1536，再在放大后的画布上
写文字 —— 画面保住像素网格，文字保持锐利，两者互不糊。

用法：
    python tools/make_cover_v3.py                # 晴日版 -> assets/cover/fishing-cover-v3-3x4.png
    python tools/make_cover_v3.py --warm          # 黄昏版 -> assets/cover/fishing-cover-v3-warm-3x4.png
    python tools/make_cover_v3.py --compare       # 另出一张 旧/晴日/黄昏 三栏对比图
"""
from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, "assets", "sprites", "png")
COVER_DIR = os.path.join(ROOT, "assets", "cover")

W, H = 240, 320          # 原生分辨率（正好 3:4）
SCALE = 4.8              # 240*4.8=1152, 320*4.8=1536（发布助手要求的封面尺寸）

TITLE = "竿影浮标"
SUBTITLE = "钓鱼图鉴"
TAGLINE = "24 种鱼 · 3 大钓场 · 收集图鉴"

# 场景在 240x320 上的落点。水线大约在 y=176（bg_pond 的天空/水面交界）。
ROD_COLOR = (92, 62, 36)
ROD_HILITE = (156, 116, 70)
LINE_COLOR = (238, 240, 232)
ROD_TIP = (84, 112)
FLOAT_TOP = (204, 184)


def load(name: str) -> Image.Image:
    return Image.open(os.path.join(PNG, name + ".png"))


def clean_alpha(im: Image.Image) -> Image.Image:
    """去掉主体之外的孤立像素（AI 出图偶尔留的杂点）。只保留最大连通块。"""
    a = im.getchannel("A")
    solid = a.point(lambda v: 255 if v > 96 else 0)
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
                clean: bool = False) -> None:
    im = load(name)
    if clean:
        im = clean_alpha(im)
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
    """画面左下角伸出（钓者在画外）的一根手竿 + 从竿尖垂到浮漂的鱼线。
    竿尖刻意压到 y=112：再往上就会顶进标题区。"""
    pts = bezier((-18, 318), (30, 206), ROD_TIP, 44)
    d.line(pts, fill=ROD_COLOR, width=3)
    d.line([(p[0] + 1, p[1]) for p in pts], fill=ROD_HILITE, width=1)

    prev = ROD_TIP
    for p in bezier(ROD_TIP, (152, 156), FLOAT_TOP, 36)[1:]:
        d.line([prev, p], fill=LINE_COLOR, width=1)
        prev = p


def build_scene() -> Image.Image:
    base = load("bg_pond").convert("RGBA")
    d = ImageDraw.Draw(base)

    draw_rod(d)

    # 浮漂（下半截入水）+ 落点涟漪
    paste_alpha(base, "prop_wave", 196, 202)
    paste_alpha(base, "prop_float", 204, 182)

    # 主角：一条正跃出水面的大鲤鱼（2 倍像素，做视觉焦点）
    paste_alpha(base, "fish_liyu", 68, 130, zoom=2, clean=True)

    # 出水水花：贴在水线上，刚好接住鱼的下缘
    paste_alpha(base, "prop_wave", 62, 192, zoom=2)
    paste_alpha(base, "prop_wave", 114, 198)

    # 水下远景鱼，让水下不空
    paste_alpha(base, "fish_jiyu", 22, 250)
    paste_alpha(base, "fish_luyu", 176, 260)
    return base


def find_font() -> str:
    for p in ("C:/Windows/Fonts/msyhbd.ttc", "C:/Windows/Fonts/msyh.ttc",
              "C:/Windows/Fonts/simhei.ttf",
              os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")):
        if os.path.exists(p):
            return p
    raise SystemExit("找不到可用的中文字体")


def centered(d: ImageDraw.ImageDraw, text: str, font, y: int, fill,
             stroke_w: int, stroke_fill, shadow=None, size=(0, 0)):
    """把一行字水平居中画在 y 上，返回这行字的下沿 y。"""
    bb = d.textbbox((0, 0), text, font=font, stroke_width=stroke_w)
    w = bb[2] - bb[0]
    x = (size[0] - w) // 2 - bb[0]
    if shadow:
        d.text((x + shadow[0], y + shadow[1]), text, font=font,
               fill=shadow[2], stroke_width=stroke_w, stroke_fill=shadow[2])
    d.text((x, y), text, font=font, fill=fill,
           stroke_width=stroke_w, stroke_fill=stroke_fill)
    return y + (bb[3] - bb[1])


def overlay_text(img: Image.Image, warm: bool) -> Image.Image:
    wp, hp = img.size
    fpath = find_font()

    f_title = ImageFont.truetype(fpath, int(wp * 0.198))
    f_sub = ImageFont.truetype(fpath, int(wp * 0.071))
    f_tag = ImageFont.truetype(fpath, int(wp * 0.047))

    if warm:
        ink, stroke, shadow = (255, 248, 226), (98, 42, 18), (52, 20, 8)
    else:
        ink, stroke, shadow = (255, 255, 255), (16, 60, 100), (8, 34, 60)

    # 底部卖点先垫一条半透明深色底，保证压在水草/浅水上都读得清
    ty = int(hp * 0.924)
    tmp = ImageDraw.Draw(img)
    bb = tmp.textbbox((0, 0), TAGLINE, font=f_tag, stroke_width=5)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    pad = 20
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(ov).rounded_rectangle(
        [(wp - tw) // 2 - pad, ty - pad // 2, (wp + tw) // 2 + pad, ty + th + pad // 2],
        radius=16, fill=(10, 30, 48, 135))
    img = Image.alpha_composite(img.convert("RGBA"), ov).convert("RGB")
    d = ImageDraw.Draw(img)

    y = int(hp * 0.072)
    y = centered(d, TITLE, f_title, y, ink, 8, stroke, shadow=(7, 9, shadow), size=(wp, hp))
    y = centered(d, SUBTITLE, f_sub, y + 16, (255, 246, 222), 4, stroke,
                 shadow=(4, 5, shadow), size=(wp, hp))
    centered(d, TAGLINE, f_tag, ty, (255, 255, 255), 5, stroke,
             shadow=(3, 4, shadow), size=(wp, hp))
    return img


def warm_filter(img: Image.Image) -> Image.Image:
    """黄昏色调：只把天空染成暖金（不整幅压暗，云和鱼的颜色都保住），
    再在右上角加一点夕阳逆光光晕。只染天空是因为水面染暖会显得浑浊。"""
    wp, hp = img.size
    out = img.copy()
    sky_h = int(hp * 0.55)
    strip = out.crop((0, 0, wp, sky_h)).convert("RGB")
    strip = Image.blend(strip, Image.new("RGB", strip.size, (255, 176, 96)), 0.42)
    # 越靠上越暖，做出"太阳在远方"的层次
    grad = Image.new("RGBA", strip.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for yy in range(sky_h):
        a = int(70 * (1 - yy / sky_h) ** 2)
        gd.line([(0, yy), (wp, yy)], fill=(255, 150, 60, a))
    strip = Image.alpha_composite(strip.convert("RGBA"), grad).convert("RGB")
    out.paste(strip, (0, 0))

    glow = Image.new("L", out.size, 0)
    gd2 = ImageDraw.Draw(glow)
    cx, cy, r = int(wp * 0.82), int(hp * 0.13), int(wp * 0.40)
    for i in range(30):
        rr = int(r * (1 - i / 30.0))
        gd2.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=int(26 * (i / 30.0) ** 2))
    return Image.composite(Image.new("RGB", out.size, (255, 240, 200)), out, glow)


def make(warm: bool = False, y_offset: int = 0) -> Image.Image:
    scene = build_scene().convert("RGB").resize(
        (int(W * SCALE), int(H * SCALE)), Image.NEAREST)
    if warm:
        scene = warm_filter(scene)
    if y_offset:
        d = ImageDraw.Draw(scene)
        d = d  # 预留：整幅微调
    return overlay_text(scene, warm)


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
    fpath = find_font()
    fl = ImageFont.truetype(fpath, 26)
    x = pad
    for label, im in ims:
        sheet.paste(im, (x, top))
        sd.rectangle([x - 1, top - 1, x + im.width, top + im.height], outline=(196, 200, 210))
        sd.text((x + 2, top - 36), label, font=fl, fill=(30, 36, 48))
        x += im.width + gap
    sheet.save(out_path)
    print("saved", out_path, sheet.size)


def main() -> int:
    warm = "--warm" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else os.path.join(
        COVER_DIR, "fishing-cover-v3-warm-3x4.png" if warm else "fishing-cover-v3-3x4.png")
    os.makedirs(COVER_DIR, exist_ok=True)

    img = make(warm)
    img.save(out)
    w, h = img.size
    print("saved %s  %dx%d  is3x4=%s" % (out, w, h, w * 4 == h * 3))

    if "--compare" in sys.argv:
        old = os.path.join(ROOT, "assets", "publish", "v2", "fishing-cover-3x4.png")
        a = os.path.join(COVER_DIR, "fishing-cover-v3-3x4.png")
        b = os.path.join(COVER_DIR, "fishing-cover-v3-warm-3x4.png")
        if not (os.path.exists(a) and os.path.exists(b)):
            print("对比图需要先分别生成 晴日/黄昏 两版")
            return 1
        make_compare([("旧封面", old), ("新 · 晴日", a), ("新 · 黄昏", b)],
                     os.path.join(COVER_DIR, "fishing-cover-compare.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
