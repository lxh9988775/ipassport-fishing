#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
preview_pet.py - 口袋兔 v4 改进「电脑端对比图」生成器（纯预览，不参与固件编译）

用与固件同套坐标/素材渲染 240x320 模拟屏，输出两张对比图交用户确认：
  - pet_preview_naming.png  : A 轻引导/命名（左=改前无引导直进主屏，右=改后首次启动命名屏）
  - pet_preview_season.png  : C 昼夜/季节纯视觉（左=改前恒定白天，右=改后夜晚+冬季蓝调+雪帽）

依赖：Pillow + 源字体 SourceHanSansCN-Normal.otf（与 gen_font.py 同源）。
用法：python tools/preview_pet.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPR = os.path.join(ROOT, "assets", "sprites")
FONT = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
OTF = FONT if os.path.exists(FONT) else None

W, H = 240, 320
CREAM = (255, 246, 240)
PINK = (255, 222, 232)
RED = (196, 30, 80)
BROWN = (96, 64, 44)
GRAYTX = (120, 100, 90)
WHITE = (255, 255, 255)

_f = None
def font(sz=16):
    global _f
    if _f is None:
        _f = ImageFont.truetype(OTF, 16) if OTF else ImageFont.load_default()
    return _f

def load_room():
    p = os.path.join(SPR, "pet_room_preview.png")
    if os.path.exists(p):
        im = Image.open(p).convert("RGB").resize((W, H))
        return im
    # 退化：纯色房间
    return Image.new("RGB", (W, H), (220, 235, 210))

def load_rabbit():
    p = os.path.join(SPR, "rabbit_front.png")
    if os.path.exists(p):
        return Image.open(p).convert("RGBA")
    return None

def draw_home(base, tint=None, winter=False, name=None):
    """在 base(房间图) 上叠加主屏 UI，返回 PIL Image。tint=(r,g,b,opa) 可选。

    绘制层级严格对齐固件 build_home 的 z 序：
      房间背景 → 昼夜/季节叠色(g_tint, 沉底, 只压房间顶部 y<116)
      → 奶油文字面板 → HUD/状态条/列表 → 兔子/雪帽 → 季节标
    因此 tint 只给顶部房间区上氛围色，兔子与文字画在 tint 之上，绝不被盖（无遮挡）。
    """
    img = base.convert("RGBA")
    # 1) 昼夜/季节叠色层：只压房间背景图顶部（y<116），下半蒙版擦透明（与固件 g_tint 沉底一致）
    if tint:
        r, g, b, opa = tint
        ov = Image.new("RGBA", (W, H), (r, g, b, opa))
        ImageDraw.Draw(ov).rectangle([0, 116, W, H], fill=(0, 0, 0, 0))
        img = Image.alpha_composite(img, ov)
    d = ImageDraw.Draw(img, "RGBA")
    # 2) 文字衬底面板（盖住下半，与固件一致；在 tint 之上）
    d.rectangle([4, 116, 168, 178], fill=CREAM + (255,))
    d.rectangle([8, 176, 232, 308], fill=CREAM + (255,))
    # 3) HUD（在 tint 之上，不被染）
    d.text((14, 4), "等级 1", font=font(), fill=BROWN)
    d.text((92, 4), "开心", font=font(), fill=RED)
    # 4) 状态条（忠实固件：文字 x=8 起，条 x=40 起；预览用 TrueType 比设备点阵宽，
    #    故条起点取 44、宽 116，既与固件视觉一致又消除预览字体溢出造成的假遮挡）
    rows = [("饱食", 90), ("干净", 85), ("玩乐", 70), ("精力", 95)]
    for i, (lab, v) in enumerate(rows):
        y = 118 + i * 14
        d.rectangle([44, y, 160, y + 8], fill=(232, 220, 210, 255))
        d.rectangle([44, y, 44 + int(v * 116 / 100), y + 8], fill=(120, 210, 130, 255))
        d.text((8, y), lab, font=font(), fill=BROWN)
    # 5) 照顾列表
    items = ["喂食", "洗澡", "陪玩", "睡觉", "小游戏", "护照"]
    sel = 0
    d.rounded_rectangle([10, 178 + sel * 21, 218, 178 + sel * 21 + 22], radius=11, fill=RED)
    for i, it in enumerate(items):
        col = WHITE if i == sel else BROWN
        d.text((18, 179 + i * 21), it, font=font(), fill=col)
    # 6) 兔子（在 tint 之上，不被染）
    rb = load_rabbit()
    if rb:
        img.alpha_composite(rb, (84, 26))
        if winter:
            # 雪帽：复用草帽精灵当雪帽纯显示（与固件一致，不改真实 costume）
            cap = os.path.join(SPR, "costume_hat.png")
            if os.path.exists(cap):
                img.alpha_composite(Image.open(cap).convert("RGBA"), (84, 26))
    # 7) 季节字符（HUD 小标，在 tint 之上）
    if name is not None:
        d = ImageDraw.Draw(img, "RGBA")
        d.text((150, 4), name, font=font(), fill=BROWN)
    return img.convert("RGB")

