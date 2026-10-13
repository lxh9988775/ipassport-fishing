#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_mage_battle.py —— 在电脑上精确渲染「固件战斗界面」预期效果图。

目的（硬性流程约束）：任何界面改动，先出这张图给作者与 HTML 原版截图对比，
作者确认后才允许烧录。不跑硬件、不跑 LVGL，直接用 main/mage_hud.c 与
main/mage_fx.c 里**同一套坐标常量**画出来，所以"预览长什么样，实机就长什么样"。

坐标全部来自源码常量，1:1 对应：
  顶部 HUD 26px     : mage_hud.c  HUD_H=26
  血条 HP/MP       : mage_hud.c  bar(20,4,58,7) / bar(20,15,58,5)
  战斗画面带       : mage_fx.c   BG_Y=62 BG_H=132, GROUND_Y=250
  角色尺寸         : HTML drawSprite(spHero,46) / ENEMY.h
  底部动作卡 44px  : mage_hud.c  CARD_Y=H-44=276 CARD_W=72 CARD_H=40, x=8+i*78
  回合徽标         : mage_hud.c  box(W/2-lw/2, y-7, lw, 14)

产出：assets/preview/mage_battle_240x320.png（原生）+ 放大版
"""
from __future__ import annotations

import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "mage-game", "assets")
OUT = os.path.join(ROOT, "assets", "preview")

# ---- 源码常量（与 main/mage_hud.c / main/mage_fx.c 保持一致）----
W, H = 240, 320
HUD_H = 26
CARD_Y, CARD_W, CARD_H = H - 44, 72, 40
BG_Y, BG_H, GROUND_Y = 0, 320, 250

def font(px):
    """中文字体优先（consola 无中文字形，会画成方框）。"""
    for name in ("msyh.ttc", "simhei.ttf", "simsun.ttc", "consola.ttf", "arial.ttf"):
        for base in (r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu"):
            p = os.path.join(base, name)
            if os.path.exists(p):
                try:
                    return ImageFont.truetype(p, px)
                except Exception:
                    pass
    return ImageFont.load_default()

F8, F9, F10, F12, F16 = font(8), font(9), font(10), font(12), font(16)


def cover(img_path, w, h):
    """严格复刻 HTML drawBattle 的背景取窗：
         win = round(bg.height * (W/H));  sx = (cam*0.14 + t*3) % (bg.width-win)
         drawImage(bg, sx, 0, win, bg.height, 0, 0, W, H)
       取 range 中点作静态代表帧。"""
    im = Image.open(img_path).convert("RGB")
    sw, sh = im.size
    win = int(round(sh * (w / h)))
    if win > sw:
        win = sw
    left = (sw - win) // 2
    return im.crop((left, 0, left + win, sh)).resize((w, h), Image.LANCZOS)


def draw_sprite(im, d, png, cx, foot_y, disp_h):
    """按 HTML drawSprite(key, x, footY, hPix, 1, tilt)：脚底中心锚点、等比、1:1 像素。"""
    if not os.path.exists(png):
        return
    s = Image.open(png).convert("RGBA")
    ratio = s.height / s.width
    w = max(1, int(round(disp_h / ratio)))
    s = s.resize((w, disp_h), Image.LANCZOS)
    im.paste(s, (int(cx - w / 2), int(foot_y - disp_h)), s)


def main():
    os.makedirs(OUT, exist_ok=True)
    im = Image.new("RGB", (W, H), (0x15, 0x10, 0x2a))
    d = ImageDraw.Draw(im)

    # ---- 战斗背景（cover 铺满）----
    bg = os.path.join(ASSETS, "battle_bg_k.png")
    if os.path.exists(bg):
        # HTML: drawImage(bg, sx,0,sw,bg.height, 0,0,W,H) —— 整屏 cover 铺满
        im.paste(cover(bg, W, H), (0, 0))

    # ---- 活动带压暗（HTML gDim: 140..270 半透明黑渐变）----
    # 必须用 alpha 混合，不能直接 fill 纯色（会把背景涂死成黑块）
    dim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dim)
    for y in range(140, min(H, 271)):
        t = (y - 140) / 130.0
        a = int(70 * (1 - t))
        dd.rectangle([0, y, W, y], fill=(6, 5, 16, max(0, min(70, a))))
    im = Image.alpha_composite(im.convert("RGBA"), dim).convert("RGB")
    d = ImageDraw.Draw(im)

    # ---- 地面线 ----
    d.rectangle([0, GROUND_Y, W, GROUND_Y + 2], fill=(0x5c, 0x8f, 0x42))

    # ---- 敌人：HTML engageStop = bt.px + 78 + i*16，bt.px=70 → 148/164/180 ----
    # 史莱姆 ENEMY.slime.h=24，猫灵 cat.h=32
    draw_sprite(im, d, os.path.join(ASSETS, "sp_slime_k.png"), 148, GROUND_Y, 24)
    draw_sprite(im, d, os.path.join(ASSETS, "sp_slime_k.png"), 164, GROUND_Y, 24)
    # 血条：HTML bar(sx-12, sy-26, 24, 3)
    d.rectangle([136, GROUND_Y - 26, 160, GROUND_Y - 23], fill=(0x33, 0x1a, 0x1a))
    d.rectangle([136, GROUND_Y - 26, 160, GROUND_Y - 23], fill=(0xe0, 0x55, 0x55))
    d.text((163, GROUND_Y - 30), "Lv1", font=F8, fill=(0xc9, 0xb8, 0xee))

    # ---- 伙伴猫：HTML psx = bt.px - 26 = 44, drawSprite(spCat,...,26) ----
    draw_sprite(im, d, os.path.join(ASSETS, "sp_cat_k.png"), 44, 246, 26)

    # ---- 主角：HTML drawSprite('spHero', bt.px, 250, 46) ----
    draw_sprite(im, d, os.path.join(ASSETS, "sp_hero_k.png"), 70, GROUND_Y, 46)

    # ---- 前景草丛 ----
    for i in range(9):
        x = (i * 41) % (W + 41) - 20
        d.polygon([(x, H + 2), (x + 6, H - 10), (x + 12, H + 2)], fill=(0x14, 0x24, 0x0e))

    # ---- 顶部 HUD 26px ----
    d.rectangle([0, 0, W, HUD_H], fill=(0x10, 0x0c, 0x1a), outline=(0x6a, 0x5a, 0x3a))
    d.text((7, 3), "HP", font=F8, fill=(0xff, 0x88, 0x88))
    d.rectangle([20, 4, 78, 11], fill=(0x3a, 0x1a, 0x1a))
    d.rectangle([20, 4, 78, 11], fill=(0xe0, 0x55, 0x55))
    d.text((82, 3), "80/80", font=F8, fill=(0xff, 0xff, 0xff))
    d.text((7, 14), "MP", font=F8, fill=(0x7c, 0xb8, 0xff))
    d.rectangle([20, 15, 78, 20], fill=(0x1a, 0x2a, 0x3a))
    d.rectangle([20, 15, 78, 20], fill=(0x5e, 0xc8, 0xf2))
    d.text((82, 14), "50/50", font=F8, fill=(0x5e, 0xc8, 0xf2))
    d.text((W - 26, 3), "1/10", font=F10, fill=(0xff, 0xd9, 0x3d))
    d.text((W - 92, 14), "AUTO编1 球3 卡1 药2", font=F8, fill=(0x7f, 0xdc, 0x8a))

    # ---- 底部三张动作卡 ----
    cards = [("A", "攻击", (0xff, 0xb7, 0x4d), (0xff, 0x8a, 0x3d)),
             ("B", "魔法", (0x7c, 0xd4, 0xff), (0x5a, 0x8a, 0xff)),
             ("C", "道具", (0x8f, 0xe8, 0x9a), (0x3d, 0xbd, 0x7a))]
    for i, (key, name, c0, c1) in enumerate(cards):
        x = 8 + i * 78
        d.rectangle([x, CARD_Y, x + CARD_W, CARD_Y + CARD_H], fill=(0x24, 0x1d, 0x4a),
                    outline=(0x8a, 0x6a, 0x2a))
        d.rectangle([x + 2, CARD_Y + 3, x + 5, CARD_Y + 37], fill=c1)
        d.rectangle([x + 2, CARD_Y + 3, x + 5, CARD_Y + 8], fill=c0)
        d.ellipse([x + 7, CARD_Y + 4, x + 22, CARD_Y + 19], fill=(0x1a, 0x14, 0x30),
                  outline=(0xc9, 0xa4, 0x4a))
        tw = d.textlength(key, font=F9)
        d.text((x + 14 - tw / 2, CARD_Y + 7), key, font=F9, fill=(0xff, 0xd9, 0x3d))
        tw = d.textlength(name, font=F12)
        d.text((x + 38 - tw / 2, CARD_Y + 18), name, font=F12, fill=(0xff, 0xff, 0xff))

    # ---- 回合徽标 ----
    lb = "回合 1"
    lw = len(lb) * 6 + 14
    bx = W // 2 - lw // 2
    d.rectangle([bx, CARD_Y - 7, bx + lw, CARD_Y + 7], fill=(0x0d, 0x0a, 0x1a),
                outline=(0xc9, 0xa4, 0x4a))
    tw = d.textlength(lb, font=F8)
    d.text((W / 2 - tw / 2, CARD_Y - 5), lb, font=F8, fill=(0xff, 0xd9, 0x3d))

    p1 = os.path.join(OUT, "mage_battle_240x320.png")
    im.save(p1)
    im.resize((W * 2, H * 2), Image.LANCZOS).save(
        os.path.join(OUT, "mage_battle_2x.png"))
    print("输出：", p1)


if __name__ == "__main__":
    main()
