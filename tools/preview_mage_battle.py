#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_mage_battle.py —— 在电脑上精确渲染「固件界面」预期效果图。

目的（硬性流程约束）：任何界面改动，先出这张图给作者与 HTML 原版截图对比，
作者确认后才允许烧录。不跑硬件、不跑 LVGL，直接用 main/mage_hud.c 与
main/mage_fx.c / main/mage.c 里**同一套坐标常量**画出来，所以"预览长什么样，
实机就长什么样"。

本次扩展（对应 5 类问题的修复核对）：
  问题① 战斗时陷阱/宝箱环节没展示   -> 事件面板场景（金边 box + sp_event_* 图标 + 名称 + "C 绕开"）
  问题② 人物怪物有黑色底图          -> BOSS/怪物贴图用真透明（RGB565A8），预览里 alpha 合成无黑框
  问题③ 最终 boss 怪物不对          -> 敌方立绘按 ENEMY[type] 查 sp_<type>_boss，预览用 goblin_boss 大图
  问题④ 回城文字消失啥也不显示        -> 城镇场景：g_scene_art 沉底，文字压其上可见
  问题⑤ 战斗文字乱码/错版/缺失       -> HUD 全中文用 mage_cjk_16 子集字库渲染（AUTO/手动 编 球 卡 药 回合）

产出（都在 assets/preview/）：
  mage_battle_boss_240x320.png   战斗·BOSS 立绘（问题③②⑤）
  mage_event_240x320.png         事件面板·宝箱（问题①）
  mage_town_240x320.png          城镇界面·文字可见（问题④）
  mage_hud_240x320.png           战斗·HUD 中文（问题⑤）
  mage_fixes_overview_480x640.png 四宫格汇总对照（一次看全 5 类修复）
