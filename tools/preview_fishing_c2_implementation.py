#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""竿影浮标 v5 · C+2 固件实现预览（电脑端，不依赖真机）。

用与 main/fishing.c 完全相同的坐标/色板，把五个页面画成 240x320 PNG；
再与已确认效果图 build/fish_ui/fishing_c2_concept.png 拼成对比图，供用户确认。
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, "build", "fish_ui")
SPR = os.path.join(ROOT, "assets", "sprites", "png")
W, H, S = 240, 320, 2

_F = {}
def font(size, bold=False):
    k = (size, bold)
    if k not in _F:
        _F[k] = ImageFont.truetype("C:/Windows/Fonts/simhei.ttf", size)
    return _F[k]

def load(path):
    return Image.open(path).convert("RGBA")

INK   = (8, 16, 30)
PANEL = (16, 32, 54)
PANEL2= (24, 46, 74)
HI    = (120, 156, 190)
GOLD  = (238, 190, 96)
TEAL  = (108, 190, 174)
WHITE = (232, 240, 248)
MUTED = (140, 164, 190)
RED   = (198, 76, 104)

def panel(d, x0, y0, x1, y1, fill=PANEL):
    d.rectangle([x0, y0, x1, y1], fill=INK)
    d.rectangle([x0+1, y0+1, x1-1, y1-1], fill=fill)
    d.line([x0+1, y0+1, x1-1, y0+1], fill=HI, width=1)
    d.line([x0+1, y1-1, x1-1, y1-1], fill=HI, width=1)

def hud(d):
    d.rectangle([0, 0, W, 24], fill=(0, 0, 0))
    d.text((17, 4), "得分 1280", font=font(12), fill=WHITE)
    d.text((96, 4), "最高 3200", font=font(12), fill=WHITE)
    d.rectangle([176, 9, 188, 17], outline=WHITE, width=1)
    d.rectangle([177, 10, 186, 16], fill=(90, 220, 120))
    d.rectangle([188, 11, 190, 15], fill=WHITE)
    d.text((193, 4), "82", font=font(12), fill=WHITE)

def hint(d, txt):
    d.rectangle([0, H-22, W, H], fill=(0, 0, 0))
    d.text((52, H-18), txt, font=font(10), fill=MUTED)

def base(fill=INK):
    im = Image.new("RGBA", (W, H), fill + (255,))
    return im, ImageDraw.Draw(im)

def thumb(d, im, src, x, y, w=34, h=32):
    i = load(src)
    i = i.resize((w, h), Image.NEAREST)
    im.alpha_composite(i, (x, y))
    d.rectangle([x, y, x+w-1, y+h-1], outline=(70, 100, 130), width=1)

def screen_spots():
    im, d = base()
    d.text((14, 32), "选钓场", font=font(16, True), fill=WHITE)
    d.text((14, 52), "天色为首，去哪下竿", font=font(9), fill=MUTED)
    rows = [
        ("pond", "静水塘", "晨晴 · 白条成群", False),
        ("river", "急流河", "昏雨 · 鳜鱼藏石缝", False),
        ("sea", "深海", "夜晴 · 金枪旗鱼", False),
        ("estuary", "入海口", "黄昏 · 鲈鱼洄游", True),
        ("pier", "夜钓栈桥", "夜晴 · 灯光诱鱼", False),
    ]
    for i, (key, name, hook, sel) in enumerate(rows):
        y = 68 + i * 47
        panel(d, 10, y, 230, y+42, PANEL2 if sel else PANEL)
        if sel: d.rectangle([10, y, 13, y+42], fill=RED)
        thumb(d, im, os.path.join(UI, "bg_%s.png" % key), 16, y+5)
        d = ImageDraw.Draw(im)
        d.text((58, y+4), name, font=font(12, True), fill=WHITE if sel else (200, 214, 230))
        d.text((58, y+20), "2 处标点", font=font(9), fill=TEAL)
        d.text((58, y+31), hook, font=font(8), fill=MUTED)
        if i >= 3: d.text((192, y+20), "NEW", font=font(8, True), fill=GOLD)
    hint(d, "上下选钓场 · OK进标点")
    return im.convert("RGB")

