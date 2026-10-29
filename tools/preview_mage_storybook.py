#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
preview_mage_storybook.py - 在 PC 上用与固件一致的 240x320 坐标渲染「故事书过场页」，
输出 PNG 供总监/用户核对版式（不进固件，仅预览）。

布局 1:1 对齐 mage.c 的 SC_STORY 渲染：
  - 全屏手稿图 story_N.png 沉底（240x320）
  - 顶部金色标题「第X章 · 章节名」/「终焉之塔 · 真结局」
  - 底部深色面板（0x140e26 @75%）叠 vignette 文案 + 评级
  - 最底操作提示「A 继续 →」/「A 翻页 →」

用法：python tools/preview_mage_storybook.py
产物：mage-game/preview/storybook_pN.png（N=1..6）
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "mage-game", "assets")
OTF = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
OUTDIR = os.path.join(ROOT, "mage-game", "preview")
os.makedirs(OUTDIR, exist_ok=True)

W, H = 240, 320
CHAPTERS = ["试炼森林", "幽暗洞窟", "灰烬火山", "霜风雪原", "浮空遗迹"]
VIGNETTE = [
    "阿辰握紧橡木杖，踏入雾气弥漫的试炼森林。哥布林王的狞笑回荡林间——夺回森之符，是少年成为魔法师的第一步。",
    "幽暗洞窟的白骨铺成小径，上一任见习魔法师长眠于此。阿辰咬紧牙关：这一次，由他来终结骸骨领主的亡者大军。",
    "灰烬火山的核心在颤抖，熔岩巨像睁开通红的熔金之眼。阿辰顶着热浪上前——三块炎之心，必须夺回。",
    "霜风雪原的冰湖结着寒霜，霜牙狼王咬碎了冬之铃，春天迟迟不来。阿辰呼出白气：这一击，要让嚎声永远停息。",
    "浮空遗迹的最顶层，遗迹守卫神睁开了所有眼睛。它守护着魔法帝之冠。阿辰踏上最后一级台阶：五道封印，今夜终结。",
    "魔法帝之冠落定额心。阿辰望向云海之外的终焉之塔——那里，登顶者方能配称真正的魔法帝。新的征途，才刚刚开始。",
]
STARS = [1, 2, 3, 3, 2, 3]  # 模拟评星（预览用，仅展示不同评级版式）

GOLD = (255, 217, 61)
WHITE = (242, 238, 251)
SUB = (154, 143, 192)
PANEL = (20, 14, 38)
PANEL_A = int(0.75 * 255)


def font(sz):
    return ImageFont.truetype(OTF, sz)


def wrap(text, per_line):
    out = []
    for para in text.split("\n"):
        if not para:
            out.append("")
            continue
        line = ""
        for ch in para:
            line += ch
            if len(line) >= per_line:
                out.append(line)
                line = ""
        if line:
            out.append(line)
    return out


def render(page):
    img = Image.open(os.path.join(ASSETS, "story_%d.png" % page)).convert("RGB").resize((W, H), Image.LANCZOS)
    d = ImageDraw.Draw(img, "RGBA")

    # 标题
    if page == 6:
        title = "终焉之塔 · 真结局"
    else:
        title = "第%d章 · %s" % (page, CHAPTERS[page - 1])
    tf = font(16)
    tb = d.textbbox((0, 0), title, font=tf)
    tw = tb[2] - tb[0]
    d.text(((W - tw) // 2, 12), title, font=tf, fill=GOLD)

    # 底部面板
    py0, py1 = 178, H
    d.rectangle([0, py0, W - 1, py1 - 1], fill=(PANEL[0], PANEL[1], PANEL[2], PANEL_A))

    # 正文（vignette + 评级）
    st = STARS[page - 1]
    starstr = (st >= 3 and "评级：三星（无伤·零道具·速攻）") or (st == 2 and "评级：二星") or "评级：一星"
    body = VIGNETTE[page - 1] + "\n\n" + starstr
    bf = font(13)
    lines = wrap(body, 15)
    y = py0 + 8
    for ln in lines:
        d.text((18, y), ln, font=bf, fill=WHITE)
        y += 18

    # 提示
    hf = font(12)
    hint = (page == 5 and "A 翻页 →") or "A 继续 →"
    hb = d.textbbox((0, 0), hint, font=hf)
    hw = hb[2] - hb[0]
    d.text(((W - hw) // 2, H - 20), hint, font=hf, fill=SUB)

    out = os.path.join(OUTDIR, "storybook_p%d.png" % page)
    img.save(out)
    print("saved", out)


def main():
    for p in range(1, 7):
        render(p)
    print("done ->", OUTDIR)


if __name__ == "__main__":
    main()