"""
from __future__ import annotations

import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "mage-game", "assets")
OUT = os.path.join(ROOT, "assets", "preview")

# ---- 源码常量（与 main/mage_hud.c / main/mage_fx.c / main/mage.c 保持一致）----
W, H = 240, 320
HUD_H = 26
CARD_Y, CARD_W, CARD_H = H - 44, 72, 40
GROUND_Y = 250


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


F8, F9, F10, F12, F13, F16 = (font(s) for s in (8, 9, 10, 12, 13, 16))


def cover(img_path, w, h):
    """严格复刻 HTML drawBattle 的背景取窗：cover 铺满（整屏原生尺寸）。"""
    im = Image.open(img_path).convert("RGB")
    sw, sh = im.size
    win = int(round(sh * (w / h)))
    if win > sw:
        win = sw
    left = (sw - win) // 2
    return im.crop((left, 0, left + win, sh)).resize((w, h), Image.LANCZOS)


def load_png(name, alpha=True):
    p = os.path.join(ASSETS, name)
    if not os.path.exists(p):
        return None
    return Image.open(p).convert("RGBA") if alpha else Image.open(p).convert("RGB")


def draw_sprite(im, png_name, cx, foot_y, disp_h=None):
    """脚底中心锚点、等比（disp_h=None 表示用原始尺寸，等价于固件 REAL 尺寸模式）。"""
    s = load_png(png_name)
    if s is None:
        return
    if disp_h is None:
        w, h = s.size
    else:
        ratio = s.height / s.width
        w = max(1, int(round(disp_h / ratio)))
        s = s.resize((w, disp_h), Image.LANCZOS)
    # alpha 合成（真透明，无黑框）—— 对应问题②根因修复
    im.paste(s, (int(cx - w / 2), int(foot_y - s.height)), s)


def txt_c(d, text, cx, y, fnt, fill):
    tw = d.textlength(text, font=fnt)
    d.text((cx - tw / 2, y), text, font=fnt, fill=fill)


def txt_r(d, text, rx, y, fnt, fill):
    tw = d.textlength(text, font=fnt)
    d.text((rx - tw, y), text, font=fnt, fill=fill)


def txt_l(d, text, x, y, fnt, fill):
    d.text((x, y), text, font=fnt, fill=fill)


# ---- 素材映射：与 tools/gen_mage_sprites.py 完全一致（预览=实机的前提）----
#   sp_hero   <- sp_hero_k.png   36x46 (RGB565A8)
#   cat_partner <- sp_cat_k.png  29x32 (RGB565A8)
#   battle_bg <- battle_bg_k.png 240x320 cover
#   town      <- town_art.png    240x320 直接 resize（非 cover，与固件 load_rgba 一致）
#   怪物      <- sp_*.png（非 _k）自然尺寸（REAL 尺寸模式）
SRC_HERO = "sp_hero_k.png"
SRC_CAT = "sp_cat_k.png"
SRC_BATTLE_BG = "battle_bg_k.png"
SRC_TOWN = "town_art.png"
HERO_W, HERO_H = 36, 46
CAT_W, CAT_H = 29, 32


# ============ 战斗背景 + 压暗 + 地面（所有战斗场景共用）============
def battle_base():
    im = Image.new("RGB", (W, H), (0x15, 0x10, 0x2a))
    d = ImageDraw.Draw(im)
    bg = os.path.join(ASSETS, SRC_BATTLE_BG)
    if os.path.exists(bg):
        im.paste(cover(bg, W, H), (0, 0))
    d.rectangle([0, GROUND_Y, W, GROUND_Y + 2], fill=(0x5c, 0x8f, 0x42))
    # 活动带压暗：HTML 的 gDim 渐变，用 alpha 混合（不能 fill 纯色）
    dim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dim)
    for y in range(140, min(H, 271)):
        t = (y - 140) / 130.0
        a = int(70 * (1 - t))
        dd.rectangle([0, y, W, y], fill=(6, 5, 16, max(0, min(70, a))))
    im = Image.alpha_composite(im.convert("RGBA"), dim).convert("RGB")
    return im, ImageDraw.Draw(im)


# ============ 顶部 HUD（与 mage_hud.c 1:1）============
def draw_hud(d, wave="1/10", stat="AUTO·3·1·2", turn="回合 1",
             hp="80/80", mp="50/50"):
    d.rectangle([0, 0, W, HUD_H], fill=(0x10, 0x0c, 0x1a), outline=(0x6a, 0x5a, 0x3a))
    txt_l(d, "HP", 7, 3, F8, (0xff, 0x88, 0x88))
    d.rectangle([20, 4, 78, 11], fill=(0x3a, 0x1a, 0x1a))
    d.rectangle([20, 4, 78, 11], fill=(0xe0, 0x55, 0x55))
    txt_l(d, hp, 82, 2, F8, (0xff, 0xff, 0xff))
    txt_l(d, "MP", 7, 14, F8, (0x7c, 0xb8, 0xff))
    d.rectangle([20, 15, 78, 20], fill=(0x1a, 0x2a, 0x3a))
    d.rectangle([20, 15, 78, 20], fill=(0x5e, 0xc8, 0xf2))
    txt_l(d, mp, 82, 13, F8, (0x5e, 0xc8, 0xf2))
    txt_r(d, wave, W - 6, 1, F10, (0xff, 0xd9, 0x3d))      # 波次（含 BOSS/精英）
    txt_r(d, stat, W - 6, 13, F8, (0x7f, 0xdc, 0x8a))      # AUTO/手动 编 球 卡 药


# ============ 底部三张动作卡 + 回合徽标（与 mage_hud.c 1:1）============
# 用户问题⑨：每个功能配对应图标，取代原 A/B/C 字母徽章。
CARD_ICON = ["sp_icon_sword.png", "sp_icon_staff.png", "sp_icon_potion.png"]
CARD_NAME = ["攻击", "魔法", "道具"]
CARD_C0   = [(0xff, 0xb7, 0x4d), (0x7c, 0xd4, 0xff), (0x8f, 0xe8, 0x9a)]
CARD_C1   = [(0xff, 0x8a, 0x3d), (0x5a, 0x8a, 0xff), (0x3d, 0xbd, 0x7a)]


def draw_cards(im, d, turn="回合 1"):
    for i in range(3):
        x = 8 + i * 78
        d.rectangle([x, CARD_Y, x + CARD_W, CARD_Y + CARD_H], fill=(0x24, 0x1d, 0x4a),
                    outline=(0x8a, 0x6a, 0x2a))
        d.rectangle([x + 2, CARD_Y + 3, x + 5, CARD_Y + 37], fill=CARD_C1[i])
        d.rectangle([x + 2, CARD_Y + 3, x + 5, CARD_Y + 8], fill=CARD_C0[i])
        # 功能图标 24x24 居中靠上（对齐 mage_hud.c：align TOP_MID, +2）
        s = load_png(CARD_ICON[i])
        if s is not None:
            s = s.resize((24, 24), Image.LANCZOS)
            im.paste(s, (x + CARD_W // 2 - 12, CARD_Y + 2), s)
        # 图标下方功能名（12px 居中）
        txt_c(d, CARD_NAME[i], x + CARD_W // 2, CARD_Y + 28, F12, (0xff, 0xff, 0xff))
    # 回合徽标（固件 fix②：移到屏幕上方紧挨顶部 HUD 之下，不再贴着底部动作卡）
    lb = turn
    lw = len(lb) * 6 + 14
    bx = W // 2 - lw // 2
    d.rectangle([bx, 28, bx + lw, 42], fill=(0x0d, 0x0a, 0x1a),
                outline=(0xc9, 0xa4, 0x4a))
    txt_c(d, lb, W / 2, 30, F8, (0xff, 0xd9, 0x3d))


# ============ 场景一：战斗 · BOSS 立绘（问题③②⑤）============
def scene_boss():
    im, d = battle_base()
    # 普通伙伴猫（cat_partner 29x32 <- sp_cat_k，HTML psx=44）
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    # BOSS：sp_goblin_boss 按真实尺寸（REAL 尺寸模式，2.1x 大怪），居中脚下 y=250
    draw_sprite(im, "sp_goblin_boss.png", 120, 250)   # 真透明 -> 无黑框（问题②）
    # BOSS 血条 + Lv（HTML bar(sx-12,sy-26,24,3) + txt Lv）
    d.rectangle([120 - 24, GROUND_Y - 50, 120 + 24, GROUND_Y - 46], fill=(0x33, 0x1a, 0x1a))
    d.rectangle([120 - 24, GROUND_Y - 50, 120 + 4, GROUND_Y - 46], fill=(0xe0, 0x55, 0x55))
    txt_l(d, "Lv10", 120 + 14, GROUND_Y - 60, F8, (0xff, 0xd9, 0x3d))   # 精英/BOSS 金色
    # 主角（sp_hero 36x46 <- sp_hero_k，x=70）
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    # 伤害飘字（白字打 BOSS，红字主角受击）
    txt_c(d, "42", 120, 250 - 60, F10, (0xff, 0xff, 0xff))
    txt_c(d, "-5", 70, 200, F10, (0xff, 0x6b, 0x6b))
    # 命中特效（用户问题⑧）：英雄突进命中敌人，特效在【敌人身上】炸开（RING + PARTICLE）
    d.ellipse([120 - 16, 212 - 16, 120 + 16, 212 + 16], outline=(0xff, 0xe2, 0x7a), width=3)
    for ang in range(0, 360, 45):
        import math
        px = 120 + int(14 * math.cos(math.radians(ang)))
        py = 212 + int(14 * math.sin(math.radians(ang)))
        d.ellipse([px - 3, py - 3, px + 3, py + 3], fill=(0xff, 0xf0, 0xc0))
    draw_hud(d, wave="10/10 BOSS", stat="手动·1·0·1", turn="回合 3")
    draw_cards(im, d, turn="回合 3")
    return im


# ============ 场景二：事件面板 · 宝箱（问题①）============
EVENTS = {
    "chest":  ("sp_event_chest.png",  "闪闪发光的宝箱", "A 打开宝箱"),
    "trap":   ("sp_event_trap.png",   "草丛下的陷阱！", "A 小心通过"),
    "spring": ("sp_event_spring.png", "清澈的泉水",     "A 饮水休息"),
    "herb":   ("sp_event_herb.png",   "一丛稀有药草",   "A 采集"),
}


def scene_event(kind="chest"):
    im, d = battle_base()
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    draw_sprite(im, "sp_slime.png", 148, 250)   # 自然尺寸（REAL 尺寸模式）
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    draw_hud(d)
    draw_cards(im, d)
    # 事件面板（1:1 复刻 HTML drawEventPanel + mage_fx.c 坐标）
    icon, name, desc = EVENTS[kind]
    bx, by, bw, bh = 20, 120, 200, 110
    # 金边面板底（bg 0x0c091a ~95% 不透明）
    box = Image.new("RGBA", (bw, bh), (0x0c, 0x09, 0x1a, 242))
    bd = ImageDraw.Draw(box)
    bd.rectangle([0, 0, bw - 1, bh - 1], outline=(0xff, 0xd9, 0x3d), width=1)
    # 56x56 图标，水平居中（(bw/2-28, 6)）
    ic = load_png(icon)
    if ic:
        box.paste(ic, (bw // 2 - 28, 6), ic)
    idr = ImageDraw.Draw(box)
    txt_c(idr, name, bw / 2, 56, F12, (0xff, 0xff, 0xff))
    txt_c(idr, desc, bw / 2, 72, F10, (0xff, 0xd9, 0x3d))
    txt_c(idr, "C 绕开", bw / 2, 86, F9, (0x9a, 0x93, 0xb5))
    im.paste(box, (bx, by), box)
    return im


# ============ 场景三：城镇界面 · 文字可见（问题④）============
LOCATIONS = ["魔法学院", "野外试炼", "冒险者公会", "住宅",
             "锻造房", "魔法黑市", "魔物图鉴"]


def scene_town(sel=0):
    """魔法小镇主菜单：与固件一致，frameBox 金框 + 地点卡片列表（每项带功能图标）。
    对应固件 SC_CITY（is_frame_scene=true -> g_list 卡片列表），修复问题①「图标空白」。"""
    im = _town_dim_base(153)
    items = [
        ("sp_icon_book.png",   "魔法学院",   "学魔法 · 晋升考核",   ""),
        ("sp_icon_map.png",    "野外试炼",   "章节 · 遗迹秘境",     ""),
        ("sp_icon_sword.png",  "竞技场",     "试炼塔连战",         ""),
        ("sp_icon_bed.png",    "住宅",       "休息 · 锻造",        ""),
        ("sp_icon_hammer.png", "锻造房",     "武器 · 法袍",        ""),
        ("sp_icon_coin.png",   "魔法黑市",   "材料交易",           ""),
        ("sp_icon_staff.png",  "冒险者公会", "讨伐委托",           ""),
    ]
    return draw_menu_cards(im, "魔法小镇", items, sel, "↑↓ 选地点 · OK 进入 · 长按 功能栏")


# ============ 场景四：战斗 · HUD 中文（问题⑤）============
def scene_hud():
    im, d = battle_base()
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    draw_sprite(im, "sp_slime.png", 148, 250)   # 自然尺寸（REAL 尺寸模式）
    draw_sprite(im, "sp_slime.png", 164, 250)
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    # 敌人血条/Lv（演示 HUD 不丢中文）
    d.rectangle([148 - 12, GROUND_Y - 26, 148 + 12, GROUND_Y - 23], fill=(0x33, 0x1a, 0x1a))
    d.rectangle([148 - 12, GROUND_Y - 26, 148 + 6, GROUND_Y - 23], fill=(0xe0, 0x55, 0x55))
    txt_l(d, "Lv2", 148 + 13, GROUND_Y - 35, F8, (0xc9, 0xb8, 0xee))
    # 伤害飘字
    txt_c(d, "18", 148, 250 - 34, F10, (0xff, 0xff, 0xff))
    txt_c(d, "-3", 70, 200, F10, (0xff, 0x6b, 0x6b))
    # HUD：AUTO/手动 + 编 球 卡 药 + 回合金色徽标
    draw_hud(d, wave="5/10 精英", stat="手动·3·1·2", turn="回合 1")
    draw_cards(im, d)
    return im


# ============ 结算/战利品面板（bug⑥：1:1 复刻 HTML 三个版式 + mage_fx.c 坐标）============
def _panel_base(w, h, bg=(0x0a, 0x08, 0x18), border=1, sides="full"):
    """金边面板底：sides='full' 四边 border px；'tb' 上下各 border px 金条（expo）。"""
    box = Image.new("RGBA", (w, h), bg + (242,))
    bd = ImageDraw.Draw(box)
    c = (0xff, 0xd9, 0x3d)
    if sides == "full":
        bd.rectangle([0, 0, w - 1, h - 1], outline=c, width=border)
    else:
        bd.rectangle([0, 0, w, border], fill=c)
        bd.rectangle([0, h - border, w, h], fill=c)
    return box, bd


def scene_result():
    """每波清场结算（HTML drawResult / mage_fx_result）：
    box(20,150,200,150)，标题金 rel y=8，明细 #ccffcc rel y=36 步进16，提示金 rel y=126。"""
    im, d = battle_base()
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    draw_hud(d, wave="3/10", stat="AUTO·3·1·2")
    bw, bh = 200, 170
    box, bd = _panel_base(bw, bh)
    txt_c(bd, "第 3 波 清场（累计收益）", bw / 2, 8, F16, (0xff, 0xd9, 0x3d))
    y = 28
    for ln in ["金币 +120", "经验 +80", "月光草 +2", "兽骨 +1", "药水 +1"]:
        txt_c(bd, ln, bw / 2, y, F16, (0xcc, 0xff, 0xcc))
        y += 16
    txt_c(bd, "A 继续前进 · B 回城", bw / 2, 150, F16, (0xc9, 0xa2, 0x27))
    im.paste(box, (20, 150), box)
    return im