def screen_subspots():
    im, d = base(INK)
    bg = load(os.path.join(ROOT, "assets", "raw", "bg_estuary_dark.png")).convert("RGB")
    im.paste(bg, (0, 0)); d = ImageDraw.Draw(im)
    d.text((14, 32), "入海口 · 选标点", font=font(15, True), fill=WHITE)
    d.text((14, 52), "同一片水，不同位置不一样的鱼", font=font(9), fill=TEAL)
    cards = [
        ("泥滩", "出鱼：鲈鱼/鲻鱼", "退潮时鲻鱼翻泥", True),
        ("潮汐道", "出鱼：黑鲷/海鳗", "涨潮黑鲷巡游", False),
    ]
    for i, (name, fish, hook, sel) in enumerate(cards):
        y = 72 + i * 84
        panel(d, 14, y, 226, y+74, PANEL2 if sel else PANEL)
        if sel: d.rectangle([14, y, 17, y+74], fill=RED)
        d.text((26, y+8), name, font=font(15, True), fill=WHITE)
        d.text((26, y+30), fish, font=font(10), fill=GOLD)
        d.text((26, y+50), hook, font=font(9), fill=MUTED)
        if sel: d.text((186, y+8), "选中", font=font(9), fill=RED)
    panel(d, 14, 246, 226, 286, PANEL)
    d.text((22, 252), "标点只是提高概率，不锁死", font=font(9), fill=TEAL)
    d.text((22, 268), "换个位置，稀客也会来，只是少", font=font(8), fill=MUTED)
    hint(d, "左右选标点 · OK下竿")
    return im.convert("RGB")

def screen_gameplay():
    im, d = base()
    bg = load(os.path.join(UI, "bg_estuary.png")); im.alpha_composite(bg, (0, 0))
    d = ImageDraw.Draw(im)
    d.line([(62, 122), (150, 162)], fill=(244, 248, 255), width=1)
    im.alpha_composite(load(os.path.join(UI, "rod_sea.png")), (8, 116))
    im.alpha_composite(load(os.path.join(UI, "prop_float.png")), (150, 146))
    d = ImageDraw.Draw(im)
    hud(d)
    panel(d, 10, 34, 150, 72, PANEL)   # 绝对坐标：固件相对场景 (10,12)
    d.text((18, 37), "入海口", font=font(12), fill=WHITE)
    d.text((18, 53), "泥滩 · 鲈鱼/鲻鱼", font=font(9), fill=TEAL)
    d.line([(18, 68), (134, 68)], fill=(46, 72, 100), width=1)
    d.rectangle([10, 74, 150, 108], fill=(0, 0, 0))
    d.text((14, 76), "黄昏 · 小雨", font=font(10), fill=WHITE)
    d.text((14, 92), "退潮时鲻鱼翻泥", font=font(9), fill=MUTED)
    panel(d, 126, 180, 230, 206, PANEL)
    d.text((134, 186), "浮漂静着，等一口", font=font(9), fill=MUTED)
    hint(d, "OK 抛竿 · 长按菜单")
    return im.convert("RGB")

def screen_codex_list():
    im, d = base()
    d.text((14, 32), "图鉴 · 钓获", font=font(16, True), fill=WHITE)
    d.text((14, 52), "每一条都记着是在哪钓的", font=font(9), fill=MUTED)
    items = [
        ("baitiao", "1. 白条", "静水塘·芦苇荡", "晨 · 蚯蚓", "18.0cm"),
        ("guiyu",   "13. 鳜鱼", "急流河·浅滩", "昏 · 亮片", "35.0cm"),
        ("luyu",    "12. 鲈鱼", "入海口·泥滩", "昏 · 面团", "41.0cm"),
        ("jinqiang","22. 金枪鱼", "深海·远洋", "夜 · 亮片", "92.0cm"),
    ]
    for i, (key, name, where, when, size) in enumerate(items):
        y = 68 + i * 58
        panel(d, 12, y, 228, y+52, PANEL)
        p = os.path.join(SPR, "thumb_fish_%s.png" % key)
        if os.path.exists(p): thumb(d, im, p, 18, y+8)
        d = ImageDraw.Draw(im)
        d.text((60, y+4), name, font=font(12, True), fill=WHITE)
        d.text((60, y+21), where, font=font(9), fill=GOLD)
        d.text((60, y+37), when, font=font(8), fill=MUTED)
        d.text((184, y+21), size, font=font(9), fill=TEAL)
    hint(d, "已收录 27 种 · 标点 10 处")
    return im.convert("RGB")

