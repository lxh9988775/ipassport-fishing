#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_mage_fixes.py —— 渲染「魔法帝之路」三处修复的电脑端对比图。

目的（硬性流程约束）：界面改动先出对比图，作者确认后才允许烧录。
直接用与固件 main/mage.c / main/mage_fx.c / main/mage_hud.c **同一套坐标与颜色常量**画出来。

覆盖本次修复：
  —— 前序批次 ——
  ① 战斗中「切回自动」全阶段可切；BP_RESULT 时顶部 HUD 显示 AUTO/手动。
  ② 结算面板上移留边距；非交互阶段(RESULT/LOOT/INTRO/EVENT/FAINT)藏底部三卡防盖 footer。
  ③ 菜单/状态副标题改用亮藕荷色 C_SUB。
  ④ 菜单行高 33、name(y+1)/sub(y+17) 分离防叠字；图标用真实 sp_icon_*。
  —— 第六批（用户板照炸裂反馈）——
  ⑤ 列表不铺满：frame 高度固定 270 + pitch 固定 34，学院 3 行下面大片空白。
     -> 现在：menu_list_render 动态 pitch=(LIST_BOTTOM-g_list_y0)/n 铺满、框高随行数自适应(封顶298)、行内容垂直居中。
  ⑥ 角色页/菜单右列裁切：「见习魔法师」(60px) 被旧 56px 右列裁成「凡习魔法师」，信息显示不全。
     -> 现在：右列宽 56→76、pos ROW_W-80，职级等右列文本完整显示。
  ⑦ 伙伴图鉴没图：PARTNER_THUMBS 硬编码 7 + 烘焙只 8 个 thumb → sp_treant_thumb 等 NULL 空白格。
     -> 现在：gen_mage_sprites 遍历全部魔物烘 84 个 _thumb，网格 8 格全有立绘。
  ⑧ 豆腐块缺字：10-15~10-23 加的文案(当前/可挑战等)及 emoji 从未进字库；check_cjk_coverage 只查钓鱼线漏查魔法线。
     -> 现在：重跑 gen_font_mage(1173 字形) + 扩展检查脚本覆盖魔法线(双线 PASS)；删 💥🔥 emoji toast。
     注：缺字属字库层缺陷，电脑端预览用真实中文 ttf 无法复现豆腐块，故该项的诚实证据是 check_cjk_coverage.py PASS。