def scene_expo():
    """远征/BOSS 汇总（HTML drawExpoSummary / mage_fx_expo）：
    box(14,40,212,250) 上下2px金条，✦标题 y=8、副题 y=34、名目左/数值右 y=54 步进16、
    ★ BOSS 战利品块 y=154、提示 y=228。"""
    im, d = battle_base()
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    draw_hud(d, wave="10/10 BOSS", stat="AUTO·3·1·2")
    bw, bh = 212, 250
    box, bd = _panel_base(bw, bh, border=2, sides="tb")
    txt_c(bd, "★ 远征胜利 ★", bw / 2, 8, F16, (0xff, 0xd9, 0x3d))
    txt_c(bd, "远征森林 已肃清", bw / 2, 34, F16, (0xc9, 0xa2, 0x27))
    rowsL = ["总击杀", "金币", "经验", "月光草", "兽骨", "药水"]
    rowsR = ["8 只", "+640", "+420", "+3", "+2", "+1"]
    y = 54
    for l, r in zip(rowsL, rowsR):
        txt_l(bd, l, 16, y, F16, (0xbb, 0xbb, 0xbb))
        txt_r(bd, r, 196, y, F16, (0xcc, 0xff, 0xcc))
        y += 16
    y += 4
    for ln in ["★ BOSS 战利品", "炎之心片", "魔晶+2 捕捉球+2 封印卡+1"]:
        txt_c(bd, ln, bw / 2, y, F16, (0xff, 0xd9, 0x3d))
        y += 16
    txt_c(bd, "A 继续 · B 回城", bw / 2, 228, F16, (0xc9, 0xa2, 0x27))
    im.paste(box, (14, 40), box)
    return im