def draw_home_v2(base, winter=False, name=None):
    """改版主屏（有设计感，坐标与固件 pet.c build_home 完全对齐）：
       - 状态卡 [4,116,236,196] + 细边框 + 四项配色圆点 + 同色进度条
       - 照顾卡 [4,200,236,316] + 选中红胶囊
    层级与固件一致：房间背景 -> tint(顶区) -> 卡片(不透明) -> 文字/兔子。"""
    img = base.convert("RGBA")
    if winter:
        ov = Image.new("RGBA", (W, H), (55, 75, 130, 56))
        ImageDraw.Draw(ov).rectangle([0, 116, W, H], fill=(0, 0, 0, 0))
        img = Image.alpha_composite(img, ov)
    d = ImageDraw.Draw(img, "RGBA")

    CARD = CREAM + (255,)
    BORD = (150, 110, 80, 255)

    # ---- 状态卡 [4,116,236,200] ----
    d.rounded_rectangle([4, 116, 236, 200], radius=14, fill=CARD, outline=BORD, width=2)
    d.text((16, 117), "状态", font=font(), fill=BROWN)
    stat = [("饱食", 90, (240, 160, 64)), ("干净", 85, (91, 168, 224)),
            ("玩乐", 70, (240, 111, 160)), ("精力", 95, (111, 192, 111))]
    for i, (lab, v, col) in enumerate(stat):
        y = 133 + i * 15          # 133,148,163,178
        d.ellipse([12, y - 1, 22, y + 9], fill=col + (255,))   # 配色圆点
        d.text((26, y), lab, font=font(), fill=BROWN)
        bx0, bx1 = 60, 218                              # bar 宽 158
        d.rounded_rectangle([bx0, y, bx1, y + 8], radius=4, fill=(232, 220, 210, 255))
        fw = int((bx1 - bx0) * v / 100)
        if fw > 4:
            d.rounded_rectangle([bx0, y, bx0 + fw, y + 8], radius=4, fill=col + (255,))

    # ---- 照顾卡 [4,204,236,316] ----
    d.rounded_rectangle([4, 204, 236, 316], radius=14, fill=CARD, outline=BORD, width=2)
    d.text((16, 205), "照顾", font=font(), fill=BROWN)
    items = ["喂食", "洗澡", "陪玩", "睡觉", "小游戏", "护照"]
    sel = 0
    for i, it in enumerate(items):
        y = 222 + i * 15          # 222,237,252,267,282,297
        if i == sel:
            d.rounded_rectangle([4, 220, 236, 238], radius=9, fill=RED)
            d.text((18, y), it, font=font(), fill=WHITE)
        else:
            d.text((18, y), it, font=font(), fill=BROWN)

    # ---- HUD ----
    d.text((14, 4), "等级 1", font=font(), fill=BROWN)
    d.text((92, 4), "开心", font=font(), fill=RED)
    if name is not None:
        d.text((150, 4), name, font=font(), fill=BROWN)

    # ---- 兔子（在卡片之上，干净）----
    rb = load_rabbit()
    if rb:
        img.alpha_composite(rb, (84, 26))
        if winter:
            cap = os.path.join(SPR, "costume_hat.png")
            if os.path.exists(cap):
                img.alpha_composite(Image.open(cap).convert("RGBA"), (84, 26))
    return img.convert("RGB")

