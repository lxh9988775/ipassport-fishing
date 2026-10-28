#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_mage_partners_menu.py
渲染「魔法帝之路」伙伴页长按菜单 + 放生二次确认的电脑端对比图。

硬性流程约束：界面改动先出对比图，作者确认后才允许烧录。
直接用与固件 main/mage.c **同一套坐标常量**画出来（g_part_menu / g_part_conf 几何）。

覆盖本次修复：
  A. 长按 OK 弹出的「伙伴菜单」必须可见（之前逻辑开着却从不渲染，菜单"看不见"）。
  B. 菜单默认选中"关闭菜单"（安全项），放生需二次确认面板才生效，杜绝误删。

产出 assets/preview/mage_partner_*.png（240x320 原生 + 2x 放大 + 一张 3 联总览）。
"""
from __future__ import annotations
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import preview_mage_fixes as pm  # noqa: E402

OUT = pm.OUT
os.makedirs(OUT, exist_ok=True)

W, H = pm.W, pm.H
C_BG = pm.C_BG
C_CELL = pm.C_CELL
C_GOLD = pm.C_GOLD
C_WHITE = pm.C_WHITE
C_CYAN = pm.C_CYAN
F9, F12 = pm.F9, pm.F12
load_monster = pm.load_monster

# 伙伴网格真实物种（与固件 PARTNERS 一致）
PARTNERS = [
    ("cat", "猫灵", "优 Lv5"), ("fox", "灵狐", "极 Lv6"),
    ("frog", "沼泽蛙", "良 Lv4"), ("deer", "斑鹿", "良 Lv5"),
    ("bat", "洞穴蝙蝠", "普 Lv3"), ("boar", "獠牙野猪", "良 Lv5"),
    ("goblin", "哥布林", "普 Lv4"), ("treant", "小树精", "优 Lv7"),
]


def bg_and_title(d):
    d.rectangle([0, 0, W, H], fill=C_BG)
    # 金框标题条（固件 frame 场景：box(10,8,220,18) + 金字 y=11）
    d.rectangle([10, 8, 230, 26], fill=(42, 35, 72), outline=(201, 162, 39))
    d.text((18, 11), "伙伴", font=F12, fill=C_GOLD)


def draw_partner_grid(d, im):
    cw, ch, ciw = 54, 100, 50
    for i, (typ, nm, tag) in enumerate(PARTNERS):
        col = i % 4
        rowi = i // 4
        cx = 10 + 2 + col * cw
        cy = 26 + 2 + rowi * ch
        cell = (cx, cy, cx + ciw, cy + (ch - 4))
        sel = (i == 0)
        d.rectangle(cell, fill=(58, 44, 92) if sel else C_CELL,
                    outline=C_GOLD if sel else pm.C_GRAY, width=2 if sel else 1)
        mon = load_monster(typ)
        mw, mh = mon.size
        mx = cx + (ciw - mw) // 2
        my = cy + 8 + (40 - mh) // 2
        im.paste(mon, (mx, my), mon)
        d.text((cx + ciw // 2, cy + 66), nm, font=F9, fill=C_WHITE, anchor="mm")
        d.text((cx + ciw // 2, cy + 82), tag, font=F9, fill=C_GOLD, anchor="mm")
    d.text((W // 2, 280), "OK 出战 · ↓ 切换 · 长按 菜单", font=F9, fill=C_CYAN, anchor="mm")


def draw_menu(d, im, sel_idx):
    """伙伴操作菜单面板（g_part_menu: 20,78,200,150）。grid 被隐藏，只留暗底 + 面板。"""
    opts = ["进化", "放生", "返回城市", "关闭菜单"]
    bx, by, bw, bh = 20, 78, 200, 150
    d.rectangle([bx, by, bx + bw, by + bh], fill=(20, 13, 40), outline=C_GOLD, width=2)
    d.text((bx + bw // 2, by + 8), "伙伴菜单", font=F12, fill=C_GOLD, anchor="mm")
    for i, o in enumerate(opts):
        oy = by + 32 + i * 26
        fill = C_GOLD if i == sel_idx else C_WHITE
        d.text((bx + bw // 2, oy), o, font=F12, fill=fill, anchor="mm")
        if i == sel_idx:
            d.polygon([(bx + 24, oy - 5), (bx + 24, oy + 7), (bx + 34, oy + 1)], fill=C_GOLD)
    d.text((W // 2, 280), "↑↓ 选择 · OK 确定 · 返回 关闭", font=F9, fill=C_CYAN, anchor="mm")


def draw_confirm(d, im, name):
    bx, by, bw, bh = 20, 108, 200, 92
    d.rectangle([bx, by, bx + bw, by + bh], fill=(42, 13, 24), outline=(255, 85, 85), width=2)
    d.text((bx + bw // 2, by + 16), "放生 %s？" % name, font=F12, fill=C_WHITE, anchor="mm")
    d.text((bx + bw // 2, by + 58), "OK 确认放生 · 返回 取消", font=F9, fill=(201, 194, 230), anchor="mm")


def make(fn, *a):
    im = __import__("PIL.Image", fromlist=["Image"]).new("RGB", (W, H), C_BG)
    d = __import__("PIL.ImageDraw", fromlist=["ImageDraw"]).Draw(im)
    fn(d, im, *a)
    return im


def up2x(im):
    return im.resize((W * 2, H * 2), __import__("PIL.Image", fromlist=["Image"]).NEAREST)


def main():
    shots = {
        "mage_partner_normal": make(lambda d, im: (bg_and_title(d), draw_partner_grid(d, im))),
        # 默认选中"关闭菜单"（最后一项，安全默认，杜绝误删）
        "mage_partner_menu": make(lambda d, im: (bg_and_title(d), draw_menu(d, im, 3))),
        # 演示：选中"放生"时高亮跟随（确认前还需二次确认面板）
        "mage_partner_menu_release": make(lambda d, im: (bg_and_title(d), draw_menu(d, im, 1))),
        "mage_partner_confirm": make(lambda d, im: (bg_and_title(d), draw_confirm(d, im, "猫灵"))),
    }
    for k, im in shots.items():
        im.save(os.path.join(OUT, k + "_240x320.png"))
        up2x(im).save(os.path.join(OUT, k + "_2x.png"))

    # 3 联总览（2x 放大 + 标注）
    Z, gap, lab = 2, 24, 30
    cw, ch = W * Z, H * Z + lab
    sheet = __import__("PIL.Image", fromlist=["Image"]).new("RGB",
        (gap + 4 * (cw + gap), gap + ch), (240, 242, 246))
    ds = __import__("PIL.ImageDraw", fromlist=["ImageDraw"]).Draw(sheet)
    lf = pm.font(20)
    titles = [
        "① 伙伴页（长按 OK 前）：4×2 立绘网格",
        "② 长按 OK：弹出可见「伙伴菜单」（默认选中关闭菜单）",
        "③ 选中放生→OK：先弹红框二次确认，绝不秒删",
        "④ 确认面板：OK 才放生 · 返回 取消",
    ]
    order = ["mage_partner_normal", "mage_partner_menu",
             "mage_partner_menu_release", "mage_partner_confirm"]
    for i, k in enumerate(order):
        x = gap + i * (cw + gap)
        y = gap
        big = up2x(shots[k])
        sheet.paste(big, (x, y + lab))
        ds.text((x + 2, y + 4), titles[i], font=lf, fill=(30, 36, 48))
    out = os.path.join(OUT, "mage_partner_menu_overview.png")
    sheet.save(out)
    print("saved", out, sheet.size)
    for k in order:
        print("  ", os.path.join(OUT, k + "_240x320.png"))


if __name__ == "__main__":
    main()