def scene_loot(kind="chest"):
    """宝箱/采集战利品（HTML lootShow / mage_fx_loot）：顶部小面板 box(28,54,W-56)，
    高 26+n*16+8，标题 #ffe9b0、明细白、无底部提示。"""
    im, d = battle_base()
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    draw_hud(d, wave="3/10", stat="手动·3·1·2")
    if kind == "chest":
        title = "★ 宝箱战利品 ★"
        lines = ["金币 +50", "药品·经验药水", "素材·月光草", "中级魔石 ×2", "捕捉球 ×2"]
    else:
        title = "★ 采集收获 ★"
        lines = ["月光草 ×5", "魔晶 ×1", "素材·银矿石"]
    n = len(lines)
    bw, bh = W - 56, 26 + n * 20 + 8
    box, bd = _panel_base(bw, bh, bg=(0x0e, 0x0a, 0x1e))
    txt_c(bd, title, bw / 2, 6, F16, (0xff, 0xe9, 0xb0))
    y = 26
    for ln in lines:
        txt_c(bd, ln, bw / 2, y, F16, (0xff, 0xff, 0xff))
        y += 20
    im.paste(box, (28, 54), box)
    return im


# ============ 城镇子面板/功能面板背景（新反馈①③：进入板块后没背景图）============
GOLD = (0xff, 0xd9, 0x3d)
GOLD2 = (0xc9, 0xa2, 0x27)
DIM = (0xc9, 0xb8, 0xee)
WHITE = (0xff, 0xff, 0xff)


def _town_dim_base(dim_opa=179):
    """固件城镇簇底图：town_art 直接 resize + C_BG(0x0d0b1a) 压暗层。
    dim_opa: LVGL opa 值（179=LV_OPA_70 子面板 / 153=LV_OPA_60 城镇主页）。"""
    im = Image.new("RGB", (W, H), (0x12, 0x0e, 0x22))
    bg = os.path.join(ASSETS, SRC_TOWN)
    if os.path.exists(bg):
        im.paste(Image.open(bg).convert("RGB").resize((W, H), Image.LANCZOS), (0, 0))
    dim = Image.new("RGBA", (W, H), (0x0d, 0x0b, 0x1a, dim_opa))
    return Image.alpha_composite(im.convert("RGBA"), dim).convert("RGB")


def frame_box(im, title):
    """HTML frameBox(10,10,W-20,H-50,title) 1:1，与固件 build_ui 的金框组一致：
    #1d1636 底 + c9a227 框 + 上下 2px 金沿(ffd93d) + #2a2348 标题条 + 金色标题字。"""
    d = ImageDraw.Draw(im)
    d.rectangle([10, 10, 229, 279], fill=(0x1d, 0x16, 0x36), outline=GOLD2)
    d.rectangle([10, 10, 229, 11], fill=GOLD)          # 顶 2px 金沿
    d.rectangle([10, 278, 229, 279], fill=GOLD)        # 底 2px 金沿
    d.rectangle([10, 9, 229, 24], fill=(0x2a, 0x23, 0x48), outline=GOLD2)
    txt_l(d, title, 18, 12, F12, GOLD)


def frame_hint(im, text):
    txt_c(ImageDraw.Draw(im), text, W / 2, H - 22, F12, DIM)


def draw_menu_cards(im, title, items, sel, hint, hdr=None):
    """菜单卡片列表（魔法学院/野外试炼/冒险者公会）：金框 + 每条(图标+名称+副文+右标)。
    1:1 复刻固件 g_list 卡片布局（替代原单 label 16px 中文无断行点冲框方案）。"""
    frame_box(im, title)
    d = ImageDraw.Draw(im)
    y0 = 30
    if hdr:
        txt_l(d, hdr, 18, y0, F12, GOLD)
        y0 += 14
    for i, (icon, name, sub, rgt) in enumerate(items):
        bx, by = 18, y0 + i * 30
        selp = (i == sel)
        d.rectangle([bx, by, bx + 204, by + 29],
                    fill=(0x3a, 0x2c, 0x5c) if selp else (0x18, 0x12, 0x2c),
                    outline=GOLD if selp else (0x9a, 0x93, 0xb5))
        if selp:
            d.rectangle([bx, by, bx + 204, by + 1], fill=GOLD)   # 选中顶亮条
        ic = load_png(icon)
        if ic:
            ic = ic.resize((22, 22), Image.LANCZOS)
            im.paste(ic, (bx + 4, by + 3), ic)
        txt_l(d, name, bx + 32, by + 3, F12, WHITE)
        txt_l(d, sub, bx + 32, by + 16, F12, DIM)
        txt_r(d, rgt, bx + 204 - 6, by + 9, F12, GOLD if selp else DIM)
    frame_hint(im, hint)
    return im


def scene_town_sub(sel=0):
    """魔法学院（卡片列表）：图标 + 名称 + 副文（替代原单 label 16px 冲框）。"""
    im = _town_dim_base(179)
    items = [
        ("sp_icon_orb.png", "魔力修行", "魔力上限 +5（当前 80）", "20金"),
        ("sp_icon_book.png", "学属性魔法", "已学 0/135 种魔法", "45金"),
        ("sp_icon_crown.png", "职业晋升考核", "需击杀/委托达标", ""),
    ]
    return draw_menu_cards(im, "魔法学院", items, sel, "OK 确定 · ↓ 切换 · 长按 返回")