def draw_intro(step):
    """首次启动引导屏（A 项）。step: 0 欢迎 / 1 起名 / 2 照顾提示。"""
    img = Image.new("RGBA", (W, H), CREAM + (255,))
    d = ImageDraw.Draw(img, "RGBA")
    if step == 0:
        d.text((W // 2 - 32, 24), "初次见面", font=font(), fill=RED)
        body = ["你好呀！", "我是你的口袋兔", "我会陪你一起长大"]
        for i, line in enumerate(body):
            d.text((W // 2 - 48, 70 + i * 24), line, font=font(), fill=BROWN)
        d.text((W // 2 - 48, H - 36), "按 OK 继续", font=font(), fill=GRAYTX)
    elif step == 1:
        d.text((W // 2 - 56, 24), "给它起个名字", font=font(), fill=RED)
        d.text((W // 2 - 56, 70), "用上下键选一个", font=font(), fill=BROWN)
        d.text((W // 2 - 40, 94), "按 OK 确定", font=font(), fill=BROWN)
        # 名字预设（团子高亮）
        names = ["团子", "豆豆", "毛毛", "小白", "点点", "球球"]
        for i, n in enumerate(names):
            col = RED if i == 0 else BROWN
            d.text((W // 2 - 18, 140 + i * 22), n, font=font(), fill=col)
        d.text((W // 2 - 52, H - 36), "↑↓选 · OK确定", font=font(), fill=GRAYTX)
    else:
        d.text((W // 2 - 40, 24), "照顾我吧", font=font(), fill=RED)
        body = ["喂食·洗澡·陪玩·睡觉", "把我照顾好", "还能换装、集贴纸！"]
        for i, line in enumerate(body):
            d.text((W // 2 - 64, 70 + i * 24), line, font=font(), fill=BROWN)
        d.text((W // 2 - 48, H - 36), "按 OK 开始", font=font(), fill=GRAYTX)
    return img.convert("RGB")

def side_by_side(left, right, llabel, rlabel, title):
    pad = 8
    tw = W * 2 + pad * 3
    th = H + 40
    canvas = Image.new("RGB", (tw, th), (240, 240, 240))
    canvas.paste(left, (pad, 36))
    canvas.paste(right, (pad * 2 + W, 36))
    d = ImageDraw.Draw(canvas)
    d.text((pad, 8), title, font=font(), fill=(40, 40, 40))
    d.text((pad, 36 + H + 6), llabel, font=font(), fill=GRAYTX)
    d.text((pad * 2 + W, 36 + H + 6), rlabel, font=font(), fill=GRAYTX)
    return canvas

def main():
    room = load_room()
    # naming.png: 改前(无引导直进主屏) vs 改后(首次启动命名屏)
    before_home = draw_home(room)
    after_intro = draw_intro(1)
    naming = side_by_side(before_home, after_intro,
                           "改前：无引导，直接进主屏", "改后：首次启动·起名屏(默认团子可改)",
                           "A 轻剧情引导 + 兔子人格（命名屏）")
    out1 = os.path.join(ROOT, "pet_preview_naming.png")
    naming.save(out1)
    print("written", out1)

    # season.png: 改前(恒定白天，无季节功能) vs 改后(夜晚+冬季蓝调+雪帽)
    before_season = draw_home(room, tint=None, winter=False, name=None)
    after_season = draw_home(room, tint=(55, 75, 130, 56), winter=True, name="冬")
    season = side_by_side(before_season, after_season,
                           "改前：恒定白天，无季节变化", "改后：夜晚+冬季蓝调，兔子戴雪帽",
                           "C 昼夜/季节纯视觉（不碰 pet_tick 衰减）")
    out2 = os.path.join(ROOT, "pet_preview_season.png")
    season.save(out2)
    print("written", out2)

    # home_redesign.png: 改前(现版朴素状态区) vs 改后(有设计感的状态卡+照顾卡)
    before_home = draw_home(room)
    after_home = draw_home_v2(room, winter=False, name=None)
    redesign = side_by_side(before_home, after_home,
                             "改前：朴素状态条(文字易与条重叠)",
                             "改后：状态卡+配色圆点+同色条+照顾卡",
                             "主屏改版（设计感提升，待确认）")
    out3 = os.path.join(ROOT, "pet_preview_home_redesign.png")
    redesign.save(out3)
    print("written", out3)

if __name__ == "__main__":
    main()
