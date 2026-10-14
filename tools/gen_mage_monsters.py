#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 HTML 原版（mage-game/index.html）的 drawMob / drawMobArt / 手绘函数 1:1 移植到
PIL，把全部 50 种怪物 + 5 种 BOSS（2.1x）渲染为带透明通道的 PNG。

为什么不用浏览器截图：本机没装 Playwright/Chromium 二进制；且 HTML 的绘制函数是
参数化 + 手绘混合，直接移植到 PIL 确定性更强、可控、可复现。

输出：mage-game/assets/sp_<type>.png  （普通怪，原生像素尺寸）
      mage-game/assets/sp_<type>_boss.png  （5 种 BOSS，2.1x 放大）

透明黑底问题（用户问题②）的根因是旧 gen_mage_sprites 把 alpha 预乘烘焙成背景色；
这里输出真·透明 RGBA，交给新版 gen_mage_sprites 转 LV_COLOR_FORMAT_RGB565A8。

用法：python tools/gen_mage_monsters.py
"""
import os
import math
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "mage-game", "assets")

# 画布固定尺寸（足够容纳 2.1x 最大怪）。脚底锚点 footY0，水平中心 cx。
CW, CH = 240, 260
CX, FOOTY = 120, 240

# 全局变换（模拟 canvas 的 translate/scale）：绝对 = (ox + scale*rx, oy + scale*ry)
G = {"scale": 1.0, "ox": CX, "oy": FOOTY, "alpha": 1.0, "fill": (0, 0, 0, 255)}
_draw = None  # ImageDraw 在渲染时绑定


def set_alpha(a):
    G["alpha"] = a


def set_fill(c):
    if c is None:
        return
    if isinstance(c, tuple):
        G["fill"] = (c[0], c[1], c[2], int(255 * G["alpha"]))
        return
    c = c.lstrip("#")
    if len(c) == 3:
        c = c[0] * 2 + c[1] * 2 + c[2] * 2
    r = int(c[0:2], 16); g = int(c[2:4], 16); b = int(c[4:6], 16)
    G["fill"] = (r, g, b, int(255 * G["alpha"]))


def _T(x, y):
    return (G["ox"] + G["scale"] * x, G["oy"] + G["scale"] * y)


def _Ts(v):
    return G["scale"] * v


def px(x, y, w, h, c=None):
    """fillRect"""
    if c is not None:
        set_fill(c)
    x0, y0 = _T(x, y)
    ww, hh = _Ts(w), _Ts(h)
    _draw.rectangle([x0, y0, x0 + ww, y0 + hh], fill=G["fill"])


def circ(x, y, r, c=None):
    """arc/fill 圆（HTML circ 只有实心圆）"""
    if c is not None:
        set_fill(c)
    cx, cy = _T(x, y)
    rr = _Ts(r)
    _draw.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=G["fill"])


def ellipse(x, y, rx, ry, c=None):
    """ctx.ellipse(x,y,rx,ry) -> 中心 (x,y) 半径 (rx,ry)"""
    if c is not None:
        set_fill(c)
    cx, cy = _T(x, y)
    rxr, ryr = _Ts(rx), _Ts(ry)
    _draw.ellipse([cx - rxr, cy - ryr, cx + rxr, cy + ryr], fill=G["fill"])


def tri(cx, base, w, h, c=None):
    """HTML tri(cx,base,w,h): (cx-w/2,base),(cx,base-h),(cx+w/2,base)"""
    if c is not None:
        set_fill(c)
    p1 = _T(cx - w / 2, base)
    p2 = _T(cx, base - h)
    p3 = _T(cx + w / 2, base)
    _draw.polygon([p1, p2, p3], fill=G["fill"])


def mobShade(c, f):
    c = c.lstrip("#")
    r = int(c[0:2], 16); g = int(c[2:4], 16); b = int(c[4:6], 16)
    if f >= 0:
        r = round(r + (255 - r) * f); g = round(g + (255 - g) * f); b = round(b + (255 - b) * f)
    else:
        r = round(r * (1 + f)); g = round(g * (1 + f)); b = round(b * (1 + f))
    r = max(0, min(255, r)); g = max(0, min(255, g)); b = max(0, min(255, b))
    return "#%02x%02x%02x" % (r, g, b)


MOB_SHAPE_SPEC = {
    "blob":  {"bw": 22, "bh": 26, "top": 26},
    "float": {"bw": 18, "bh": 20, "top": 22, "hover": 8},
    "quad":  {"bw": 18, "bh": 14, "top": 25},
    "biped": {"bw": 14, "bh": 16, "top": 27},
}


def _clamp(v, a, b):
    return max(a, min(b, v))


def drawMobArt_body(art):
    """绘制参数化立绘（drawMobArt 本体）。调用前须设定 G 变换。"""
    c = art.get("c", "#999")
    c2 = art.get("c2", mobShade(c, -0.35))
    eye = art.get("eye", "#222")
    spec = MOB_SHAPE_SPEC.get(art.get("shape"), MOB_SHAPE_SPEC["biped"])
    S = _clamp(art.get("sz", 1), 0.75, 1.25)
    # t=0：breathing br≈1（中性姿态）
    br = 1.0
    W2 = spec["bw"] / 2.0
    BH = spec["bh"]
    hov = spec.get("hover", 0)
    # 应用本函数内部 translate(0,-hov)*scale(S) 到全局变换
    G["scale"] = G["_sGlobal"] * S
    G["ox"] = CX
    G["oy"] = FOOTY - G["_sGlobal"] * S * hov

    # 注：上方已改过 G，下面用 px/ellipse/tri 时坐标即局部坐标（脚底原点 0）
    OL = mobShade(c, -0.55)
    HI = mobShade(c, 0.30)
    hT = -BH if art.get("shape") in ("blob", "float") else -spec["top"]

    if art.get("shape") == "blob":
        set_fill(OL); ellipse(0, -BH / 2 - 0.5, W2 + 1, BH / 2 + 1)
        set_fill(c); ellipse(0, -BH / 2, W2, BH / 2)
        set_fill(HI); ellipse(-W2 * 0.28, -BH * 0.68, W2 * 0.42, BH * 0.26)
        set_fill(c2); px(-W2 + 1, -4, W2 * 2 - 2, 3)
    elif art.get("shape") == "float":
        set_fill(OL); ellipse(0, -BH / 2 - 0.5, W2, BH / 2 + 1)
        set_fill(c); ellipse(0, -BH / 2, W2 - 1, BH / 2)
        set_fill(HI); px(-W2 * 0.5, -BH * 0.72, 4, 5)
        set_fill(c2); px(-3, -2, 2, 3); px(1, -2, 2, 3)
    elif art.get("shape") == "quad":
        bT = hT + 8
        set_fill(c); px(W2 - 4, hT, 9, 9)
        set_fill(mobShade(c, -0.4)); px(W2 + 4, hT + 2, 2, 4)
        set_fill(OL); px(-W2 - 1, bT - 1, W2 * 2 + 2, BH - 2)
        set_fill(c); px(-W2, bT, W2 * 2, BH - 4)
        set_fill(c2); px(-W2, -6, W2 * 2, 3)
        set_fill(HI); px(-W2, bT, W2 * 2, 3)
        set_fill(mobShade(c, -0.3)); px(-W2 + 1, -4, 3, 4); px(W2 - 4, -4, 3, 4)
    else:  # biped
        bT = -BH - 2
        set_fill(OL); px(-W2 - 1, bT - 1, W2 * 2 + 2, BH)
        set_fill(c); px(-W2, bT, W2 * 2, BH - 3)
        set_fill(c2); px(-W2, -6, W2 * 2, 3)
        set_fill(HI); px(-W2, bT, W2 * 2, 3)
        set_fill(OL); px(-W2 - 2, hT, W2 * 2 + 4, 10)
        set_fill(c); px(-W2 - 1, hT + 1, W2 * 2 + 2, 8)
        set_fill(HI); px(-W2, hT + 2, W2 * 2, 3)
        set_fill(mobShade(c, -0.3)); px(-W2 + 1, -4, 3, 4); px(W2 - 4, -4, 3, 4)

    # 耳
    ear = art.get("ear", 0)
    if ear == 1:
        set_fill(mobShade(c, -0.2)); tri(-5, hT, 4, 6); tri(5, hT, 4, 6)
    elif ear == 2:
        set_fill(mobShade(c, -0.2)); tri(-6, hT - 2, 6, 9); tri(6, hT - 2, 6, 9)
        set_fill(mobShade(c, 0.25)); tri(-5, hT - 1, 3, 5); tri(5, hT - 1, 3, 5)
    # 角
    horn = art.get("horn", 0)
    if horn == 1:
        set_fill("#e8e0c8"); px(-3, hT - 6, 2, 6); px(2, hT - 6, 2, 6)
    elif horn == 2:
        set_fill("#e8e0c8"); tri(-4, hT - 6, 4, 9); tri(3, hT - 6, 4, 9)
    # 背翼
    if art.get("wing"):
        set_fill(mobShade(c, -0.5)); tri(0, hT + 5, 13, 17); tri(0, hT + 5, -13, 17)
        set_fill(mobShade(c, 0.25)); tri(0, hT + 7, 9, 13); tri(0, hT + 7, -9, 13)
    # 尾
    if art.get("tail"):
        set_fill(mobShade(c, -0.2))
        if art.get("shape") == "quad":
            px(-W2 - 3, -BH * 0.55, 4, 3); px(-W2 - 5, -BH * 0.55 - 2, 3, 5)
        else:
            px(-W2 - 1, -BH * 0.4, 4, 3); px(-W2 - 3, -BH * 0.4 - 3, 3, 4)
    # 眼
    eyY = hT + 4 if art.get("shape") in ("quad", "biped") else -BH * 0.65
    ex = W2 + 1 if art.get("shape") == "quad" else 0
    set_fill(eye); px(-3 + ex, eyY, 2, 2); px(2 + ex, eyY, 2, 2)
    set_fill("#fff"); px(-3 + ex, eyY, 1, 1); px(2 + ex, eyY, 1, 1)
    if art.get("shape") not in ("blob", "float"):
        set_fill(mobShade(c, -0.4)); px(ex - 1, eyY + 4, 3, 1)


# ---------------- 手绘怪物（旧 11 种，移植 HTML 对应函数） ----------------

def drawGoblin(x, footY, t):
    step = -1  # sin(0)=0 -> not >0
    set_fill("#1c2e14"); px(x - 7, footY - 18, 14, 15); px(x - 8, footY - 26, 16, 9)
    set_fill("#5a8f3a"); px(x - 6, footY - 17, 12, 13); px(x - 6, footY - 17, 12, 4); px(x - 6, footY - 8, 12, 4)
    set_fill("#254018"); px(x - 7, footY - 25, 14, 8); px(x - 6, footY - 24, 12, 6)
    set_fill("#6fa84a"); px(x - 9, footY - 27, 4, 4); px(x + 5, footY - 27, 4, 4)
    set_fill("#5a8f3a"); px(x - 8, footY - 26, 3, 3); px(x + 5, footY - 26, 3, 3)
    set_fill("#0f0f14"); px(x - 4, footY - 22, 3, 3); px(x + 2, footY - 22, 3, 3)
    set_fill("#ff5a5a"); px(x - 3, footY - 21, 1, 1); px(x + 3, footY - 21, 1, 1)
    set_fill("#2c4420"); px(x - 2, footY - 18, 4, 1)
    set_fill("#436f2c"); px(x - 4 + step, footY - 4, 4, 4); px(x + 1 - step, footY - 4, 4, 4)
    set_fill("#3a2412"); px(x + 6, footY - 19, 2, 14); set_fill("#8b5a2b"); px(x + 5, footY - 20, 4, 2); px(x + 4, footY - 6, 6, 2)


def drawSlime(x, footY, t):
    sq = 0.0  # sin(0)=0 -> w=13, hh=12
    w = 13; hh = 12
    set_fill("#153815"); px(x - 7, footY - 12, 14, 12)
    set_fill("#3f9f4f"); px(x - (w >> 1), footY - hh, w, hh); px(x - (w >> 1) + 1, footY - hh, w - 2, 3); px(x - (w >> 1) + 1, footY - 4, w - 2, 3)
    set_fill("#0f2418"); px(x - 4, footY - 9, 3, 3); px(x + 2, footY - 9, 3, 3)
    set_fill("#dff6ff"); px(x - 4, footY - 9, 1, 1); px(x + 3, footY - 9, 1, 1)
    set_fill("#1f5c2c"); px(x - 2, footY - 4, 5, 1)
    set_fill("#7ed88a"); px(x - 8, footY - 9, 2, 3); px(x + 7, footY - 8, 2, 2)
    set_alpha(0.3); circ(x, footY - 16, 2, "#b8ffc4"); set_alpha(1.0)


def drawBat(x, footY, t):
    w = 0  # sin(0)=0 -> w=0
    set_fill("#241a33"); tri(x - 6, footY - 12 - w, 8, 6); tri(x + 6, footY - 12 - w, 8, 6)
    set_fill("#0f0a18"); px(x - 5, footY - 15, 10, 9)
    set_fill("#5a4472"); px(x - 4, footY - 14, 8, 7); px(x - 4, footY - 14, 8, 2); px(x - 4, footY - 9, 8, 2)
    set_fill("#241a33"); px(x - 3, footY - 17, 2, 3); px(x + 1, footY - 17, 2, 3)
    set_fill("#0f0f14"); px(x - 3, footY - 12, 2, 2); px(x + 1, footY - 12, 2, 2)
    set_fill("#ff7a7a"); px(x - 3, footY - 12, 1, 1); px(x + 2, footY - 12, 1, 1)
    set_fill("#fff"); px(x - 1, footY - 9, 2, 1)
    set_fill("#3f3055"); tri(x - 5, footY - 12 - w, 7, 5); tri(x + 5, footY - 12 - w, 7, 5)


def drawSkel(x, footY, t):
    sway = 0  # sin(0)>0? sin0=0 not >0 -> sway=0
    set_fill("#1a1a24"); px(x - 5, footY - 13, 10, 13)
    set_fill("#ddd6c2"); px(x - 4, footY - 12, 3, 11); px(x + 1, footY - 12, 3, 11); px(x - 4, footY - 8, 8, 2)
    set_fill("#0f0f18"); px(x - 4, footY - 21, 8, 9)
    set_fill("#e8e2d0"); px(x - 3, footY - 20, 6, 8); px(x - 3, footY - 20, 6, 2); px(x - 3, footY - 15, 6, 3)
    set_fill("#0f0f14"); px(x - 3, footY - 18, 2, 2); px(x + 1, footY - 18, 2, 2)
    set_fill("#7cf"); px(x - 3, footY - 18, 1, 1); px(x + 2, footY - 18, 1, 1)
    set_fill("#0f0f14"); px(x - 2, footY - 14, 4, 1)
    set_fill("#b8b09a"); px(x - 7 + sway, footY - 18, 3, 8); px(x + 4 - sway, footY - 18, 3, 8)
    set_fill("#3a2412"); px(x + 7, footY - 24, 2, 15); set_fill("#b0b8c8"); px(x + 6, footY - 25, 4, 3); px(x + 6, footY - 25, 4, 1)


def drawMagma(x, footY, t):
    sq = 0.0  # sin(0)*1.2=0 -> bob=0
    bob = 0
    set_fill("#160a08"); px(x - 7, footY - 13 + bob, 14, 13)
    set_fill("#2a1614"); px(x - 6, footY - 12 + bob, 12, 12 - sq); px(x - 6, footY - 12 + bob, 12, 3); px(x - 6, footY - 4 + bob, 12, 3)
    set_fill("#ff7b2e"); px(x - 4, footY - 9 + bob, 8, 5); px(x - 2, footY - 7 + bob, 4, 3); px(x - 3, footY - 8 + bob, 2, 1)
    set_fill("#fff3b0"); px(x - 3, footY - 8 + bob, 1, 1)
    set_fill("#0f0a08"); px(x - 4, footY - 11, 3, 3); px(x + 1, footY - 11, 3, 3)
    set_fill("#fff"); px(x - 3, footY - 10, 1, 1); px(x + 2, footY - 10, 1, 1)
    set_fill("#3a1a14"); px(x - 5, footY - 14 + bob, 10, 2)
    set_alpha(0.4); px(x - 7, footY - 15 + bob, 14, 2, "#ff7b2e"); circ(x, footY - 3 + bob, 3, "#ff9a4d"); set_alpha(1.0)


def drawSala(x, footY, t):
    wag = 0  # sin(0)*2=0
    set_fill("#180c08"); px(x - 9, footY - 11, 18, 11)
    set_fill("#c2452a"); px(x - 8, footY - 10, 12, 7); px(x - 8, footY - 10, 12, 2); px(x - 8, footY - 5, 12, 2)
    set_fill("#0f0806"); px(x - 10, footY - 17, 7, 7)
    set_fill("#d85838"); px(x - 9, footY - 16, 6, 6)
    set_fill("#ffd93d"); px(x - 6, footY - 14, 2, 2)
    set_fill("#fff"); px(x - 9, footY - 15, 1, 1)
    set_fill("#0f0806"); px(x + 3, footY - 18, 3, 18); set_fill("#a83a24"); px(x + 4, footY - 18, 3, 18); px(x + 4, footY - 18, 3, 3)
    set_fill("#e06a3f"); px(x + 6 + wag, footY - 20, 2, 3)
    set_fill("#5c1c10"); px(x - 2, footY - 8, 7, 2)
    set_alpha(0.3); px(x - 9, footY - 19, 7, 2, "#ffd93d"); set_alpha(1.0)


def drawWolf(x, footY, t):
    br = False  # sin(0)>0? sin0=0 not >0 -> br=False
    set_fill("#0e1520"); px(x - 13, footY - 16, 22, 12)
    set_fill("#c9d4e2"); px(x - 12, footY - 9, 12, 7); px(x - 12, footY - 9, 12, 2); px(x - 12, footY - 4, 12, 2)
    set_fill("#0e1520"); px(x - 16, footY - 15, 8, 7); set_fill("#c9d4e2"); px(x - 15, footY - 14, 7, 6); px(x - 15, footY - 14, 3, 3)
    set_fill("#0e1520"); px(x - 17, footY - 18, 2, 4); px(x - 13, footY - 18, 2, 4); set_fill("#c9d4e2"); px(x - 17, footY - 17, 2, 3); px(x - 13, footY - 17, 2, 3)
    set_fill("#0f0f14"); px(x - 14, footY - 12, 2, 2); px(x - 11, footY - 12, 2, 2)
    set_fill("#7cf"); px(x - 14, footY - 12, 1, 1); px(x - 10, footY - 12, 1, 1)
    set_fill("#c9d4e2"); px(x - 17, footY - 8, 2, 2)
    legcol = "#aebccd" if br else "#b8c4d4"
    set_fill(legcol); px(x + 8, footY - 13, 2, 13); px(x - 1, footY - 13, 2, 13)
    set_fill("#eef4fa"); px(x - 6, footY - 13, 4, 2); set_fill("#aebccd"); px(x - 8, footY - 11, 6, 1)


def drawFrost(x, footY, t):
    g = 0.5  # 0.5+0.5*sin(0)=0.5
    fl = 0.0  # sin(0)*2=0
    set_alpha(0.3 + 0.3 * g); circ(x, footY - 13 + fl, 10, "#aee7ff"); set_alpha(1.0)
    set_fill("#0e2836"); tri(x, footY - 19 + fl, 7, 15); tri(x - 7, footY - 9 + fl, 6, 10); tri(x + 7, footY - 9 + fl, 6, 10)
    set_fill("#d8f4ff"); tri(x, footY - 20 + fl, 5, 14); tri(x - 6, footY - 10 + fl, 4, 9); tri(x + 6, footY - 10 + fl, 4, 9)
    circ(x - 2, footY - 14 + fl, 1.4, "#0e2836"); circ(x + 2, footY - 14 + fl, 1.4, "#0e2836")
    set_alpha(g); circ(x, footY - 24 + fl, 1.6, "#fff"); set_alpha(1.0)


def drawGolem(x, footY, t):
    g = 0.4  # 0.4+0.4*sin(0)=0.4
    set_fill("#141210"); px(x - 10, footY - 19, 20, 18)
    set_fill("#7a7468"); px(x - 9, footY - 18, 18, 17); px(x - 9, footY - 18, 18, 4); px(x - 9, footY - 5, 18, 4)
    set_fill("#0f0d0b"); px(x - 11, footY - 14, 4, 10); px(x + 7, footY - 14, 4, 10)
    set_fill("#6a6458"); px(x - 10, footY - 14, 3, 9); px(x + 8, footY - 14, 3, 9)
    set_fill("#0f0d0b"); px(x - 7, footY - 27, 14, 10); set_fill("#8a8478"); px(x - 6, footY - 26, 12, 9); px(x - 6, footY - 26, 12, 2)
    set_fill("#0f2a33"); px(x - 4, footY - 24, 3, 3); px(x + 2, footY - 24, 3, 3); px(x - 4, footY - 24, 2, 2, "#5ec8f2"); px(x + 3, footY - 24, 2, 2, "#5ec8f2")
    set_fill("#4a4438"); px(x - 4, footY - 20, 8, 1)
    set_alpha(g); px(x - 3, footY - 16, 6, 2, "#5ec8f2"); px(x - 8, footY - 10, 16, 1, "#5ec8f2"); set_alpha(1.0)


def drawGhost(x, footY, t):
    fl = 0.0  # sin(0)*2=0
    footY = footY + int(fl)
    set_fill("#171226"); px(x - 7, footY - 19, 14, 16)
    set_alpha(0.85)
    set_fill("#8a7ab8"); px(x - 6, footY - 18, 12, 14); px(x - 6, footY - 18, 12, 3); px(x - 6, footY - 9, 12, 3)
    set_fill("#171226"); px(x - 4, footY - 22, 8, 5); px(x - 3, footY - 21, 6, 4, "#8a7ab8")
    set_fill("#8a7ab8"); px(x - 6, footY - 8, 2, 3); px(x - 2, footY - 7, 2, 4); px(x + 2, footY - 8, 2, 3)
    set_fill("#0f0a1a"); px(x - 4, footY - 17, 3, 4); px(x + 1, footY - 17, 3, 4); px(x - 4, footY - 17, 2, 3, "#ffec9e"); px(x + 2, footY - 17, 2, 3, "#ffec9e")
    set_fill("#3a2a5a"); px(x - 2, footY - 11, 4, 1); px(x - 5, footY - 13, 10, 1, "#6a5a9a")
    set_alpha(1.0)


def drawCat(x, footY, t, border=False):
    set_fill("#fdfdfd")
    px(x - 5, footY - 15, 10, 11); px(x - 5, footY - 19, 3, 5); px(x + 2, footY - 19, 3, 5)
    set_fill("#ff9ec4"); px(x - 4, footY - 18, 1, 2); px(x + 3, footY - 18, 1, 2)
    set_fill("#fdfdfd"); px(x + 5, footY - 13, 3, 9)
    set_fill("#ffd93d"); px(x - 3, footY - 12, 6, 2)
    set_fill("#333"); px(x - 3, footY - 13, 2, 2); px(x + 1, footY - 13, 2, 2)
HAND = {
    "goblin": drawGoblin, "slime": drawSlime, "bat": drawBat, "skel": drawSkel,
    "magma": drawMagma, "sala": drawSala, "wolf": drawWolf, "frost": drawFrost,
    "golem": drawGolem, "ghost": drawGhost, "cat": drawCat,
}

# 39 种参数化怪物 art 配置（与 HTML MORE_MOBS 完全一致）
ART = {
    "hornet":  {"shape": "float", "c": "#ffd93d", "c2": "#b8860b", "wing": 1, "tail": 1, "eye": "#2a1a3a"},
    "mush":    {"shape": "blob",  "c": "#e07a5f", "c2": "#8a3b2a", "ear": 0, "horn": 0, "eye": "#2a1a3a"},
    "boar":    {"shape": "quad",  "c": "#8a6248", "c2": "#4a3222", "horn": 2, "tail": 1, "eye": "#3a1a1a"},
    "treant":  {"shape": "biped", "c": "#5a8a4a", "c2": "#2e5228", "horn": 1, "eye": "#ffd93d"},
    "frog":    {"shape": "blob",  "c": "#6ab04c", "c2": "#38682a", "eye": "#ffd93d"},
    "deer":    {"shape": "quad",  "c": "#c8a878", "c2": "#7a5f3a", "horn": 1, "tail": 1, "eye": "#2a1a3a"},
    "snail":   {"shape": "blob",  "c": "#a8b8d8", "c2": "#5a6a8a", "horn": 2, "eye": "#2a1a3a"},
    "fox":     {"shape": "quad",  "c": "#ff9a5a", "c2": "#c85a2a", "ear": 2, "tail": 1, "eye": "#ffd93d"},
    "spider":  {"shape": "blob",  "c": "#4a4a6a", "c2": "#22224a", "eye": "#ff5555", "horn": 2},
    "kobold":  {"shape": "biped", "c": "#b8824a", "c2": "#6a4a26", "ear": 2, "eye": "#ffd93d"},
    "ghoul":   {"shape": "biped", "c": "#7a8a6a", "c2": "#3a4a34", "eye": "#7cfc9b", "horn": 1},
    "ooze":    {"shape": "blob",  "c": "#7a9a3a", "c2": "#3e5220", "eye": "#c8ff5a"},
    "glowfish":{"shape": "float", "c": "#5ec8f2", "c2": "#2a6a8a", "eye": "#fff3b0", "tail": 1},
    "stal":    {"shape": "biped", "c": "#8a8aa0", "c2": "#4a4a5e", "horn": 2, "eye": "#7fd4ff"},
    "weasel":  {"shape": "quad",  "c": "#c8b8a0", "c2": "#6a5a48", "ear": 1, "tail": 1, "eye": "#2a1a3a"},
    "shade":   {"shape": "float", "c": "#3a3050", "c2": "#1a1428", "eye": "#b06bff"},
    "imp":     {"shape": "biped", "c": "#e05545", "c2": "#8a2418", "horn": 2, "wing": 1, "tail": 1, "eye": "#ffd93d"},
    "cinder":  {"shape": "float", "c": "#4a3a3a", "c2": "#1e1414", "wing": 1, "tail": 1, "eye": "#ff8a3d"},
    "lavadog": {"shape": "quad",  "c": "#e0722a", "c2": "#8a3208", "horn": 1, "tail": 1, "eye": "#ffe14d"},
    "obsid":   {"shape": "biped", "c": "#2a2a34", "c2": "#101018", "horn": 2, "eye": "#ff6b3d"},
    "phoenix": {"shape": "float", "c": "#ffb03d", "c2": "#c85a10", "wing": 1, "tail": 1, "eye": "#fff3b0"},
    "geo":     {"shape": "blob",  "c": "#c85a2a", "c2": "#6a2a10", "eye": "#ffe14d"},
    "ashwisp": {"shape": "float", "c": "#9a8a8a", "c2": "#4a3e3e", "eye": "#ffb03d"},
    "slag":    {"shape": "quad",  "c": "#6a4a3a", "c2": "#2e1c14", "horn": 2, "eye": "#ff6b3d"},
    "yeti":    {"shape": "biped", "c": "#e8f0f8", "c2": "#8aa8c8", "horn": 0, "eye": "#4db8ff"},
    "icefox":  {"shape": "quad",  "c": "#c8e8ff", "c2": "#6a9ac8", "ear": 2, "tail": 1, "eye": "#4db8ff"},
    "peng":    {"shape": "biped", "c": "#e8ecf4", "c2": "#3a4258", "eye": "#ffd93d"},
    "hail":    {"shape": "float", "c": "#aee7ff", "c2": "#5a9ac8", "eye": "#fff", "tail": 1},
    "mam":     {"shape": "quad",  "c": "#a8886a", "c2": "#5a4232", "horn": 2, "tail": 1, "eye": "#2a1a3a"},
    "owl":     {"shape": "float", "c": "#e8e8f4", "c2": "#8a8aa8", "ear": 2, "eye": "#ffd93d", "wing": 1},
    "sealf":   {"shape": "blob",  "c": "#d8e8f4", "c2": "#7a9ab8", "eye": "#2a1a3a", "horn": 0},
    "banshee": {"shape": "float", "c": "#b8d8e8", "c2": "#5a7a92", "eye": "#fff3b0", "wing": 1},
    "rune":    {"shape": "biped", "c": "#7a9ac8", "c2": "#3a4a6a", "horn": 1, "eye": "#7fd4ff"},
    "mimic":   {"shape": "blob",  "c": "#a8794a", "c2": "#5a3a1f", "eye": "#ffd93d", "horn": 1},
    "eyemon":  {"shape": "float", "c": "#b06bff", "c2": "#5a2a8a", "eye": "#fff3b0", "wing": 1},
    "wraith":  {"shape": "biped", "c": "#6a7a9a", "c2": "#2e3a52", "horn": 2, "eye": "#7fd4ff"},
    "sentinel":{"shape": "biped", "c": "#c8c8d8", "c2": "#6a6a80", "horn": 1, "eye": "#ffd93d"},
    "drake":   {"shape": "quad",  "c": "#5ab84a", "c2": "#2a6a2a", "horn": 2, "wing": 1, "tail": 1, "eye": "#ffd93d"},
    "sphinx":  {"shape": "quad",  "c": "#d8b86a", "c2": "#8a6a2a", "horn": 1, "wing": 1, "eye": "#fff3b0"},
}

BOSSES = ["goblin", "skel", "magma", "wolf", "golem"]


def render_hand(name, fn, s_global, out_path):
    img = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    global _draw
    _draw = ImageDraw.Draw(img)
    G["scale"] = s_global
    G["ox"] = CX
    G["oy"] = FOOTY
    G["alpha"] = 1.0
    G["_sGlobal"] = s_global
    fn(0, 0, 0)
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    img.save(out_path)
    return img.size


def render_param(name, art, s_global, out_path):
    img = Image.new("RGBA", (CW, CH), (0, 0, 0, 0))
    global _draw
    _draw = ImageDraw.Draw(img)
    G["scale"] = s_global
    G["ox"] = CX
    G["oy"] = FOOTY
    G["alpha"] = 1.0
    G["_sGlobal"] = s_global
    drawMobArt_body(art)
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    img.save(out_path)
    return img.size


def main():
    os.makedirs(OUT, exist_ok=True)
    total = 0
    for name, fn in HAND.items():
        p = os.path.join(OUT, "sp_%s.png" % name)
        sz = render_hand(name, fn, 1.0, p)
        print("hand  %-10s %sx%s" % (name, sz[0], sz[1]))
        total += 1
    for name, art in ART.items():
        p = os.path.join(OUT, "sp_%s.png" % name)
        sz = render_param(name, art, 1.0, p)
        print("param %-10s %sx%s" % (name, sz[0], sz[1]))
        total += 1
    # BOSS：2.1x
    for b in BOSSES:
        if b in HAND:
            fn = HAND[b]
            p = os.path.join(OUT, "sp_%s_boss.png" % b)
            sz = render_hand(b, fn, 2.1, p)
            print("BOSS  %-10s %sx%s" % (b, sz[0], sz[1]))
            total += 1
        elif b in ART:
            p = os.path.join(OUT, "sp_%s_boss.png" % b)
            sz = render_param(b, ART[b], 2.1, p)
            print("BOSS  %-10s %sx%s" % (b, sz[0], sz[1]))
            total += 1
        else:
            print("!! 未找到 BOSS 类型 %s" % b)
    # 接触表（便于肉眼核对）
    names = list(HAND.keys()) + list(ART.keys()) + ["%s_boss" % b for b in BOSSES]
    cols = 8
    cell = 80
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell + 10, rows * cell + 10), (20, 16, 36, 255))
    sd = ImageDraw.Draw(sheet)
    for i, nm in enumerate(names):
        r = i // cols; c = i % cols
        sp = os.path.join(OUT, "sp_%s.png" % nm)
        if not os.path.exists(sp):
            continue
        im = Image.open(sp).convert("RGBA")
        # 放大到 cell 内显示
        im.thumbnail((cell - 6, cell - 6), Image.LANCZOS)
        sheet.paste(im, (c * cell + 5 + (cell - 6 - im.width) // 2,
                         r * cell + 5 + (cell - 6 - im.height) // 2), im)
        sd.text((c * cell + 5, r * cell + cell - 12), nm, fill=(230, 226, 255, 255))
    sheet.save(os.path.join(OUT, "_contact_sheet.png"))
    print("\n共生成 %d 张贴图，接触表：%s" % (total, os.path.join(OUT, "_contact_sheet.png")))


if __name__ == "__main__":
    main()