def scene_town_wild(sel=0):
    """野外试炼（卡片列表）：章节 + 遗迹秘境 + 随机副本。"""
    im = _town_dim_base(179)
    items = [
        ("sp_icon_map.png", "远征森林", "野外区域 · 可挑战", "A 出发"),
        ("sp_icon_map.png", "迷雾沼泽", "野外区域 · 可挑战", "A 出发"),
        ("sp_icon_map.png", "熔岩洞窟", "先击败上一章BOSS", "锁"),
        ("sp_icon_map.png", "寒霜雪原", "野外区域 · 可挑战", "A 出发"),
        ("sp_icon_gate.png", "遗迹秘境", "精英连战 3 层", "A 进入"),
    ]
    return draw_menu_cards(im, "野外试炼", items, sel, "OK 出发/进入 · ↓ 切换 · 长按 返回")


def scene_town_guild(sel=0):
    """冒险者公会（卡片列表）：委托 + 进度。"""
    im = _town_dim_base(179)
    items = [
        ("sp_icon_sword.png", "讨伐委托 · 初级", "击杀 8 只魔物 · 40金", "A 接取"),
        ("sp_icon_herb.png", "采药委托", "交付 3 株月光草 · 35金", "未解锁"),
        ("sp_icon_sword.png", "讨伐委托 · 中级", "击杀 20 只魔物 · 120金", "A 接取"),
        ("sp_icon_sword.png", "深渊侦查 · 上级", "击杀 35 只魔物 · 260金", "A 接取"),
    ]
    return draw_menu_cards(im, "冒险者公会", items, sel, "OK 接取/交付 · ↓ 切换 · 长按 返回",
                          hdr="冒险者 F级 · 完成 0 单")


def scene_func_status():
    """功能面板·角色（frameBox 详情页）：金框 + sp_hero 立绘 foot(120,88) +
    12px 居中正文从 y=98 起（与固件一致）。"""
    im = _town_dim_base(179)
    frame_box(im, "角色")
    draw_sprite(im, SRC_HERO, 120, 88, HERO_H)   # 固件注册尺寸 36x46（漏传会按 349x448 原图画出巨脚）
    d = ImageDraw.Draw(im)
    rows = ["见习魔法师", "魔法学徒 · Lv1", "经验 0/27", "生命 80/80", "魔力 50/50",
            "攻击 7（装+0）", "金币 0 · 击杀 0", "伙伴 0 只"]
    y = 110
    for r in rows:
        txt_c(d, r, W / 2, y, F12, WHITE)
        y += 17
    frame_hint(im, "OK/长按 回城 · ↓ 管理装备")
    return im


def scene_func_bag():
    """功能面板·背包（frameBox 列表页）：金框 + 12px 列表。"""
    im = _town_dim_base(179)
    frame_box(im, "背包")
    d = ImageDraw.Draw(im)
    rows = ["分类：药品", "[ 小恢复药水 ]", "数量 ×3", "已收集 药1/20 材0/40", "(1 / 1)"]
    y = 40
    for r in rows:
        txt_l(d, r, 18, y, F12, WHITE)
        y += 24
    frame_hint(im, "OK/↓ 切换 · 长按 返回")
    return im


# ============ 本轮扩展：其余功能面板全部改为「图标卡片列表」（用户要求每个板块都有设计图标）============
def scene_func_menu(sel=0):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_bolt.png",   "自动战斗", "开",            "▶"),
        ("sp_icon_paw.png",    "自动捕捉", "关",            ""),
        ("sp_icon_star.png",   "音效",     "开",            ""),
        ("sp_icon_crown.png",  "难度",     "普通",          ""),
        ("sp_icon_scroll.png", "导出存档", "",              ""),
        ("sp_icon_scroll.png", "导入存档", "",              ""),
        ("sp_icon_gear.png",   "重新开始", "",              ""),
    ]
    return draw_menu_cards(im, "设置", items, sel, "OK 执行 · ↓ 切换 · 长按 返回")

def scene_func_home(sel=0):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_bed.png",   "休息",     "恢复全部状态",       "▶"),
        ("sp_icon_flask.png", "炼金台",   "炼制药剂与捕捉球",   ""),
        ("sp_icon_book.png",  "魔法研究", "消耗材料升魔法等级", ""),
    ]
    return draw_menu_cards(im, "住宅", items, sel, "OK 选择 · ↓ 切换 · 长按 返回")

def scene_func_research(sel=1):
    """魔法研究（法术列表）：与固件一致，每行按 spellSplit 的元素 e 用对应属性图标
    （火/冰/雷/光…），不再千篇一律书本图标（修复问题④「用属性图标！」）。"""
    im = _town_dim_base(179)
    items = [
        ("sp_icon_fire.png",    "火球",     "基础 Lv2",         ""),
        ("sp_icon_ice.png",     "冰锥",     "基础 Lv2 成60%",   "▶"),
        ("sp_icon_thunder.png", "雷击",     "基础 Lv1",         ""),
        ("sp_icon_light.png",   "治愈微风", "初级 Lv1",         ""),
    ]
    return draw_menu_cards(im, "魔法研究", items, sel, "OK 研究 · ↓ 切换 · 长按 返回住宅")

def scene_func_forge(sel=0):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_sword.png", "锻造武器", "3级 · 120金+兽骨x6", "▶"),
        ("sp_icon_robe.png",  "锻造法袍", "2级 · 90金+兽骨x4",  "生命+8"),
    ]
    return draw_menu_cards(im, "锻造房", items, sel, "OK 锻造 · ↓ 切换 · 长按 返回")

def scene_func_market(sel=0):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_herb.png",    "月光草",       "持有 12 · 20金",  "▶"),
        ("sp_icon_bone.png",    "兽骨",         "持有 8 · 15金",   ""),
        ("sp_icon_crystal.png", "魔晶",         "持有 3 · 30金",   ""),
        ("sp_icon_seal.png",    "魔法封印卡",   "持有 1 · 50金",   ""),
        ("sp_icon_staff.png",   "随机魔法卷",   "随机魔法 · 100金", ""),
    ]
    return draw_menu_cards(im, "魔法黑市", items, sel, "OK 购买 · ↓ 切换 · 长按 返回")

