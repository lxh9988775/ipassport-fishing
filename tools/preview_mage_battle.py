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
def draw_hud(d, wave="1/10", stat="AUTO 编1 球3 卡1 药2", turn="回合 1",
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
def draw_cards(d, turn="回合 1"):
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
        txt_c(d, key, x + 14, CARD_Y + 6, F9, (0xff, 0xd9, 0x3d))
        txt_c(d, name, x + 38, CARD_Y + 18, F12, (0xff, 0xff, 0xff))
    # 回合徽标
    lb = turn
    lw = len(lb) * 6 + 14
    bx = W // 2 - lw // 2
    d.rectangle([bx, CARD_Y - 7, bx + lw, CARD_Y + 7], fill=(0x0d, 0x0a, 0x1a),
                outline=(0xc9, 0xa4, 0x4a))
    txt_c(d, lb, W / 2, CARD_Y - 5, F8, (0xff, 0xd9, 0x3d))


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
    draw_hud(d, wave="10/10 BOSS", stat="手动 编2 球1 卡0 药1", turn="回合 3")
    draw_cards(d, turn="回合 3")
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
    draw_cards(d)
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
    txt_c(idr, name, bw / 2, 64, F12, (0xff, 0xff, 0xff))
    txt_c(idr, desc, bw / 2, 82, F10, (0xff, 0xd9, 0x3d))
    txt_c(idr, "C 绕开", bw / 2, 98, F9, (0x9a, 0x93, 0xb5))
    im.paste(box, (bx, by), box)
    return im


# ============ 场景三：城镇界面 · 文字可见（问题④）============
LOCATIONS = ["魔法学院", "野外试炼", "冒险者公会", "住宅",
             "锻造房", "魔法黑市", "魔物图鉴"]


def scene_town(sel=0):
    im = Image.new("RGB", (W, H), (0x12, 0x0e, 0x22))
    d = ImageDraw.Draw(im)
    bg = os.path.join(ASSETS, SRC_TOWN)
    if os.path.exists(bg):
        # 与固件 load_rgba 一致：直接 resize 到 240x320（非 cover 裁剪）
        im.paste(Image.open(bg).convert("RGB").resize((W, H), Image.LANCZOS), (0, 0))
    # 顶部半透明压暗条，保证标题可读
    bar = Image.new("RGBA", (W, 40), (0x08, 0x05, 0x14, 150))
    im.paste(bar, (0, 0), bar)
    im = im.convert("RGB")
    d = ImageDraw.Draw(im)
    txt_c(d, "· 魔法小镇 ·", W / 2, 16, F16, (0xff, 0xd9, 0x3d))   # g_scene_title 金色
    # 地点列表（g_scene_body 白色，▶ 选中）
    y = 56
    for i, name in enumerate(LOCATIONS):
        prefix = "▶ " if i == sel else "  "
        txt_l(d, prefix + name, 18, y, F13, (0xff, 0xff, 0xff))
        y += 30
    # 底部提示
    txt_c(d, "↑↓ 选地点 · OK 进入 · 长按 功能栏", W / 2, H - 22, F10, (0xc9, 0xb8, 0xee))
    return im


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
    draw_hud(d, wave="5/10 精英", stat="手动 编1 球3 卡1 药2", turn="回合 1")
    draw_cards(d)
    return im


# ============ 汇总对照（2 列网格，逐格标注对应问题）============
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

    paths = {
        "mage_battle_boss_240x320.png": boss,
        "mage_event_240x320.png": evt,
        "mage_event_trap_240x320.png": trap,
        "mage_town_240x320.png": town,
        "mage_hud_240x320.png": hud,
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
    ])
    op = os.path.join(OUT, "mage_fixes_overview_480x640.png")
    ov.save(op)
    print("输出：", op)


if __name__ == "__main__":
    main()
