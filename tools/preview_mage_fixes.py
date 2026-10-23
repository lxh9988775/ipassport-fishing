#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_mage_fixes.py —— 渲染「魔法帝之路」三处修复的电脑端对比图。

目的（硬性流程约束）：界面改动先出对比图，作者确认后才允许烧录。
直接用与固件 main/mage.c / main/mage_fx.c / main/mage_hud.c **同一套坐标与颜色常量**画出来。

覆盖本次修复：
  ① 战斗中「切回自动」只在 BP_MENU/MAGIC/ITEM 处理，清场屏(BP_RESULT)与遭遇(BP_INTRO)没处理，
     且顶部 HUD 在 BP_RESULT/LOOT/INTRO 隐藏 → 看不到 AUTO/手动。
     -> 现在：ACT_LUP(长按↑) 全战斗阶段可切自动；BP_RESULT 时顶部 HUD 显示 AUTO/手动。
  ② 结算面板 box(20,150,*,170) 底边顶到 320 屏底，footer 被压在屏幕最下沿像被裁掉。
     -> 现在：box(20,120,*,166)，footer 留 16px 下边距。
  ③ 菜单/状态副标题用 C_DIM(0x6b6480) 太暗看不清。
     -> 现在：改用 C_SUB(0xc9c2e6) 亮藕荷色。