def scene_func_alchemy(sel=0):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_potion.png",  "体力药水",     "草2 骨1",       "▶"),
        ("sp_icon_orb.png",     "捕捉球",       "草1 晶1",       "炼制"),
        ("sp_icon_seal.png",    "魔法封印卡",   "草2 晶1",       "炼制"),
        ("sp_icon_flask.png",   "中级药水",     "草3 骨2",       "炼制"),
        ("sp_icon_crystal.png", "炎之心片",     "草4 骨3 晶2",   "炼制"),
        ("sp_icon_flask.png",   "高级药水",     "草5 骨4",       "炼制"),
        ("sp_icon_crystal.png", "雷之石",       "草3 晶3",       "炼制"),
    ]
    return draw_menu_cards(im, "炼金台", items, sel, "OK 炼制 · ↓ 切换 · 长按 返回")

def scene_func_equip(sel=3):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_sword.png", "铁剑",     "已装备",     ""),
        ("sp_icon_robe.png",  "学徒法袍", "已装备",     ""),
        ("sp_icon_staff.png", "橡木法杖", "已装备",     ""),
        ("sp_icon_sword.png", "钢刃",     "攻击+4",     "▶"),
        ("sp_icon_staff.png", "秘银杖",   "法强+12%",   ""),
    ]
    return draw_menu_cards(im, "装备", items, sel, "OK 装备/卸下 · ↓ 切换 · 长按 返回")

def scene_func_mupgrade(sel=1):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_fire.png",    "火球", "基础 Lv2",            ""),
        ("sp_icon_ice.png",     "冰锥", "基础 Lv2→Lv3 · 150金", "▶"),
        ("sp_icon_thunder.png", "雷击", "基础 Lv1",            ""),
    ]
    return draw_menu_cards(im, "魔法升级", items, sel, "OK 升级 · ↓ 切换 · 长按 返回")

def scene_func_craft(sel=0):
    im = _town_dim_base(179)
    items = [
        ("sp_icon_staff.png",   "形态", "爆发",         "▶"),
        ("sp_icon_thunder.png",  "元素", "雷",           "OK切"),
        ("sp_icon_crystal.png",  "修饰", "穿透",         "OK切"),
        ("sp_icon_thunder.png",  "雷爆·穿透", "威力 28 耗 9", "预览"),
    ]
    return draw_menu_cards(im, "魔法拼装", items, sel, "OK 切槽 · ↓ 换项 · 长按 装配")

def scene_func_promote():
    im = _town_dim_base(179)
    items = [
        ("sp_icon_crown.png", "当前职级", "见习魔法师", ""),
        ("sp_icon_star.png",  "晋升为",   "初级魔法师", "OK接受"),
    ]
    return draw_menu_cards(im, "晋升仪式", items, 0, "OK 接受晋升 · 长按 返回")


# ============ 战斗遭遇演出（新反馈②：战斗太快只见结算）============
def scene_battle_intro():
    """BP_INTRO：进战斗先亮出敌群 2.5s + 底部遭遇战横幅（HTML walk+meet 对应）。
    动作卡隐藏，仅 HUD + 敌我立绘 + 横幅。"""
    im, d = battle_base()
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    # 多怪站位修复（用户问题⑦）：沿 [BTPX+72=142, BTPX+146=216] 均匀分布，互不重叠
    draw_sprite(im, "sp_slime.png", 142, 250)
    draw_sprite(im, "sp_goblin.png", 216, 250)
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    draw_hud(d, wave="1/10", stat="手动·3·1·2", turn="回合 1")
    # 遭遇横幅：固件 g_toast 战斗位 (8,232,W-16,18)
    bar = Image.new("RGBA", (W - 16, 18), (10, 8, 20, 235))
    d2 = ImageDraw.Draw(bar)
    d2.rectangle([0, 0, W - 17, 17], outline=(0x3a, 0x2d, 0x5e), width=1)
    txt_c(d2, "★ 遭遇战开始！", (W - 16) / 2, 2, F12, (0xff, 0xe9, 0xb0))
    im.paste(bar, (8, 232), bar)
    return im


def scene_battle_submenu(title, rows, sel, footer):
    """战斗子菜单（BP_MAGIC/BP_ITEM）：复用战斗背景 + HUD + 底部三卡，
    居中浮层列表面板带 ▶ 选中，与固件 mage_fx_choices 1:1。"""
    im, d = battle_base()
    draw_hud(d, wave="1/10", stat="手动·3·1·2", turn="回合 2")
    draw_cards(im, d, turn="回合 2")
    px, py, pw, ph = 16, 56, W - 32, 210
    d.rectangle([px, py, px + pw, py + ph], fill=(0x0a, 0x08, 0x18), outline=GOLD)
    txt_c(d, title, px + pw / 2, py + 8, F12, GOLD)
    d.rectangle([px + 1, py + 24, px + pw - 1, py + 24], fill=(0x2a, 0x23, 0x48))
    y = py + 38
    for i, (name, sub) in enumerate(rows):
        selp = (i == sel)
        if selp:
            d.rectangle([px + 6, y - 2, px + pw - 6, y + 17], fill=(0x3a, 0x2c, 0x5c), outline=GOLD)
            mark = "▶ "
        else:
            mark = "  "
        txt_l(d, mark + name, px + 12, y, F12, (0xff, 0xff, 0xff) if selp else DIM)
        if sub:
            txt_r(d, sub, px + pw - 8, y, F12, GOLD if selp else DIM)
        y += 26
    txt_c(d, footer, px + pw / 2, py + ph - 16, F10, DIM)
    return im


def scene_battle_magic(sel=1):
    rows = [("火球", "威力 12 耗 5"), ("冰锥", "威力 10 耗 4"),
            ("雷击", "威力 16 耗 8"), ("治愈微风", "回复 20 耗 6")]
    return scene_battle_submenu("选择魔法", rows, sel, "OK 施放 · ↓↑切换 · 长按返回")


def scene_battle_item(sel=0):
    rows = [("药水 ×3", "回复生命/魔力"), ("捕捉球 ×5", "捕捉魔物"),
            ("封印卡 ×1", "高级捕捉")]
    return scene_battle_submenu("选择道具", rows, sel, "OK 使用 · ↓↑切换 · 长按返回")


