#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_mage_refine.py —— 魔法帝之路「设计感细化」PC 对比图（提案版）。

目的：把社区参考作品的「布局与设计感」内化到魔法帝自己的紫金魔法风格里，
      NOT 抄素材。三屏提案：
        ① 主菜单（统一标题栏 + 卡片 + 底部 HUD 常驻）
        ② 伙伴网格（4×2 立绘格子，替代稀疏列表）
        ③ 面板页·锻造（统一面板框 + 标题栏 + 进度条 + 底部提示条）
产出（assets/preview/）：三屏 240x320 + 2x + 一张总览。
"""
from __future__ import annotations
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "mage-game", "assets")
OUT = os.path.join(ROOT, "assets", "preview")
W, H = 240, 320

# ---- 配色（魔法紫金调性，与固件 C_BG / GOLD 一致）----
BG = (0x0d, 0x0b, 0x1a)
GOLD = (0xff, 0xd9, 0x3d)
GOLD2 = (0xc9, 0xa2, 0x27)
DIM = (0xc9, 0xb8, 0xee)
WHITE = (0xff, 0xff, 0xff)
PANEL = (0x1a, 0x14, 0x33)
CARD = (0x23, 0x1b, 0x42)
CARD_SEL = (0x3a, 0x2c, 0x5c)
LINE = (0x4a, 0x3d, 0x6e)
ACCENT = (0xb9, 0x8c, 0xff)   # 魔法紫，用于强调/选中描边


def font(px):
    for name in ("msyh.ttc", "simhei.ttf", "simsun.ttc", "consola.ttf", "arial.ttf"):
        for base in (r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu"):
            p = os.path.join(base, name)
            if os.path.exists(p):
                try:
                    return ImageFont.truetype(p, px)
                except Exception:
                    pass
    return ImageFont.load_default()


F8, F10, F12, F13, F16 = (font(s) for s in (8, 10, 12, 13, 16))

GROUND = 250


def cover(img_path, w, h):
    im = Image.open(img_path).convert("RGB")
    sw, sh = im.size
    win = int(round(sh * (w / h)))
    if win > sw:
        win = sw
    left = (sw - win) // 2
    return im.crop((left, 0, left + win, sh)).resize((w, h), Image.LANCZOS)


def tri(d, x, y, s, col, right=True):
    """选中标记：实心三角（替代 ▶ 文字符号，避免字体豆腐块）。"""
    if right:
        d.polygon([(x, y), (x, y + s), (x + s, y + s / 2)], fill=col)
    else:
        d.polygon([(x + s, y), (x + s, y + s), (x, y + s / 2)], fill=col)


def load_png(name, alpha=True):
    p = os.path.join(ASSETS, name)
    if not os.path.exists(p):
        return None
    return Image.open(p).convert("RGBA") if alpha else Image.open(p).convert("RGB")


def draw_sprite(im, png_name, cx, foot_y, disp_h=None):
    s = load_png(png_name)
    if s is None:
        return
    if disp_h is None:
        w, h = s.size
    else:
        ratio = s.height / s.width
        w = max(1, int(round(disp_h / ratio)))
        s = s.resize((w, disp_h), Image.LANCZOS)
    im.paste(s, (int(cx - w / 2), int(foot_y - s.height)), s)


def txt_c(d, t, cx, y, fnt, fill):
    tw = d.textlength(t, font=fnt)
    d.text((cx - tw / 2, y), t, font=fnt, fill=fill)


def txt_l(d, t, x, y, fnt, fill):
    d.text((x, y), t, font=fnt, fill=fill)


def txt_r(d, t, rx, y, fnt, fill):
    tw = d.textlength(t, font=fnt)
    d.text((rx - tw, y), t, font=fnt, fill=fill)


# ---- 背景：魔法暗调 + 星点 + 中心柔光（仅预览，不耗 Flash）----
def make_bg():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    # 中心柔光（魔法氛围）
    for r in range(120, 0, -8):
        a = int(10 * (1 - r / 120))
        d.ellipse([W // 2 - r, H // 2 - r * 1.3, W // 2 + r, H // 2 + r * 1.3],
                  fill=(0x1e, 0x16, 0x3a))
    # 星点
    import random
    random.seed(7)
    for _ in range(46):
        x = random.randint(4, W - 4)
        y = random.randint(4, H - 4)
        b = random.randint(60, 150)
        d.point((x, y), fill=(b, b, b + 20))
    # 底部压暗，保证底部 HUD 可读
    for y in range(H - 40, H):
        t = (y - (H - 40)) / 40.0
        d.line([(0, y), (W, y)], fill=(0x06, 0x05, 0x12))
    return im, ImageDraw.Draw(im)


def rounded(d, box, fill, outline, width=1, radius=8):
    try:
        d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)
    except Exception:
        d.rectangle(box, fill=fill, outline=outline, width=width)


def progress_bar(d, x, y, w, h, ratio, col=ACCENT):
    rounded(d, (x, y, x + w, y + h), (0x12, 0x0e, 0x26), LINE, width=1, radius=h // 2)
    fw = max(0, int((w - 2) * max(0.0, min(1.0, ratio))))
    if fw > 0:
        rounded(d, (x + 1, y + 1, x + 1 + fw, y + h - 1), col, None, radius=h // 2)


# ============ ① 主菜单 ============
def scene_menu(sel=0):
    im, d = make_bg()
    # 顶部标题栏
    d.rectangle([0, 0, W, 22], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, 22), (W, 22)], fill=GOLD, width=1)
    txt_l(d, "魔法帝之路", 8, 5, F13, GOLD)
    txt_r(d, "Lv3 ★", W - 8, 6, F10, ACCENT)
    # 副标题
    txt_c(d, "— 学徒的征途 —", W / 2, 30, F10, DIM)
    # 菜单卡片
    items = [
        ("sp_icon_bolt.png", "开始冒险", "深入试炼森林"),
        ("sp_icon_book.png", "魔法研究", "135 种魔法待习"),
        ("sp_icon_paw.png", "伙伴图鉴", "已收服 3 / 8"),
        ("sp_icon_crown.png", "晋升仪式", "见习 → 初级"),
        ("sp_icon_gear.png", "设置", "自动/音效/存档"),
    ]
    y0 = 50
    for i, (icon, name, sub) in enumerate(items):
        bx, by = 14, y0 + i * 46
        selp = (i == sel)
        rounded(d, (bx, by, bx + W - 28, by + 40), CARD_SEL if selp else CARD,
                GOLD if selp else LINE, width=2 if selp else 1, radius=9)
        if selp:
            d.rectangle([bx + 2, by + 2, bx + 5, by + 38], fill=GOLD)   # 左侧选中亮条
            tri(d, bx + 9, by + 14, 10, GOLD)
        ic = load_png(icon)
        if ic:
            ic = ic.resize((26, 26), Image.LANCZOS)
            im.paste(ic, (bx + 26, by + 7), ic)
        txt_l(d, name, bx + 60, by + 8, F13, WHITE if selp else WHITE)
        txt_l(d, sub, bx + 60, by + 24, F10, DIM)
    # 底部 HUD 常驻条
    d.rectangle([0, H - 20, W, H], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, H - 20), (W, H - 20)], fill=GOLD, width=1)
    txt_l(d, "金币 320", 8, H - 16, F10, GOLD)
    txt_c(d, "↑↓ 选择 · OK 进入", W / 2, H - 16, F10, DIM)
    txt_r(d, "HP 80/80", W - 8, H - 16, F10, (0xff, 0x88, 0x88))
    return im


# ============ ② 伙伴网格 ============
PARTNERS = [
    ("sp_cat.png", "绒绒", "猫灵 Lv3 ★", True),
    ("sp_fox.png", "小狐", "灵狐 Lv4 ★", True),
    ("sp_slime.png", "石甲", "史莱姆 Lv2", True),
    ("sp_goblin.png", "棘刺", "哥布林 Lv5", True),
    ("sp_mush.png", "菇宝", "蘑菇怪 Lv3", True),
    ("sp_wolf.png", "苍牙", "魔狼 Lv6", True),
    ("sp_goblin_boss.png", "炎魔", "首领 Lv10", True),
    (None, "？？？", "未解锁", False),
]


def scene_partners_grid(sel=0):
    im, d = make_bg()
    # 标题栏
    d.rectangle([0, 0, W, 22], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, 22), (W, 22)], fill=GOLD, width=1)
    txt_l(d, "伙伴图鉴", 8, 5, F13, GOLD)
    txt_r(d, "出战 3/8", W - 8, 6, F10, ACCENT)
    # 网格 4×2
    cols, rows = 4, 2
    gx, gy = 10, 30
    cw, ch = (W - 20) // cols, 104
    for idx, (spr, name, tag, owned) in enumerate(PARTNERS):
        r, c = divmod(idx, cols)
        bx, by = gx + c * cw, gy + r * ch
        selp = (idx == sel)
        rounded(d, (bx + 2, by + 2, bx + cw - 2, by + ch - 2),
                CARD_SEL if selp else CARD, GOLD if selp else LINE,
                width=2 if selp else 1, radius=8)
        # 立绘框
        ix, iy, iw, ih = bx + 8, by + 8, cw - 16, 56
        rounded(d, (ix, iy, ix + iw, iy + ih), (0x10, 0x0b, 0x24), LINE, width=1, radius=6)
        if spr:
            draw_sprite(im, spr, ix + iw / 2, iy + ih - 4, 40)
        else:
            txt_c(d, "？", ix + iw / 2, iy + 18, F16, LINE)
        # 名字 + 角标
        txt_c(d, name, bx + cw / 2, by + 68, F12, WHITE)
        txt_c(d, tag, bx + cw / 2, by + 84, F8, GOLD if owned else DIM)
        if selp:
            tri(d, bx + 5, by + 7, 9, GOLD)
    # 选中伙伴情报条（保留关键可读信息，与固件 g_part_info 一致）
    d.rectangle([0, H - 38, W, H - 20], fill=(0x14, 0x0f, 0x28))
    txt_c(d, "绒绒 ★ 资质S(42/45) · Lv3 · 出战", W / 2, H - 34, F10, GOLD)
    # 底部提示条
    d.rectangle([0, H - 18, W, H], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, H - 18), (W, H - 18)], fill=GOLD, width=1)
    txt_c(d, "OK 出战 · ↓ 切换 · 长按 菜单", W / 2, H - 15, F10, DIM)
    return im


# ============ ③ 面板页·锻造（统一框 + 标题栏 + 进度条 + 提示条）============
def scene_forge(sel=0):
    im, d = make_bg()
    # 标题栏
    d.rectangle([0, 0, W, 22], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, 22), (W, 22)], fill=GOLD, width=1)
    txt_l(d, "锻造房", 8, 5, F13, GOLD)
    txt_r(d, "持有 320 金", W - 8, 6, F10, GOLD)
    # 大面板（统一边框：外金描边 + 内浅描边）
    px, py, pw, ph = 10, 30, W - 20, H - 30 - 22
    rounded(d, (px, py, px + pw, py + ph), PANEL, GOLD, width=2, radius=10)
    rounded(d, (px + 3, py + 3, px + pw - 3, py + ph - 3), None, LINE, width=1, radius=8)
    # 面板内标题条
    d.rectangle([px + 3, py + 3, px + pw - 3, py + 22], fill=(0x24, 0x1c, 0x44))
    txt_l(d, "锻 造 工 坊", px + 12, py + 8, F12, GOLD)
    txt_r(d, "工匠 Lv4", px + pw - 6, py + 9, F10, DIM)
    # 进度条（工匠升级）
    txt_l(d, "工匠经验", px + 12, py + 30, F10, DIM)
    progress_bar(d, px + 70, py + 31, pw - 90, 8, 0.64)
    txt_r(d, "64%", px + pw - 6, py + 29, F8, ACCENT)
    # 锻造项卡片
    items = [
        ("sp_icon_sword.png", "锻造武器", "钢刃 · 攻击+4", "120金 · 兽骨×6"),
        ("sp_icon_robe.png", "锻造法袍", "秘银袍 · 生命+12", "90金 · 兽骨×4"),
    ]
    y0 = py + 48
    for i, (icon, name, sub, cost) in enumerate(items):
        bx, by = px + 10, y0 + i * 44
        selp = (i == sel)
        rounded(d, (bx, by, bx + pw - 20, by + 38), CARD_SEL if selp else CARD,
                GOLD if selp else LINE, width=2 if selp else 1, radius=9)
        if selp:
            d.rectangle([bx + 2, by + 2, bx + 5, by + 36], fill=GOLD)
            tri(d, bx + 9, by + 13, 10, GOLD)
        ic = load_png(icon)
        if ic:
            ic = ic.resize((26, 26), Image.LANCZOS)
            im.paste(ic, (bx + 26, by + 6), ic)
        txt_l(d, name, bx + 60, by + 7, F13, WHITE)
        txt_l(d, sub, bx + 60, by + 22, F10, DIM)
        txt_r(d, cost, bx + pw - 26, by + 12, F10, GOLD if selp else DIM)
    # 持有材料栏（填充面板下部，信息密度）
    my = y0 + 2 * 44 + 6
    d.line([(px + 10, my), (px + pw - 10, my)], fill=LINE, width=1)
    txt_l(d, "持有材料", px + 12, my + 8, F10, DIM)
    mats = [("sp_icon_bone.png", "兽骨×12"), ("sp_icon_crystal.png", "魔晶×3"),
            ("sp_icon_herb.png", "月光草×8"), ("sp_icon_coin.png", "银矿石×2")]
    mw = (pw - 24) // 4
    for j, (mic, mname) in enumerate(mats):
        mx = px + 12 + j * mw
        mic_img = load_png(mic)
        if mic_img:
            mic_img = mic_img.resize((16, 16), Image.LANCZOS)
            im.paste(mic_img, (mx, my + 28), mic_img)
        txt_l(d, mname, mx + 19, my + 30, F8, WHITE)
    # 底部提示条
    d.rectangle([0, H - 18, W, H], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, H - 18), (W, H - 18)], fill=GOLD, width=1)
    txt_c(d, "OK 锻造 · ↓ 切换 · 长按 返回", W / 2, H - 15, F10, DIM)
    return im


# ============ ④ 战斗画面（新设计语言） ============
def battle_bg():
    im = Image.new("RGB", (W, H), (0x15, 0x10, 0x2a))
    p = os.path.join(ASSETS, "battle_bg_k.png")
    if os.path.exists(p):
        im.paste(cover(p, W, H), (0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle([0, GROUND, W, GROUND + 2], fill=(0x5c, 0x8f, 0x42))
    # 活动带压暗（alpha 合成，不涂黑）；合成一次，绘图对象必须绑在最终图上
    dim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dim)
    for y in range(140, 271):
        t = (y - 140) / 130.0
        dd.rectangle([0, y, W, y], fill=(6, 5, 16, max(0, int(70 * (1 - t)))))
    im = Image.alpha_composite(im.convert("RGBA"), dim).convert("RGB")
    return im, ImageDraw.Draw(im)


def battle_hud(d, wave, turn, hp_r=0.9, mp_r=0.7):
    d.rectangle([0, 0, W, 22], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, 22), (W, 22)], fill=GOLD, width=1)
    txt_l(d, "战斗", 8, 5, F13, GOLD)
    txt_r(d, wave, W - 8, 3, F10, GOLD)
    txt_r(d, turn, W - 8, 13, F8, DIM)
    # HP / MP 细条
    txt_l(d, "HP", 8, 26, F8, (0xff, 0x88, 0x88))
    d.rounded_rectangle([26, 27, 96, 33], radius=3, fill=(0x3a, 0x1a, 0x1a))
    d.rounded_rectangle([26, 27, 26 + 70 * hp_r, 33], radius=3, fill=(0xe0, 0x55, 0x55))
    txt_l(d, "MP", 8, 36, F8, CYAN)
    d.rounded_rectangle([26, 37, 96, 43], radius=3, fill=(0x1a, 0x2a, 0x3a))
    d.rounded_rectangle([26, 37, 26 + 70 * mp_r, 43], radius=3, fill=(0x5e, 0xc8, 0xf2))


CYAN = (94, 200, 242)


def battle_cards(im, d, sel=0):
    names = ["攻击", "魔法", "道具"]
    icons = ["sp_icon_sword.png", "sp_icon_staff.png", "sp_icon_potion.png"]
    cols = 3
    cw = (W - 20) // cols
    y0 = H - 54
    for i in range(cols):
        bx = 10 + i * cw
        selp = (i == sel)
        rounded(d, (bx, y0, bx + cw - 6, y0 + 40), CARD_SEL if selp else CARD,
                GOLD if selp else LINE, width=2 if selp else 1, radius=9)
        if selp:
            d.rectangle([bx + 2, y0 + 2, bx + 5, y0 + 38], fill=GOLD)
        ic = load_png(icons[i])
        if ic:
            ic = ic.resize((26, 26), Image.LANCZOS)
            im.paste(ic, (bx + (cw - 6) // 2 - 13, y0 + 3), ic)
        txt_c(d, names[i], bx + (cw - 6) // 2, y0 + 28, F10, WHITE)
    d.rectangle([0, H - 14, W, H], fill=(0x10, 0x0c, 0x1f))
    d.line([(0, H - 14), (W, H - 14)], fill=GOLD, width=1)
    txt_c(d, "OK 攻击 · ↓ 魔法 · ↑ 道具 · 长按 返回", W / 2, H - 11, F8, DIM)


def scene_battle(sel=0):
    im, d = battle_bg()
    # 所有立绘必须显式传显示高度：原图全是巨图（slime 462x295 / goblin 374x502），
    # 不传会按原尺寸糊满整屏 = "巨型怪物"事故
    draw_sprite(im, "sp_cat_k.png", 44, GROUND, 32)       # 伙伴
    draw_sprite(im, "sp_slime_k.png", 150, GROUND, 36)    # 史莱姆
    draw_sprite(im, "sp_goblin_k.png", 200, GROUND, 44)   # 哥布林
    draw_sprite(im, "sp_hero_k.png", 92, GROUND, 46)      # 主角
    txt_c(d, "42", 150, GROUND - 52, F10, WHITE)          # 伤害飘字
    txt_c(d, "-5", 92, GROUND - 60, F10, (0xff, 0x6b, 0x6b))
    battle_hud(d, "10/10 BOSS", "回合 3")
    battle_cards(im, d, sel)
    return im


def overview(scenes):
    pad = 6
    lw, lh = W + pad * 2, H + 30
    cols = 1
    rows = len(scenes)
    canvas = Image.new("RGB", (lw * cols + 200, lh * rows), (0x20, 0x18, 0x30))
    cd = ImageDraw.Draw(canvas)
    x0 = 0
    for idx, (sc, label) in enumerate(scenes):
        cy = idx * lh
        canvas.paste(sc, (x0 + pad, cy + 26))
        cd.rectangle([x0 + pad, cy + 26, x0 + pad + W - 1, cy + 26 + H - 1],
                     outline=GOLD, width=1)
        txt_l(cd, label, x0 + pad, cy + 6, F12, WHITE)
        # 右侧说明
        txt_l(cd, NOTE[idx], x0 + pad + W + 12, cy + 120, F10, DIM)
    return canvas


NOTE = [
    "① 主菜单\n· 顶部标题栏(屏名+等级)\n· 卡片带左亮条/选中金框\n· 底部 HUD 常驻(金币/HP)",
    "② 伙伴图鉴\n· 4×2 立绘网格\n· 每格=立绘+名字+角标\n· 选中金框+三角 取代列表",
    "③ 锻造房\n· 统一面板框(金描边+内描)\n· 内标题条 + 进度条\n· 底部提示条",
    "④ 战斗\n· 标题栏(战斗+波次/回合)\n· HP/MP 细条\n· 底部三张动作卡(剑/杖/药剂)",
]


def main():
    os.makedirs(OUT, exist_ok=True)
    screens = [
        (scene_menu(0), "① 主菜单（标题栏+卡片+底部HUD）"),
        (scene_partners_grid(0), "② 伙伴图鉴（4×2 立绘网格）"),
        (scene_forge(0), "③ 锻造房（面板框+进度条+提示条）"),
        (scene_battle(0), "④ 战斗（标题栏+HP/MP细条+动作卡）"),
    ]
    for name, (im, _) in zip(
        ["mage_refine_menu_240x320.png", "mage_refine_partners_240x320.png",
         "mage_refine_forge_240x320.png", "mage_refine_battle_240x320.png"], screens):
        p = os.path.join(OUT, name)
        im.save(p)
        im.resize((W * 2, H * 2), Image.LANCZOS).save(
            os.path.join(OUT, name.replace("240x320", "2x")))
        print("输出：", p)
    ov = overview(screens)
    op = os.path.join(OUT, "mage_refine_overview.png")
    ov.save(op)
    print("输出：", op)


if __name__ == "__main__":
    main()