仅在预览里复刻「位置+颜色层面」，不渲染真实贴图（验证的是布局与对比度）。
产出 assets/preview/mage_fix_*.png（240x320 原生 + 2x 放大 + 一张 2x2 总览）。
"""
from __future__ import annotations
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "preview")
os.makedirs(OUT, exist_ok=True)

W, H = 240, 320

# ---- 与源码一致的配色 ----
C_BG    = (13, 11, 26)      # 0x0d0b1a
C_PANEL = (10, 8, 24)       # 0x0a0818 结算面板底
C_WHITE = (242, 238, 251)   # 0xf2eefb
C_GOLD  = (255, 217, 61)    # 0xffd93d
C_GRAY  = (154, 147, 181)   # 0x9a93b5
C_DIM   = (107, 100, 128)   # 0x6b6480 旧的副标题色（太暗）
C_SUB   = (201, 194, 230)   # 0xc9c2e6 新副标题色（亮藕荷）
C_CYAN  = (94, 200, 242)    # 0x5ec8f2 顶部 HUD 文本
C_REWARD= (204, 255, 204)   # 0xccffcc 结算明细


def font(px):
    for name in ("msyh.ttc", "simhei.ttf", "simsun.ttc"):
        for base in (r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu"):
            p = os.path.join(base, name)
            if os.path.exists(p):
                try:
                    return ImageFont.truetype(p, px)
                except Exception:
                    pass
    return ImageFont.load_default()


F9, F12, F13, F16 = (font(s) for s in (9, 12, 13, 16))

# 真实图标源（与固件 mage_sprites.c 烘焙同源：mage-game/assets/sp_icon_*.png，44x44 RGBA）
ICON_DIR = os.path.join(ROOT, "mage-game", "assets")
_ICON_CACHE = {}


def load_icon(key):
    """读取并缩放到固件显示的 22x22（与 lv_obj_set_size(ic,22,22) 一致）。"""
    if key in _ICON_CACHE:
        return _ICON_CACHE[key]
    p = os.path.join(ICON_DIR, key + ".png")
    im = Image.open(p).convert("RGBA").resize((22, 22), Image.LANCZOS)
    _ICON_CACHE[key] = im
    return im


def round_rect(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def cjk(s):
    return s  # 已用中文 ttf，直接画


# ---------------- 结算面板（问题①②） ----------------
def draw_result(d, im, box_y, box_h, footer_text, show_hud, label):
    # 战斗场景底（夜色）
    d.rectangle([0, 0, W, H], fill=C_BG)
    # 地面草丛暗示
    d.rectangle([0, 250, W, H], fill=(18, 22, 16))
    # 顶部 HUD（问题①：修复后 BP_RESULT 也显示，含 AUTO/手动）
    if show_hud:
        d.rectangle([0, 0, W, 24], fill=(20, 16, 40))
        d.text((8, 6), "HP 274/274", font=F9, fill=C_CYAN)
        d.text((120, 6), "MP 88/120", font=F9, fill=C_CYAN)
        d.text((8, 13), "试炼 4/6", font=F9, fill=C_WHITE)
        d.text((150, 2), "手动·2·1·3", font=F9, fill=C_GOLD)   # 状态行：手动/AUTO + 球/印/药
    else:
        d.text((8, 6), "(顶部 HUD 隐藏：看不到手动/AUTO)", font=F9, fill=C_DIM)
    # 结算面板
    bx, by, bw, bh = 20, box_y, 200, box_h
    round_rect(d, [bx, by, bx + bw, by + bh], 6, fill=C_PANEL, outline=C_GOLD, width=1)
    d.text((bx + bw // 2, by + 8), "第 4 波 清场（累计收益）", font=F13, fill=C_GOLD, anchor="mm")
    # 明细（两列）
    rows = ["击杀 史莱姆 x3", "获得 经验 +48", "金币 +26", "掉落 魔法草 x2", "伙伴 喵喵 Lv↑"]
    yy = by + 28
    for i, r in enumerate(rows):
        x = bx + 14 + (i % 2) * 96
        if i % 2 == 0 and i > 0:
            yy += 18
        col = bx + 14 if i % 2 == 0 else bx + 110
        d.text((col, yy), r, font=F12, fill=C_REWARD)
    # footer（问题②：应留边距，不被屏底裁掉）
    d.text((bx + bw // 2, by + 150), footer_text, font=F12, fill=C_GOLD, anchor="mm")
    # 标签：面板顶到屏底时放顶部，避免与 footer 重叠
    if box_y + box_h >= H - 20:
        d.text((W // 2, 36), label, font=F9, fill=(255, 120, 120), anchor="mm")
    else:
        d.text((W // 2, H - 14), label, font=F9, fill=C_CYAN, anchor="mm")


# ---------------- 菜单副标题对比（问题③④） ----------------
# 严格镜像固件布局：ROW_X=8 ROW_W=204 ROW_H=33 PITCH=34 Y0=27
# name(46,1) sub(46,17) 12px字库行高15；金框标题条 box(10,9,220x16)
# 图标 22x22 在 LEFT_MID+18（即 x=ROW_X+18，垂直居中对齐）
ROW_X, ROW_W, ROW_H, ROW_PITCH, ROW_Y0 = 8, 204, 33, 34, 27

# 真实「魔法小镇」主菜单数据（与固件 LOCATIONS[7] + CITY_LOC_ICON[7] 一一对应）
MENU_ROWS = [
    ("sp_icon_book",   "魔法学院", "修行 · 学魔法 · 晋升"),
    ("sp_icon_map",    "野外试炼", "多区域野外 · 遗迹秘境"),
    ("sp_icon_sword",  "冒险者公会", "委托任务 · 冒险者等级"),
    ("sp_icon_bed",    "住宅",     "休息 · 炼金 · 魔法研究"),
    ("sp_icon_hammer", "锻造房",   "武器 · 法袍锻造"),
    ("sp_icon_coin",   "魔法黑市", "素材 · 魔法书交易"),
    ("sp_icon_staff",  "魔物图鉴", "收录 · 出没 · 捕捉条件"),
]


def draw_menu(d, im, sub_color, label, geo):
    row_h, pitch, y0, name_dy, sub_dy = geo
    d.rectangle([0, 0, W, H], fill=C_BG)
    # 金框标题条（固件 frame 场景：box(10,9,220,16) + 金字 y=12）
    d.rectangle([10, 9, 10 + 220, 9 + 16], fill=(42, 35, 72), outline=(201, 162, 39))
    d.text((18, 12), "魔法小镇", font=F12, fill=C_GOLD)
    for i, (icon, name, sub) in enumerate(MENU_ROWS):
        ry = y0 + i * pitch
        sel = (i == 0)
        if sel:
            round_rect(d, [ROW_X, ry, ROW_X + ROW_W, ry + row_h], 0,
                       fill=(58, 44, 92), outline=C_GOLD, width=2)
            # 选中三角（固件 sp_tri 10x12 在 LEFT_MID+4）
            d.polygon([(ROW_X + 4, ry + 10), (ROW_X + 4, ry + 22), (ROW_X + 14, ry + 16)],
                      fill=C_GOLD)
        else:
            round_rect(d, [ROW_X, ry, ROW_X + ROW_W, ry + row_h], 0,
                       fill=(24, 18, 44), outline=C_GRAY, width=1)
        # 真实图标（22x22 垂直居中 = LEFT_MID）：top = ry + (row_h-22)//2
        ic = load_icon(icon)
        iy = ry + (row_h - 22) // 2
        im.paste(ic, (ROW_X + 18, iy), ic)
        d.text((ROW_X + 46, ry + name_dy), name, font=F12, fill=C_WHITE)
        d.text((ROW_X + 46, ry + sub_dy), sub, font=F12, fill=sub_color)
    d.text((W // 2, H - 12), label, font=F9, fill=C_CYAN, anchor="mm")


def make_cell(fn, *a):
    im = Image.new("RGB", (W, H), C_BG)
    d = ImageDraw.Draw(im)
    fn(d, im, *a)
    return im


def up2x(im):
    return im.resize((W * 2, H * 2), Image.NEAREST)


def main():
    # 单图
    cells = {
        "result_before": make_cell(draw_result, 150, 170, "A继续·B回城·长按↑自动", False,
                                   "修复前：面板顶到屏底，footer 被裁"),
        "result_after":  make_cell(draw_result, 120, 166, "A继续·B回城·长按↑自动", True,
                                   "修复后：上移留边距 + HUD 显示手动/AUTO"),
        "gray_before":   make_cell(draw_menu, C_DIM,
                                   "修复前：行高29 name(y+3) sub(y+16) 叠字+偏暗",
                                   (29, 30, 30, 3, 16)),   # 旧几何：行高29/PITCH30/Y0=30
        "gray_after":    make_cell(draw_menu, C_SUB,
                                   "修复后：行高33 name(y+1) sub(y+17) 分离+亮色",
                                   (33, 34, 27, 1, 17)),   # 新几何
    }
    for k, im in cells.items():
        im.save(os.path.join(OUT, f"mage_fix_{k}_240x320.png"))
        up2x(im).save(os.path.join(OUT, f"mage_fix_{k}_2x.png"))

    # 2x2 总览
    ov = Image.new("RGB", (W * 2, H * 2), C_BG)
    order = ["result_before", "result_after", "gray_before", "gray_after"]
    for idx, k in enumerate(order):
        cell = cells[k].resize((W, H))
        x = (idx % 2) * W
        y = (idx // 2) * H
        ov.paste(cell, (x, y))
        # 单元格边框
        d = ImageDraw.Draw(ov)
        d.rectangle([x, y, x + W - 1, y + H - 1], outline=C_GOLD, width=2)
    ov.save(os.path.join(OUT, "mage_fix_overview_480x640.png"))
    print("written:", os.path.join(OUT, "mage_fix_overview_480x640.png"))


if __name__ == "__main__":
    main()