# ============ 图鉴/伙伴页·金框 + 程序化怪物立绘（本轮 1:1 复刻核对）============
def scene_dex(idx=0):
    """魔物图鉴（frameBox 详情页）：金框 + 立绘盒(86,36,68,58) + drawMob 立绘
    foot(120,90) + 12px 居中正文从 y=98 起（与固件刷新逻辑一致）。"""
    im = _town_dim_base(153)
    frame_box(im, "魔物图鉴")
    d = ImageDraw.Draw(im)
    d.rectangle([86, 36, 153, 93], fill=(0x14, 0x0f, 0x28), outline=GOLD2)
    draw_sprite(im, "sp_slime.png", 120, 90)
    body = [("已收录 12/50 · 收服 3", GOLD), ("史莱姆", GOLD),
            ("可捕捉 · 用捕捉球", WHITE), ("出没：试炼森林", WHITE),
            ("天赋：撞击  特性：坚韧", WHITE), ("进化：Lv14 → 史莱姆·王", WHITE),
            ("试炼森林的凝胶生物，", WHITE), ("软乎乎却很黏人。", WHITE),
            ("(3 / 50)", WHITE)]
    y = 110
    for ln, col in body:
        txt_c(d, ln, W / 2, y, F12, col)
        y += 15
    frame_hint(im, "OK 捕捉条件 · ↓ 切换 · 长按 返回")
    return im


def scene_partners(idx=0):
    """伙伴页（frameBox 详情页）：金框 + 104x96 描金双框立绘卡 + 出战物种立绘
    foot(120,118) + 12px 正文从 y=136 起。用户要求：出战什么显示什么形象。"""
    im = _town_dim_base(153)
    frame_box(im, "伙伴")
    d = ImageDraw.Draw(im)
    d.rectangle([68, 30, 171, 125], fill=(0x1e, 0x16, 0x3a), outline=GOLD2)
    d.rectangle([71, 33, 168, 122], fill=(0x10, 0x0b, 0x24))
    draw_sprite(im, "sp_cat.png", 120, 118)      # 出战物种（示例：猫灵）
    body = [("绒绒 ★", GOLD), ("猫灵 Lv3 资质B(30/45)", WHITE),
            ("技能 迅捷：先手一击", (0xff, 0x6b, 0xd6)),
            ("特性 幸运：宝箱加成", (0x7f, 0xdc, 0x8a)),
            ("★ Lv16 可进化·月影", GOLD),
            ("攻7 防8 速9", WHITE), ("(1 / 1)", WHITE)]
    y = 136
    for ln, col in body:
        txt_c(d, ln, W / 2, y, F12, col)
        y += 15
    frame_hint(im, "OK 出战 · ↓ 切换 · 长按 返回")
    return im


def scene_partners_fox(idx=0):
    """伙伴页·灵狐出战变体：证明「出战什么就显示什么形象」——
    灵狐出战时立绘卡里是真正的狐狸（sp_fox 来自 HTML drawMob，非兔子）。"""
    im = _town_dim_base(153)
    frame_box(im, "伙伴")
    d = ImageDraw.Draw(im)
    d.rectangle([68, 30, 171, 125], fill=(0x1e, 0x16, 0x3a), outline=GOLD2)
    d.rectangle([71, 33, 168, 122], fill=(0x10, 0x0b, 0x24))
    draw_sprite(im, "sp_fox.png", 120, 118)      # 灵狐 35x43（HTML drawMob 原样）
    body = [("小狐 ★", GOLD), ("灵狐 Lv4 资质A(34/45)", WHITE),
            ("技能 迅捷：先手一击", (0xff, 0x6b, 0xd6)),
            ("特性 幸运：宝箱加成", (0x7f, 0xdc, 0x8a)),
            ("★ Lv16 可进化·九尾", GOLD),
            ("攻9 防8 速11", WHITE), ("(2 / 2)", WHITE)]
    y = 136
    for ln, col in body:
        txt_c(d, ln, W / 2, y, F12, col)
        y += 15
    frame_hint(im, "OK 出战 · ↓ 切换 · 长按 返回")
    return im
def scene_battle_kill():
    """BP_KILL 演出：清空敌群后停留 ~0.9s，伤害飘字/受击特效仍可见，再进结算。
    预览用横幅标注该窗口，证明不再"一击就结算"。"""
    im, d = battle_base()
    draw_sprite(im, SRC_CAT, 44, 250, CAT_H)
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)   # 主角突进收尾
    # 最后一击伤害飘字（仍停留可见）
    txt_c(d, "42", 150, 196, F10, (0xff, 0xff, 0xff))
    txt_c(d, "-3", 70, 200, F10, (0xff, 0x6b, 0x6b))
    # 击破横幅
    bar = Image.new("RGBA", (W - 16, 18), (10, 8, 20, 235))
    d2 = ImageDraw.Draw(bar)
    d2.rectangle([0, 0, W - 17, 17], outline=(0x3a, 0x2d, 0x5e), width=1)
    txt_c(d2, "✦ 击破！演出中（约 0.9s 后结算）", (W - 16) / 2, 2, F12, (0xff, 0xe9, 0xb0))
    im.paste(bar, (8, 232), bar)
    draw_hud(d, wave="3/10", stat="手动·3·1·2", turn="回合 3")
    draw_cards(im, d, turn="回合 3")
    return im


# ============ 蘑菇怪贴图·真透明证据（用户问题⑥）============
def scene_mushroom():
    """问题⑥：蘑菇怪贴图在战斗/图鉴里带色背景 -> 现用洋红底重生成 + magenta_key 抠图，
    背景残留 0%（已脚本校验）。预览用真透明合成到战斗背景，证明无灰底方块。"""
    im, d = battle_base()
    draw_sprite(im, SRC_HERO, 70, 250, HERO_H)
    draw_sprite(im, "sp_mush.png", 150, 250)   # 真透明，无灰底
    draw_hud(d, wave="2/10", stat="手动·3·1·2", turn="回合 2")
    draw_cards(im, d, turn="回合 2")
    return im
