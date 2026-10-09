#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_extra_shots.py —— 生成两张 3:4 的玩法配图（用游戏内真实像素素材，非 AI 生图）。

产出：
  assets/publish/v2/fishing-shot-codex-3x4.png   24 种鱼图鉴墙
  assets/publish/v2/fishing-shot-scenes-3x4.png  三片钓场（静水塘 / 急流河 / 深海）

两张图都会在右下角打上"游戏素材示意"，避免被当成实机截图。
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, "assets", "sprites", "png")
OUT = os.path.join(ROOT, "assets", "publish", "v2")
FONT_PATH = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")

W, H = 1152, 1536

FISH_ORDER = [
    "baitiao", "jiyu", "luofei", "lianyu", "bianyu", "caoyu", "liyu", "qingyu",
    "makou", "huangsang", "qiaozui", "luyu", "guiyu", "nianyu", "junyu", "ganyu",
    "xiaohuang", "daiyu", "bayu", "shiban", "bimu", "jinqiang", "qiyu", "xiaosha",
]
FISH_ZH = {
    "baitiao": "白条", "jiyu": "鲫鱼", "luofei": "罗非鱼", "lianyu": "鲢鱼",
    "bianyu": "鳊鱼", "caoyu": "草鱼", "liyu": "鲤鱼", "qingyu": "青鱼",
    "makou": "马口鱼", "huangsang": "黄颡鱼", "qiaozui": "翘嘴", "luyu": "鲈鱼",
    "guiyu": "鳜鱼", "nianyu": "鲶鱼", "junyu": "军鱼", "ganyu": "鳡鱼",
    "xiaohuang": "小黄鱼", "daiyu": "带鱼", "bayu": "鲅鱼", "shiban": "石斑鱼",
    "bimu": "比目鱼", "jinqiang": "金枪鱼", "qiyu": "旗鱼", "xiaosha": "小鲨鱼",
}
# (素材, 名称, 纵向裁切位置 0=顶 1=底, 鱼竿, 鱼饵)
# 裁切位置要让"竿尖→浮漂→鱼饵"整套家什都落在画格里（竿在 y116~，饵到 y207），
# 所以三片都取中段，不再取贴顶/贴底。
SCENES = [
    ("bg_pond", "静水塘", 0.49, "rod_hand", "bait_worm"),
    ("bg_river", "急流河", 0.49, "rod_lure", "bait_dough"),
    ("bg_sea", "深海", 0.49, "rod_sea", "bait_spinner"),
]

# 与 main/fishing.c 的 ROD_X/ROD_Y/FLOAT_*/BAIT_DY 一致
ROD_X, ROD_Y = 8, 116
FLOAT_X, FLOAT_Y, FLOAT_W = 150, 146, 24
BAIT_DY = 29

INK = (14, 26, 44)
PAPER = (233, 240, 250)
ACCENT = (153, 0, 58)          # 锐胜红
NOTE = "游戏素材示意 · In-game art"


def font(size):
    return ImageFont.truetype(FONT_PATH, size)


def paste(base, name, x, y, zoom=1):
    im = Image.open(os.path.join(PNG, name + ".png")).convert("RGBA")
    if zoom != 1:
        im = im.resize((im.width * zoom, im.height * zoom), Image.NEAREST)
    base.paste(im, (x, y), im)


def corner_note(d):
    # 放左下角：右下角会被浮漂/鱼饵压住
    f = font(22)
    d.text((28, H - 40), NOTE, font=f, fill=(150, 168, 192))