仅在预览里复刻「位置+颜色层面」，不渲染真实贴图（验证的是布局与对比度）。
产出 assets/preview/mage_fix_*.png（240x320 原生 + 2x 放大 + 一张 2x2 总览）+ 第六批 mage_sixth_*.png（3×2 总览）。
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
def draw_action_cards(d, im, turn=2):
    """底部三张动作卡 + 回合徽标（与固件 mage_hud.c 同几何：x=8+i*78, y=276, 72x40）"""
    names = ["攻击", "魔法", "道具"]
    icons = ["sp_icon_sword", "sp_icon_staff", "sp_icon_potion"]
    c1 = [(255, 138, 61), (90, 138, 255), (61, 189, 122)]
    for i in range(3):
        x, y, w, h = 8 + i * 78, 276, 72, 40
        d.rectangle([x, y, x + w, y + h], fill=(36, 29, 74), outline=(138, 106, 42))
        d.rectangle([x + 2, y + 3, x + 4, y + 36], fill=c1[i])
        ic = load_icon(icons[i])
        im.paste(ic, (x + (w - 22) // 2, y + 2), ic)
        d.text((x + w // 2, y + 31), names[i], font=F9, fill=(255, 255, 255), anchor="mm")
    # 回合徽标（固件 turnBox: (W/2-30, CARD_Y-7, 60,14)）
    d.rectangle([W // 2 - 30, 269, W // 2 + 30, 283], fill=(13, 10, 26), outline=(201, 164, 74))
    d.text((W // 2, 276), f"回合 {turn}", font=F9, fill=C_GOLD, anchor="mm")


def draw_result(d, im, box_y, box_h, footer_text, show_hud, label, show_cards=False):
    # 战斗场景底（夜色）
    d.rectangle([0, 0, W, H], fill=C_BG)
    # 地面草丛暗示
    d.rectangle([0, 250, W, H], fill=(18, 22, 16))
    # 顶部 HUD（问题①：修复后 BP_RESULT 也显示，含 AUTO/手动）
    if show_hud:
        d.rectangle([0, 0, W, 24], fill=(20, 16, 40))
        d.text((82, 2), "102/104", font=F9, fill=(255, 255, 255))
        d.text((7, 3), "HP", font=F9, fill=(255, 136, 136))
        d.text((82, 13), "59/62", font=F9, fill=C_CYAN)
        d.text((7, 14), "MP", font=F9, fill=(124, 184, 255))
        d.text((W - 6, 1), "试炼 7/10", font=F9, fill=C_GOLD, anchor="ra")
        d.text((W - 6, 14), "AUTO·0·0·4", font=F9, fill=(127, 220, 138), anchor="ra")
    else:
        d.text((8, 6), "(顶部 HUD 隐藏：看不到手动/AUTO)", font=F9, fill=C_DIM)
    # 结算面板（固件：evtName y+8 / evtDesc 多行 y+28 / footer y+150）
    bx, by, bw, bh = 20, box_y, 200, box_h
    round_rect(d, [bx, by, bx + bw, by + bh], 6, fill=C_PANEL, outline=C_GOLD, width=1)
    d.text((bx + bw // 2, by + 8), "第 7 波 清场（累计收益）", font=F13, fill=C_GOLD, anchor="mm")
    rows = ["金币 +94", "经验 +201", "月光草 +5", "兽骨 +3", "药水 +2"]
    yy = by + 30
    for r in rows:
        d.text((bx + 14, yy), r, font=F12, fill=C_REWARD)
        yy += 19
    # footer（问题②：应留边距，不被屏底裁掉）
    d.text((bx + bw // 2, by + 150), footer_text, font=F12, fill=C_GOLD, anchor="mm")
    # 底部动作卡：修复前显示（盖住 footer，= 用户板上的现象）；修复后藏卡
    if show_cards:
        draw_action_cards(d, im, turn=7)
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


def draw_battle_menu(d, im, label):
    """战斗菜单阶段（BP_MENU）：证明三张动作卡照常显示，修复只藏结算/事件等非交互阶段。"""
    d.rectangle([0, 0, W, H], fill=C_BG)
    d.rectangle([0, 250, W, H], fill=(18, 22, 16))
    # 顶部 HUD（同 show_hud 分支的真实坐标）
    d.rectangle([0, 0, W, 24], fill=(20, 16, 40))
    d.text((82, 2), "102/104", font=F9, fill=(255, 255, 255))
    d.text((7, 3), "HP", font=F9, fill=(255, 136, 136))
    d.text((82, 13), "59/62", font=F9, fill=C_CYAN)
    d.text((7, 14), "MP", font=F9, fill=(124, 184, 255))
    d.text((W - 6, 1), "试炼 7/10", font=F9, fill=C_GOLD, anchor="ra")
    d.text((W - 6, 14), "AUTO·0·0·4", font=F9, fill=(127, 220, 138), anchor="ra")
    # 敌人区占位（战斗画面示意，验证的是卡不消失）
    d.rectangle([70, 60, 170, 160], fill=(30, 24, 58), outline=(90, 80, 130))
    d.text((W // 2, 110), "敌人", font=F12, fill=(120, 110, 160), anchor="mm")
    d.text((W // 2, 200), "OK攻击 · ↓魔法 · ↑道具", font=F12, fill=C_GOLD, anchor="mm")
    d.text((W // 2, 220), "长按↑自动/手动 · 长按OK回城", font=F9, fill=C_SUB, anchor="mm")
    # 三张动作卡：菜单阶段照常显示（= 修复后的行为）
    draw_action_cards(d, im, turn=7)
    d.text((W // 2, 250), "▲ 战斗菜单阶段：三卡照常显示", font=F9, fill=(127, 220, 138), anchor="mm")


def load_monster(type_key, th=40, max_w=44):
    """读取源怪物 PNG（mage-game/assets/sp_<type>.png，优先抠好透明底的 _k 版）
    并按烘焙期 make_thumb 同口径缩放。

    与固件 assets/mage_sprites.c 烘焙同源：scale=min(th/h, max_w/w) 限宽 44、高 40。
    """
    p = os.path.join(ICON_DIR, "sp_%s_k.png" % type_key)
    if not os.path.exists(p):
        p = os.path.join(ICON_DIR, "sp_%s.png" % type_key)
    im = Image.open(p).convert("RGBA")
    w, h = im.size
    scale = min(th / h, max_w / w)
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    return im.resize((nw, nh), Image.LANCZOS)


# ---------------- 第六批数据（与固件真实文案/物种一一对应） ----------------
# 学院页：3 行（g_menu.n=3），旧几何留下大片空白；新几何铺满。
ACADEMY_ROWS = [
    ("sp_icon_orb",   "魔力修行",     "魔力上限 +5（当前 104）", "20金"),
    ("sp_icon_book",  "学属性魔法",   "已学 12/135 种魔法",      "45金"),
    ("sp_icon_crown", "职业晋升考核", "需击杀/委托达标",         ""),
]
# 角色页【旧版】6 行（板上现状）：职级塞等级行右列，与长 sub 互相压字 + 顶部立绘占 88px
STATUS_ROWS_OLD = [
    ("sp_icon_heart", "生命", "104 / 104", ""),
    ("sp_icon_bolt",  "魔力", "59 / 62",   ""),
    ("sp_icon_star",  "等级", "Lv7 · 经验210/320", "见习魔法师"),
    ("sp_icon_sword", "攻击", "29（装+3）", ""),
    ("sp_icon_shield","护甲", "减伤 6",   ""),
    ("sp_icon_coin",  "金币", "1860 · 击杀 142", ""),
]
# 角色页【新版】7 行（对齐 HTML 原版：职级·Lv 独立信息、经验独立成行、无右列冲突）
STATUS_ROWS_NEW = [
    ("sp_icon_heart",  "生命", "104 / 104",          ""),
    ("sp_icon_bolt",   "魔力", "59 / 62",            ""),
    ("sp_icon_star",   "等级", "见习魔法师 · Lv7",   ""),
    ("sp_icon_scroll", "经验", "210 / 320",          ""),
    ("sp_icon_sword",  "攻击", "29（装+3）",         ""),
    ("sp_icon_shield", "护甲", "减伤 6",             ""),
    ("sp_icon_coin",   "金币", "1860 · 击杀 142",    ""),
]
# 伙伴网格：8 个真实物种（cat/fox/frog/deer/bat/boar/goblin/treant 均有源 PNG）。
PARTNERS = [
    ("cat",     "猫灵",     "优 Lv5"),
    ("fox",     "灵狐",     "极 Lv6"),
    ("frog",    "沼泽蛙",   "良 Lv4"),
    ("deer",    "斑鹿",     "良 Lv5"),
    ("bat",     "洞穴蝙蝠", "普 Lv3"),
    ("boar",    "獠牙野猪", "良 Lv5"),
    ("goblin",  "哥布林",   "普 Lv4"),
    ("treant",  "小树精",   "优 Lv7"),
]

C_CELL = (35, 27, 66)        # 0x231b42 网格单元底（与固件 lv_color_hex(0x231b42) 一致）


def draw_frame(d, frame_h, title):
    d.rectangle([10, 10, 230, 10 + frame_h], fill=C_CELL, outline=(201, 162, 39))
    d.rectangle([10, 9, 230, 25], fill=(42, 35, 72), outline=(201, 162, 39))
    d.text((18, 14), title, font=F12, fill=C_GOLD)


def draw_list_row(d, im, x, y, w, h, icon, name, sub, rgt,
                  rgt_w=76, rgt_xoff=80, sel=False, clip_rgt=False):
    if sel:
        round_rect(d, [x, y, x + w, y + h], 0, fill=(58, 44, 92), outline=C_GOLD, width=2)
        d.polygon([(x + 4, y + 10), (x + 4, y + 22), (x + 14, y + 16)], fill=C_GOLD)
    else:
        round_rect(d, [x, y, x + w, y + h], 0, fill=(24, 18, 44), outline=C_GRAY, width=1)
    ic = load_icon(icon)
    iy = y + (h - 22) // 2
    im.paste(ic, (x + 18, iy), ic)
    top = y + (h - 31) // 2
    d.text((x + 46, top + 1), name, font=F12, fill=C_WHITE)
    d.text((x + 46, top + 17), sub, font=F12, fill=C_SUB)
    rx = x + w - rgt_xoff
    ry = y + (h - 15) // 2
    if clip_rgt:
        # 模拟旧右列裁切：先画，再用单元格底色盖掉溢出部分，并画红裁切线标出旧 56px 边界
        d.text((rx, ry), rgt, font=F12, fill=C_GOLD)
        d.rectangle([rx + rgt_w, y, x + w - 2, y + h], fill=(24, 18, 44))
        d.line([rx + rgt_w, y, rx + rgt_w, y + h], fill=(255, 107, 107), width=2)
    else:
        d.text((rx, ry), rgt, font=F12, fill=C_GOLD)


# ---------------- 列表铺满（问题⑤：学院 3 行） ----------------
# 场景内不放任何标注文字（诚实渲染固件输出），说明放总览图的独立说明条。
def draw_academy(d, im, before):
    d.rectangle([0, 0, W, H], fill=C_BG)
    if before:
        # 旧：frame 固定 270 高，pitch 固定 34，3 行挤在顶部，下方大片空白
        draw_frame(d, 270, "魔法学院")
        y0, pitch, row_h = 27, 34, 33
        for i, (ic, nm, sub, rgt) in enumerate(ACADEMY_ROWS):
            draw_list_row(d, im, ROW_X, y0 + i * pitch, ROW_W, row_h, ic, nm, sub, rgt, sel=(i == 0))
    else:
        # 新：pitch=(294-27)/3≈89→cap88，rowH=87，框高随内容自适应到 284
        pitch = (294 - 27) // len(ACADEMY_ROWS)
        if pitch > 88:
            pitch = 88
        row_h = pitch - 1
        fb = 27 + len(ACADEMY_ROWS) * pitch + 3
        if fb > 298:
            fb = 298
        frame_h = fb - 10
        draw_frame(d, frame_h, "魔法学院")
        for i, (ic, nm, sub, rgt) in enumerate(ACADEMY_ROWS):
            draw_list_row(d, im, ROW_X, 27 + i * pitch, ROW_W, row_h, ic, nm, sub, rgt, sel=(i == 0))
    d.text((W // 2, H - 12), "OK 确定 · ↓ 切换 · 长按 返回", font=F9, fill=C_CYAN, anchor="mm")


# ---------------- 角色页（问题⑥：职级压字 + 显示不全） ----------------
def draw_status(d, im, before):
    d.rectangle([0, 0, W, H], fill=C_BG)
    if before:
        # 旧：立绘占顶部 88px（列表 y0=88）+ 6 行 + 职级塞右列与长 sub 压字
        rows = STATUS_ROWS_OLD
        y0 = 88
        pitch = (294 - y0) // len(rows)
        if pitch < ROW_H:
            pitch = ROW_H
        row_h = pitch - 1
        fb = y0 + len(rows) * pitch + 3
        if fb > 298:
            fb = 298
        draw_frame(d, fb - 10, "角色")
        # 立绘 = foreground 对象（在框体之上），必须在 draw_frame 之后粘贴
        try:
            hero = load_monster("hero", th=58, max_w=54)
            im.paste(hero, (W // 2 - hero.size[0] // 2, 88 - hero.size[1]), hero)
        except Exception:
            pass
        for i, (ic, nm, sub, rgt) in enumerate(rows):
            draw_list_row(d, im, ROW_X, y0 + i * pitch, ROW_W, row_h, ic, nm, sub, rgt, sel=(i == 0))
    else:
        # 新：去立绘、y0=27、7 行动态铺满，职级·Lv 并入等级行 sub，经验独立成行
        rows = STATUS_ROWS_NEW
        y0 = 27
        pitch = (294 - y0) // len(rows)
        if pitch > 88:
            pitch = 88
        if pitch < ROW_H:
            pitch = ROW_H
        row_h = pitch - 1
        fb = y0 + len(rows) * pitch + 3
        if fb > 298:
            fb = 298
        draw_frame(d, fb - 10, "角色")
        for i, (ic, nm, sub, rgt) in enumerate(rows):
            draw_list_row(d, im, ROW_X, y0 + i * pitch, ROW_W, row_h, ic, nm, sub, rgt, sel=(i == 0))
    d.text((W // 2, H - 12), "伙伴 3 只 · OK/长按 回城 · ↓ 管理装备", font=F9, fill=C_CYAN, anchor="mm")


# ---------------- 伙伴网格有图（问题⑦） ----------------
def draw_partners(d, im, before):
    d.rectangle([0, 0, W, H], fill=C_BG)
    # 镜像固件 g_part_root(10,26,220,250) + 4×2 单元(cw54,ch100,size50×96,pos(2+col*54,2+rowi*100))
    d.rectangle([10, 9, 230, 25], fill=(42, 35, 72), outline=(201, 162, 39))
    d.text((18, 14), "伙伴", font=F12, fill=C_GOLD)
    cw, ch, cell_inner_w = 54, 100, 50
    for i, (typ, nm, tag) in enumerate(PARTNERS):
        col = i % 4
        rowi = i // 4
        cx = 10 + 2 + col * cw
        cy = 26 + 2 + rowi * ch
        cell = (cx, cy, cx + cell_inner_w, cy + (ch - 4))
        round_rect(d, cell, 0, fill=C_CELL, outline=C_GRAY, width=1)
        if before:
            # 旧：立绘 NULL，格子只有名/角标，中间是空白
            d.rectangle([cx + 4, cy + 6, cx + cell_inner_w - 4, cy + 56],
                        outline=(90, 80, 130), width=1)
            d.text((cx + cell_inner_w // 2, cy + 31), "✕ 无立绘", font=F9, fill=(150, 140, 180), anchor="mm")
        else:
            mon = load_monster(typ)
            mw, mh = mon.size
            mx = cx + (cell_inner_w - mw) // 2
            my = cy + 8 + (40 - mh) // 2
            im.paste(mon, (mx, my), mon)
        d.text((cx + cell_inner_w // 2, cy + 66), nm, font=F9, fill=C_WHITE, anchor="mm")
        d.text((cx + cell_inner_w // 2, cy + 82), tag, font=F9, fill=C_GOLD, anchor="mm")
    d.text((W // 2, 280), "OK 出战 · ↓ 切换 · 长按 菜单", font=F9, fill=C_CYAN, anchor="mm")


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
                                   "修复前：面板顶到屏底 + 动作卡盖住 footer", True),
        "result_after":  make_cell(draw_result, 120, 166, "A继续·B回城·长按↑自动", True,
                                   "修复后：上移留边距 + HUD 显示 + 非交互阶段藏卡", False),
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

    # 战斗菜单阶段：证明动作卡在可交互阶段照常显示（修复不影响）
    bm = make_cell(draw_battle_menu, "战斗菜单阶段：三卡照常显示（修复只藏结算/事件阶段的卡）")
    bm.save(os.path.join(OUT, "mage_fix_battle_menu_240x320.png"))
    up2x(bm).save(os.path.join(OUT, "mage_fix_battle_menu_2x.png"))

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

    # ---------------- 第六批对比图（用户板照炸裂反馈） ----------------
    sixth = {
        "academy_before": make_cell(draw_academy, True),
        "academy_after":  make_cell(draw_academy, False),
        "status_before":  make_cell(draw_status, True),
        "status_after":   make_cell(draw_status, False),
        "partners_before": make_cell(draw_partners, True),
        "partners_after":  make_cell(draw_partners, False),
    }
    for k, im in sixth.items():
        im.save(os.path.join(OUT, "mage_sixth_%s_240x320.png" % k))
        up2x(im).save(os.path.join(OUT, "mage_sixth_%s_2x.png" % k))

    # 3×2 总览：每格 = 240x320 场景 + 下方 30px 独立说明条（不压场景）
    CAP_H = 30
    labels6 = {
        "academy_before":  ("修复前", "3 行挤在顶部，下方大片空白"),
        "academy_after":   ("修复后", "行距动态铺满整框，无空白"),
        "status_before":   ("修复前", "职级塞右列，与经验数字压字"),
        "status_after":    ("修复后", "职级并入等级行，经验独立行"),
        "partners_before": ("修复前", "只烘 8 个缩略图，多数空白格"),
        "partners_after":  ("修复后", "全部 84 只魔物都有缩略图"),
    }
    cap_color = {"修复前": (255, 120, 120), "修复后": (127, 220, 138)}
    OW, OH = W * 3, (H + CAP_H) * 2
    ov6 = Image.new("RGB", (OW, OH), (16, 12, 32))
    dd = ImageDraw.Draw(ov6)
    dd.rectangle([0, 0, OW, 22], fill=(42, 35, 72))
    dd.text((OW // 2, 11), "魔法帝之路 · 第六批界面修复对比（240x320 与固件同坐标）",
            font=F12, fill=C_GOLD, anchor="mm")
    order6 = ["academy_before", "academy_after",
              "status_before", "status_after",
              "partners_before", "partners_after"]
    for idx, k in enumerate(order6):
        col, row = idx % 3, idx // 3
        x, y = col * W, 24 + row * (H + CAP_H)
        ov6.paste(sixth[k], (x, y))
        dd.rectangle([x, y, x + W - 1, y + H - 1], outline=C_GOLD, width=1)
        # 说明条：深底 + 金顶线 + 徽章色前缀
        cy = y + H
        dd.rectangle([x, cy, x + W - 1, cy + CAP_H - 1], fill=(24, 18, 44))
        dd.line([x, cy, x + W - 1, cy], fill=C_GOLD, width=1)
        tag, desc = labels6[k]
        dd.text((x + 6, cy + 8), tag, font=F9, fill=cap_color[tag])
        tw = dd.textlength(tag, font=F9)
        dd.text((x + 12 + tw, cy + 8), desc, font=F9, fill=C_WHITE)
    ov6.save(os.path.join(OUT, "mage_sixth_overview_720x704.png"))
    print("written:", os.path.join(OUT, "mage_sixth_overview_720x704.png"))


if __name__ == "__main__":
    main()