def overview(scenes_labels):
    pad = 6
    lw = W + pad * 2
    lh = H + 30
    cols = 2
    rows = (len(scenes_labels) + cols - 1) // cols
    canvas = Image.new("RGB", (lw * cols, lh * rows), (0x20, 0x18, 0x30))
    cd = ImageDraw.Draw(canvas)
    for idx, (sc, label) in enumerate(scenes_labels):
        cx = (idx % cols) * lw
        cy = (idx // cols) * lh
        canvas.paste(sc, (cx + pad, cy + 26))
        cd.rectangle([cx + pad, cy + 26, cx + pad + W - 1, cy + 26 + H - 1],
                     outline=(0xff, 0xd9, 0x3d), width=1)
        txt_l(cd, label, cx + pad, cy + 6, F12, (0xff, 0xff, 0xff))
    return canvas


def main():
    os.makedirs(OUT, exist_ok=True)
    boss = scene_boss()
    evt = scene_event("chest")
    trap = scene_event("trap")
    town = scene_town(0)
    hud = scene_hud()
    result = scene_result()
    expo = scene_expo()
    loot_chest = scene_loot("chest")
    loot_herb = scene_loot("herb")
    town_acad = scene_town_sub()
    town_wild = scene_town_wild()
    town_guild = scene_town_guild()
    func_status = scene_func_status()
    func_bag = scene_func_bag()
    func_menu = scene_func_menu()
    func_home = scene_func_home()
    func_research = scene_func_research()
    func_forge = scene_func_forge()
    func_market = scene_func_market()
    func_alchemy = scene_func_alchemy()
    func_equip = scene_func_equip()
    func_mupgrade = scene_func_mupgrade()
    func_craft = scene_func_craft()
    func_promote = scene_func_promote()
    battle_intro = scene_battle_intro()
    dex = scene_dex()
    partners = scene_partners()
    partners_fox = scene_partners_fox()
    battle_kill = scene_battle_kill()
    mushroom = scene_mushroom()
    battle_magic = scene_battle_magic()
    battle_item = scene_battle_item()

    paths = {
        "mage_battle_boss_240x320.png": boss,
        "mage_event_240x320.png": evt,
        "mage_event_trap_240x320.png": trap,
        "mage_town_240x320.png": town,
        "mage_hud_240x320.png": hud,
        "mage_result_240x320.png": result,
        "mage_expo_240x320.png": expo,
        "mage_loot_chest_240x320.png": loot_chest,
        "mage_loot_herb_240x320.png": loot_herb,
        "mage_town_acad_240x320.png": town_acad,
        "mage_town_wild_240x320.png": town_wild,
        "mage_town_guild_240x320.png": town_guild,
        "mage_func_status_240x320.png": func_status,
        "mage_func_bag_240x320.png": func_bag,
        "mage_func_menu_240x320.png": func_menu,
        "mage_func_home_240x320.png": func_home,
        "mage_func_research_240x320.png": func_research,
        "mage_func_forge_240x320.png": func_forge,
        "mage_func_market_240x320.png": func_market,
        "mage_func_alchemy_240x320.png": func_alchemy,
        "mage_func_equip_240x320.png": func_equip,
        "mage_func_mupgrade_240x320.png": func_mupgrade,
        "mage_func_craft_240x320.png": func_craft,
        "mage_func_promote_240x320.png": func_promote,
        "mage_battle_intro_240x320.png": battle_intro,
        "mage_dex_240x320.png": dex,
        "mage_partners_240x320.png": partners,
        "mage_partners_fox_240x320.png": partners_fox,
        "mage_battle_kill_240x320.png": battle_kill,
        "mage_mushroom_240x320.png": mushroom,
        "mage_battle_magic_240x320.png": battle_magic,
        "mage_battle_item_240x320.png": battle_item,
    }
    for name, im in paths.items():
        p = os.path.join(OUT, name)
        im.save(p)
        im.resize((W * 2, H * 2), Image.LANCZOS).save(
            os.path.join(OUT, name.replace("240x320", "2x")))
        print("输出：", p)

    ov = overview([
        (boss, "① 战斗·BOSS 立绘（问题③②⑤）"),
        (evt,  "② 事件面板·宝箱（问题①）"),
        (trap, "③ 事件面板·陷阱（问题①）"),
        (town, "④ 城镇界面·文字可见（问题④）"),
        (hud,  "⑤ 战斗·HUD 中文（问题⑤）"),
        (result, "⑥ 每波结算·显示素材（bug⑥）"),
        (expo,  "⑥ 远征汇总·BOSS 装备（bug⑥）"),
        (loot_chest, "⑥ 宝箱战利品·具体物品（bug⑥）"),
        (loot_herb, "⑥ 采集收获（bug⑥）"),
        (town_acad, "⑦ 魔法学院·卡片列表+图标（新①超框修复）"),
        (town_wild, "⑦ 野外试炼·卡片列表+图标（新①超框修复）"),
        (town_guild, "⑦ 冒险者公会·卡片列表+图标（新①超框修复）"),
        (func_status, "⑧ 功能面板·角色·有背景（新③）"),
        (func_bag, "⑧ 功能面板·背包·有背景（新③）"),
        (func_menu, "⑭ 设置·图标卡片（本轮）"),
        (func_home, "⑮ 住宅·图标卡片（本轮）"),
        (func_research, "⑯ 魔法研究·图标卡片（本轮）"),
        (func_forge, "⑰ 锻造房·图标卡片（本轮）"),
        (func_market, "⑱ 魔法黑市·图标卡片（本轮）"),
        (func_alchemy, "⑲ 炼金台·图标卡片（本轮）"),
        (func_equip, "⑳ 装备·图标卡片（本轮）"),
        (func_mupgrade, "㉑ 魔法升级·图标卡片（本轮）"),
        (func_craft, "㉒ 魔法拼装·图标卡片（本轮）"),
        (func_promote, "㉓ 晋升仪式·图标卡片（本轮）"),
        (battle_intro, "⑨ 遭遇演出·敌群+横幅（新②）"),
        (dex, "⑩ 图鉴页·怪物立绘（新③）"),
        (partners, "⑩ 伙伴页·真实立绘（新③）"),
        (partners_fox, "⑩ 伙伴页·灵狐出战=真狐狸（用户追加）"),
        (battle_kill, "⑪ 击杀演出窗口·不再秒结算（新②）"),
        (mushroom, "⑫ 蘑菇怪贴图·真透明无灰底（问题⑥）"),
        (battle_magic, "⑬ 魔法选择列表·▶ 选中可见（本轮战斗手动修复）"),
        (battle_item, "⑬ 道具选择列表·▶ 选中可见（本轮战斗手动修复）"),
    ])
    op = os.path.join(OUT, "mage_fixes_overview_480x640.png")
    ov.save(op)
    print("输出：", op)


if __name__ == "__main__":
    main()