def make_codex():
    img = Image.new("RGBA", (W, H), INK + (255,))
    d = ImageDraw.Draw(img)

    # 顶部标题带
    d.rectangle([0, 0, W, 12], fill=ACCENT + (255,))
    d.text((56, 52), "图鉴 · 24 种鱼", font=font(52), fill=PAPER + (255,))
    d.text((56, 122), "没钓到的只留剪影，钓上来才解锁", font=font(26),
           fill=(140, 162, 190, 255))

    cols, rows = 4, 6
    top, left = 196, 48
    cell_w = (W - left * 2) // cols
    cell_h = (H - top - 96) // rows
    z = 5
    fw, fh = 48 * z, 32 * z
    for i, key in enumerate(FISH_ORDER):
        r, c = divmod(i, cols)
        cx = left + c * cell_w + (cell_w - fw) // 2
        cy = top + r * cell_h + (cell_h - fh) // 2 - 12
        paste(img, "fish_%s" % key, cx, cy, zoom=z)
        label = FISH_ZH[key]
        f = font(24)
        tw = d.textlength(label, font=f)
        d.text((left + c * cell_w + (cell_w - tw) // 2, cy + fh + 4), label,
               font=f, fill=(196, 212, 234, 255))

    corner_note(d)
    out = os.path.join(OUT, "fishing-shot-codex-3x4.png")
    img.convert("RGB").save(out)
    print("saved", out)


def overlay_tackle(scene, rod_name, bait_name, sc):
    """把鱼竿/鱼线/浮漂/鱼饵按 main/fishing.c 的坐标叠到放大后的钓场图上。

    坐标全部先在"游戏坐标"里算，再乘 sc 放大 —— 这样和真机画面是同一套位置。
    """
    def zoom(im):
        return im.resize((int(round(im.width * sc)), int(round(im.height * sc))),
                         Image.NEAREST)

    rod = zoom(Image.open(os.path.join(PNG, rod_name + ".png")).convert("RGBA"))
    fl = zoom(Image.open(os.path.join(PNG, "prop_float.png")).convert("RGBA"))
    bt = zoom(Image.open(os.path.join(PNG, bait_name + ".png")).convert("RGBA"))
    d = ImageDraw.Draw(scene)
    # 鱼线：竿尖 → 浮漂顶（LVGL 里线宽 1，这里按同一比例放大）
    tip = ((ROD_X + rod.width / sc - 6) * sc, (ROD_Y + 6) * sc)
    top = ((FLOAT_X + FLOAT_W / 2) * sc, FLOAT_Y * sc)
    d.line([tuple(int(round(v)) for v in tip), tuple(int(round(v)) for v in top)],
           fill=(244, 248, 255, 190), width=max(1, int(round(sc))))
    scene.alpha_composite(rod, (int(round(ROD_X * sc)), int(round(ROD_Y * sc))))
    scene.alpha_composite(fl, (int(round(FLOAT_X * sc)), int(round(FLOAT_Y * sc))))
    gx_bait = FLOAT_X + FLOAT_W / 2 - (bt.width / sc) / 2
    scene.alpha_composite(bt, (int(round(gx_bait * sc)),
                               int(round((FLOAT_Y + BAIT_DY) * sc))))


def make_scenes():
    img = Image.new("RGB", (W, H), INK)
    band_h = H // 3
    d = ImageDraw.Draw(img)
    for i, (key, zh, bias, rod, bait) in enumerate(SCENES):
        bg = Image.open(os.path.join(PNG, key + ".png")).convert("RGB")
        # 按宽度铺满、纵向居中裁切
        sc = W / bg.width
        bg = bg.resize((W, int(bg.height * sc)), Image.NEAREST).convert("RGBA")
        overlay_tackle(bg, rod, bait, sc)
        top = max(0, int((bg.height - band_h) * bias))
        band = bg.crop((0, top, W, top + band_h)).convert("RGB")
        img.paste(band, (0, i * band_h))
        d.rectangle([0, i * band_h, W, i * band_h + 3], fill=(10, 18, 32))
        f = font(46)
        tw = d.textlength(zh, font=f)
        # 标签放右上角：左上角会压到斜着的鱼竿
        box = (W - 36 - tw - 44, i * band_h + 28,
               W - 36, i * band_h + 104)
        d.rounded_rectangle(box, radius=14, fill=(12, 22, 38))
        d.text((W - 36 - tw - 22, i * band_h + 42), zh, font=f, fill=PAPER)

    corner_note(d)
    out = os.path.join(OUT, "fishing-shot-scenes-3x4.png")
    img.save(out)
    print("saved", out)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_codex()
    make_scenes()
    for n in ("fishing-shot-codex-3x4.png", "fishing-shot-scenes-3x4.png"):
        p = os.path.join(OUT, n)
        im = Image.open(p)
        print("  %s  %dx%d  is3x4=%s" % (n, im.width, im.height,
                                         im.width * 4 == im.height * 3))
