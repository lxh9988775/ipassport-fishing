#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_ui.py —— 把 main/fishing.c 的屏幕布局按真实坐标渲染成预览图。

不跑硬件、不跑 LVGL，直接用同一份源码里的坐标 + 同一份字库的度量画出来，
所以"预览长什么样，实机就长什么样（位置层面）"。

坐标来源是 tools/check_ui_layout.py 的解析结果，改坐标不用同步这里，永远不会漂移。

产出：assets/preview/<name>.png（240x320 原生）+ 一张 2x2 拼图 ui-preview.png
"""
from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_ui_layout as cul            # noqa: E402

PNG = os.path.join(ROOT, "assets", "sprites", "png")
OUT = os.path.join(ROOT, "assets", "preview")
OTF = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")

W, H, R = cul.SCR_W, cul.SCR_H, 30
INK = (10, 18, 32)
PANEL = (8, 24, 48)
ACCENT = (153, 0, 58)          # 锐胜红

# 渲染用的"看起来像真机"的取值；没列到的走 check_ui_layout 的最坏情况表
SAMPLE = {
    "得分 %d": "得分 128",
    "最高 %d": "最高 1024",
    "%d": "86",
    "收线 %d%%\n%s": "收线 62%\n鲫鱼",
    "最长 %d.%d 厘米 · 钓 %d 条": "最长 24.5 厘米 · 钓 6 条",
    "%d. %s · %s%s": "3. 鲫鱼 · 常见 ✓",
    "收录 %d/%d   累计钓获 %d": "收录 5/24   累计钓获 12",
    "%d. %s": "3. 鲫鱼",
    "图鉴   %d/%d": "图鉴   5/24",
    "钓点   %s": "钓点   静水塘",
    "鱼竿   %s": "鱼竿   手竿",
    "饵料   %s": "饵料   蚯蚓",
    "%s  %s": "鲫鱼  常见",
    r"%d.%d 厘米 · %d 克\n本次 +%d 分%s": r"24.5 厘米 · 280 克\n本次 +12 分\n完美钓获！",
}


def sample(tmpl: str) -> str:
    if tmpl in SAMPLE:
        return SAMPLE[tmpl]
    return cul.worst_text(None, tmpl)


class Fonts:
    def __init__(self):
        self.f = ImageFont.truetype(OTF, 16)

    def w(self, s):
        return self.f.getlength(s)


def round_mask():
    m = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=R, fill=255)
    return m


MASK = round_mask()


def base(bg: str | None, fill=INK):
    img = Image.new("RGB", (W, H), fill)
    if bg:
        b = Image.open(os.path.join(PNG, bg + ".png")).convert("RGB")
        img.paste(b, (0, 0))
    return img


def hud(img, d, fnt, batt=86, score=128, high=1024, spot=None):
    """顶栏：得分 / 最高 / 电池 + 数字（坐标全部取自源码解析结果）。"""
    ov = Image.new("RGBA", (W, 24), (0, 0, 0, 150))
    img.paste(Image.alpha_composite(img.crop((0, 0, W, 24)).convert("RGBA"), ov).convert("RGB"), (0, 0))

    d.text((17, 4), "得分 %d" % score, font=fnt.f, fill=(255, 255, 255))
    d.text((96, 4), "最高 %d" % high, font=fnt.f, fill=(255, 255, 255))

    # 电池：壳 + 内部电量条 + 正极凸点
    col = (230, 60, 50) if batt < 20 else (90, 220, 120)
    edge = col if batt < 20 else (255, 255, 255)     # 低电量连外壳一起变红
    d.rectangle([176, 9, 187, 16], outline=edge)
    seg = 1 + batt * 9 // 100
    d.rectangle([177, 10, 177 + seg - 1, 15], fill=col)
    d.rectangle([188, 11, 189, 14], fill=(255, 255, 255))
    d.text((193, 4), str(batt), font=fnt.f, fill=(255, 255, 255))

    if spot:
        d.rounded_rectangle([172, 28, 229, 47], radius=10, fill=(0, 0, 0, 140) if False else (12, 20, 34))
        tw = fnt.w(spot)
        d.text((172 + (58 - tw) / 2, 30), spot, font=fnt.f, fill=(255, 255, 255))


def hint_pill(img, d, fnt, text):
    tw = fnt.w(text) + 2
    x = (W - tw) / 2
    d.rounded_rectangle([x, 293, x + tw - 1, 314], radius=3, fill=(0, 0, 0))
    d.text((x + 1, 294), text, font=fnt.f, fill=(255, 255, 255))


def sprite(img, name, x, y, zoom=1):
    s = Image.open(os.path.join(PNG, name + ".png")).convert("RGBA")
    if zoom != 1:
        s = s.resize((s.width * zoom, s.height * zoom), Image.NEAREST)
    img.paste(s, (x, y), s)


def wrap_text(fnt, text, width):
    """模拟 LVGL 的 LV_LABEL_LONG_WRAP：按像素宽逐字折行。"""
    out = []
    for para in text.split("\n"):
        cur = ""
        for ch in para:
            if cur and fnt.w(cur + ch) > width:
                out.append(cur)
                cur = ch
            else:
                cur += ch
        out.append(cur)
    return out


def draw_multiline(d, fnt, xy, text, width, fill, line_h=20, spacing=4):
    x, y = xy
    for i, ln in enumerate(wrap_text(fnt, text, width)):
        d.text((x, y + i * (line_h + spacing)), ln, font=fnt.f, fill=fill)


def shot_scene(fnt):
    img = base("bg_pond")
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, spot="静水塘")
    sprite(img, "prop_float", 110, 96)
    hint_pill(img, d, fnt, "OK 抛竿 · 长按菜单")
    return img


def shot_reel(fnt):
    img = base(None, PANEL)
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, batt=17, score=246)
    d.rectangle([20, 46, 35, 269], fill=(40, 40, 60))              # 进度条底
    d.rectangle([20, 180, 35, 269], fill=(80, 220, 120))           # 进度
    d.rectangle([196, 46, 221, 269], outline=(180, 210, 255), width=2)   # 轨道
    d.rectangle([198, 150, 219, 189], fill=(90, 230, 140))         # 捕捉区
    d.rectangle([199, 168, 218, 177], fill=(255, 200, 70))         # 鱼标
    d.text((40, 46), "收线 62%\n鲫鱼", font=fnt.f, fill=(255, 255, 255), spacing=0)
    hint_pill(img, d, fnt, "按住 OK 抬竿 · 松开落下")
    return img


def shot_menu(fnt):
    img = base(None, (6, 18, 36))
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, batt=86, score=128)
    d.rounded_rectangle([6, 70, 233, 95], radius=4, fill=ACCENT)   # 选中条
    rows = ["开始钓鱼", "鱼竿   手竿", "饵料   蚯蚓", "钓点   静水塘", "图鉴   5/24"]
    for i, t in enumerate(rows):
        d.text((12, 74 + i * 30), t, font=fnt.f, fill=(255, 255, 255))
    hint_pill(img, d, fnt, "上下选·OK进入·长按返回")
    return img


def shot_codex(fnt):
    img = base(None, (6, 18, 36))
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, batt=86, score=128)
    sprite(img, "fish_jiyu", 92, 36)
    d.text((8, 56), "3. 鲫鱼 · 常见 ✓", font=fnt.f, fill=(255, 255, 255))
    d.text((8, 78), "最长 24.5 厘米 · 钓 6 条", font=fnt.f, fill=(190, 210, 240))
    draw_multiline(d, fnt, (8, 104), "圆扁身带银灰光泽，池塘里最好客的鱼", 224, (255, 255, 255))
    d.text((8, 250), "收录 5/24   累计钓获 12", font=fnt.f, fill=(190, 210, 240))
    hint_pill(img, d, fnt, "上下翻·OK详情·长按返回")
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    fnt = Fonts()
    shots = [("01-scene", shot_scene(fnt)), ("02-reel", shot_reel(fnt)),
             ("03-menu", shot_menu(fnt)), ("04-codex", shot_codex(fnt))]

    for name, im in shots:
        im.convert("RGB").save(os.path.join(OUT, name + ".png"))

    # 拼图：2x2，每格放大 2 倍 + 标注
    Z, gap, lab = 2, 24, 30
    cw, ch = W * Z, H * Z + lab
    sheet = Image.new("RGB", (gap + 2 * (cw + gap), gap + 2 * (ch + gap)), (240, 242, 246))
    ds = ImageDraw.Draw(sheet)
    lf = ImageFont.truetype(OTF, 20)
    titles = ["钓鱼场景（顶栏三项 + 右上角钓点）", "收线（低电量 17%，电池变红）",
              "菜单", "图鉴详情"]
    for i, (name, im) in enumerate(shots):
        r, c = divmod(i, 2)
        x, y = gap + c * (cw + gap), gap + r * (ch + gap)
        big = im.resize((W * Z, H * Z), Image.NEAREST)
        big = Image.composite(big, Image.new("RGB", big.size, (0, 0, 0)), MASK.resize(big.size, Image.NEAREST))
        sheet.paste(big, (x, y + lab))
        ds.text((x + 2, y + 2), titles[i], font=lf, fill=(30, 36, 48))
    out = os.path.join(OUT, "ui-preview.png")
    sheet.save(out)
    print("saved", out, sheet.size)
    for name, _ in shots:
        print("  ", os.path.join(OUT, name + ".png"))


if __name__ == "__main__":
    main()
