#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
compare_mage_stages.py - 逐环节 1:1 对比图（HTML 权威基准 vs 固件预期图）。

左列 = 真实 HTML（mage-game/index.html 经 Playwright 抓帧，assets/preview/html_*.png）
右列 = 固件预期（tools/preview_mage_battle.py 用同一套坐标常量渲染，assets/preview/mage_*.png）

产出：
  assets/preview/mage_compare_grid_1v1.png   总网格（2 列场景 × 4 行，每行一个环节）
  assets/preview/pairs/pair_<stage>.png      单环节放大对比（方便逐张细看）
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRE = os.path.join(ROOT, "assets", "preview")
PAIR_DIR = os.path.join(PRE, "pairs")
os.makedirs(PAIR_DIR, exist_ok=True)

W, H = 240, 320
SCALE = 2

# （环节名, HTML 图, 固件图, 说明）
STAGES = [
    ("battle",     "html_battle_240x320.png",      "mage_hud_240x320.png",      "战斗·普通波次（杂兵+HUD+动作卡）"),
    ("battle_boss","html_battle_boss_240x320.png", "mage_battle_boss_240x320.png", "战斗·BOSS 战（问题②③⑤）"),
    ("event_chest","html_event_chest_240x320.png", "mage_event_240x320.png",    "探索事件·宝箱（问题①）"),
    ("loot_chest", "html_loot_chest_240x320.png",  "mage_loot_chest_240x320.png", "宝箱战利品（bug⑥ 只给钱→具体物品）"),
    ("loot_herb",  "html_loot_herb_240x320.png",   "mage_loot_herb_240x320.png", "采集收获（bug⑥）"),
    ("result",     "html_result_240x320.png",      "mage_result_240x320.png",   "每波结算·显示素材（bug⑥）"),
    ("expo",       "html_expo_240x320.png",        "mage_expo_240x320.png",     "远征汇总·BOSS 装备（bug⑥）"),
    ("town",       "html_town_240x320.png",        "mage_town_240x320.png",     "城镇界面·文字可见（问题④）"),
]


def font(sz):
    for name in ("msyh.ttc", "msyhbd.ttc", "simhei.ttf"):
        try:
            return ImageFont.truetype("C:/Windows/Fonts/" + name, sz)
        except OSError:
            continue
    return ImageFont.load_default()


F_TITLE = font(22)
F_CAP = font(18)
F_TAG = font(15)

GOLD = (255, 217, 61)
DIM = (170, 160, 200)
BG = (24, 18, 40)
HDR = (36, 28, 62)


def cell(im_path, tag):
    """载入一列图并放大 2x，加顶部标签条。"""
    im = Image.open(os.path.join(PRE, im_path)).convert("RGB")
    if im.size != (W, H):
        im = im.resize((W, H))
    im = im.resize((W * SCALE, H * SCALE), Image.NEAREST)
    header = Image.new("RGB", (W * SCALE, 34), HDR)
    dh = ImageDraw.Draw(header)
    dh.text((W * SCALE // 2, 6), tag, font=F_TAG, fill=(230, 225, 255), anchor="ma")
    out = Image.new("RGB", (W * SCALE, H * SCALE + 34), BG)
    out.paste(header, (0, 0))
    out.paste(im, (0, 34))
    return out


def pair_image(stage, html_png, mage_png, caption):
    """单个环节：左 HTML / 右 固件，中间分隔，顶部环节标题。"""
    left = cell(html_png, "HTML 原版（权威基准）")
    right = cell(mage_png, "固件预期（240x320）")
    gap = 24
    pw = left.width + gap + right.width
    ph = 56 + left.height
    canvas = Image.new("RGB", (pw, ph), BG)
    d = ImageDraw.Draw(canvas)
    d.text((pw // 2, 12), caption, font=F_CAP, fill=GOLD, anchor="ma")
    canvas.paste(left, (0, 56))
    canvas.paste(right, (left.width + gap, 56))
    d.text((left.width + gap // 2, ph // 2 + 20), "▶", font=F_TITLE, fill=DIM, anchor="mm")
    return canvas


def main():
    pair_paths = []
    for stage, html_png, mage_png, caption in STAGES:
        p = pair_image(stage, html_png, mage_png, caption)
        out = os.path.join(PAIR_DIR, "pair_%s.png" % stage)
        p.save(out)
        pair_paths.append((stage, caption, p))
        print("输出：", out)

    # 总网格：2 列环节 × 4 行
    cols, gap = 2, 30
    cw = pair_paths[0][2].width + gap
    ch = pair_paths[0][2].height + gap
    rows = (len(pair_paths) + cols - 1) // cols
    canvas = Image.new("RGB", (cw * cols, ch * rows + 10), BG)
    d = ImageDraw.Draw(canvas)
    d.text((canvas.width // 2, 14), "魔法帝之路 · 逐环节 1:1 对比（左=HTML 权威基准，右=固件预期）",
           font=F_TITLE, fill=GOLD, anchor="ma")
    y0 = 60
    for i, (stage, caption, p) in enumerate(pair_paths):
        r, c = divmod(i, cols)
        canvas.paste(p, (c * cw + gap // 2, y0 + r * ch))
    out = os.path.join(PRE, "mage_compare_grid_1v1.png")
    canvas.save(out)
    print("输出：", out, "(%dx%d)" % canvas.size)


if __name__ == "__main__":
    main()