def screen_codex_detail():
    im, d = base((6, 18, 36))
    hud(d)
    p = os.path.join(SPR, "fish_luyu.png")
    if os.path.exists(p):
        im.alpha_composite(load(p).resize((72, 48), Image.NEAREST), (84, 28))
    d = ImageDraw.Draw(im)
    d.text((8, 66), "12. 鲈鱼 · 少见 ✓", font=font(16, True), fill=WHITE)
    d.text((8, 88), "最大 41.2 厘米 · 2.5千克", font=font(12), fill=GOLD)
    d.text((8, 110), "首次 第12日 · 泥滩", font=font(12), fill=GOLD)
    d.text((8, 132), "体型 20-60 厘米 · 爱面团", font=font(12), fill=(150, 215, 255))
    d.text((8, 154), "淡水中常见的掠食者，喜欢贴着水草伏击。", font=font(12), fill=WHITE)
    d.text((8, 198), "河口的老滑头，饵不对口它只看不咬。", font=font(12), fill=(255, 236, 205))
    d.text((8, 242), "集齐 12 种 · 半塘好手", font=font(12), fill=(150, 230, 175))
    d.text((8, 264), "收录 4/27（14%）  钓获 36", font=font(12), fill=(190, 210, 240))
    hint(d, "上下翻·OK详情·长按返回")
    return im.convert("RGB")

def compose():
    impls = [("屏1 选钓场", screen_spots()), ("屏2 选标点", screen_subspots()),
             ("屏3 主界面双行胶囊", screen_gameplay()), ("屏4 图鉴列表", screen_codex_list()),
             ("屏5 图鉴详情", screen_codex_detail())]
    path = os.path.join(UI, "fishing_c2_implementation.png")
    sw, sh = W*S, H*S
    sheet = Image.new("RGB", (sw*len(impls) + 20*(len(impls)+1), sh+90), (238, 242, 246))
    d = ImageDraw.Draw(sheet)
    d.text((20, 12), "竿影浮标 v5 · C+2 固件实现预览（与 main/fishing.c 同坐标/色板）",
           font=font(18, True), fill=(22, 36, 52))
    for i, (label, im) in enumerate(impls):
        x = 20 + i*(sw+20)
        d.text((x, 44), label, font=font(13, True), fill=(30, 44, 60))
        sheet.paste(im.resize((sw, sh), Image.NEAREST), (x, 70))
    sheet.save(path)
    print("saved", path, sheet.size)

    # 与用户已确认的效果图拼成对比图（前四屏一一对应）
    concept = Image.open(os.path.join(UI, "fishing_c2_concept.png")).convert("RGB")
    pad, gap, lab = 26, 22, 34
    sw, sh = W*S, H*S
    pairs = []
    for i in range(4):
        cx = pad + (i % 2) * (sw + gap)
        cy = 64 + (i // 2) * (lab + sh + gap) + lab
        pairs.append(concept.crop((cx, cy, cx + sw, cy + sh)))
    row_h = sh + 46
    cmp_sheet = Image.new("RGB", (sw*2 + pad*3, row_h*4 + 70), (238, 242, 246))
    cd = ImageDraw.Draw(cmp_sheet)
    cd.text((pad, 12), "竿影浮标 v5 · C+2 效果图 / 固件实现对比",
            font=font(18, True), fill=(22, 36, 52))
    cd.text((pad, 38), "左：用户已确认效果图　右：按 main/fishing.c 同坐标渲染的实现预览",
            font=font(11), fill=(100, 118, 138))
    for i in range(4):
        y = 62 + i * row_h
        cd.text((pad, y), "效果图 屏%d" % (i+1), font=font(12, True), fill=(30, 44, 60))
        cd.text((pad*2 + sw, y), "实现 屏%d" % (i+1), font=font(12, True), fill=(30, 44, 60))
        cmp_sheet.paste(pairs[i], (pad, y + 20))
        cmp_sheet.paste(impls[i][1].resize((sw, sh), Image.NEAREST), (pad*2 + sw, y + 20))
    cmp_path = os.path.join(UI, "fishing_c2_compare.png")
    cmp_sheet.save(cmp_path)
    print("saved", cmp_path, cmp_sheet.size)
    return impls

if __name__ == "__main__":
    compose()
