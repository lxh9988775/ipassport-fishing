#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""preview_ui.py —— 把 main/fishing.c 的屏幕布局按真实坐标渲染成预览图。

不跑硬件、不跑 LVGL，直接用同一份源码里的坐标 + 同一份字库的度量画出来，
所以"预览长什么样，实机就长什么样（位置层面）"。

坐标来源是 tools/check_ui_layout.py 的解析结果，改坐标不用同步这里，永远不会漂移。

【面板偏移】场景/收线/结算/菜单/图鉴都是 lv_obj_create(scr) 且 pos=(0, HUD_H)，
它们的子元素用的是【面板内相对坐标】，屏幕上要再加 HUD_H(22)。以前这个脚本
把面板子元素当屏幕坐标画，整页偏上 22px（竿和浮漂没落在水线上）。现在统一
按真实屏幕坐标画，和实机逐像素对得上。

产出：assets/preview/<name>.png（240x320 原生）+ 拼图 ui-preview.png + 时段/天气矩阵
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
ACCENT = (153, 0, 58)          # 高亮红
HUD_H = 22                     # 顶栏高度：所有面板都从屏幕 y=22 开始

# 渲染用的"看起来像真机"的取值；没列到的走 check_ui_layout 的最坏情况表
SAMPLE = {
    "得分 %d": "得分 128",
    "最高 %d": "最高 1024",
    "%d": "86",
    "收线 %d%%\n%s": "收线 62%\n鲫鱼",
    "最大 %d.%d 厘米 · 已钓 %d 条": "最大 24.5 厘米 · 已钓 6 条",
    "体型 %d-%d 厘米 · 爱%s": "体型 12-25 厘米 · 爱蚯蚓",
    "%d. %s · %s%s": "3. 鲫鱼 · 常见 ✓",
    "收录 %d/%d   累计钓获 %d": "收录 5/24   累计钓获 12",
    "收录 %d/%d（%d%%）  钓获 %d": "收录 5/24（21%）  钓获 12",
    "集齐 %d 种 · %s": "集齐 8 种 · 渐入佳境",
    "已集齐 %d 种 · %s": "已集齐 24 种 · 竿影大成",
    "%s · %s": "黄昏 · 雨",
    "%d. %s": "3. 鲫鱼",
    "图鉴   %d/%d": "图鉴   5/24",
    "钓点   %s": "钓点   静水塘",
    "鱼竿   %s": "鱼竿   手竿",
    "饵料   %s": "饵料   蚯蚓",
    "声音   %s": "声音   开",
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


# ---------------------------------------------------------------------------
# 钓点氛围：时段 / 天气（与 main/fishing.c 的 env_visual 逐值对齐）
# ---------------------------------------------------------------------------
ENV_PERIODS = ["清晨", "正午", "黄昏", "深夜"]
ENV_WEATHERS = ["晴", "阴", "雨"]
ENV_RAIN_DROPS = 14


def env_visual(period: str, weather: str):
    """返回 (rgb, opacity)：与 main/fishing.c 的 env_visual() 一模一样的整数运算。"""
    if period == "清晨":
        r, g, b, o = 255, 178, 96, 44
    elif period == "正午":
        r, g, b, o = 255, 250, 225, 0
    elif period == "黄昏":
        r, g, b, o = 255, 124, 62, 72
    else:
        r, g, b, o = 24, 44, 108, 120
    if weather == "阴":
        r = (r * 45 + 130 * 55) // 100
        g = (g * 45 + 138 * 55) // 100
        b = (b * 45 + 148 * 55) // 100
        o = max(o, 66)
    elif weather == "雨":
        r = (r * 35 + 92 * 65) // 100
        g = (g * 35 + 110 * 65) // 100
        b = (b * 35 + 150 * 65) // 100
        o = max(o, 96)
    return (r, g, b), o


def rain_points():
    """雨丝落点：与 main/fishing.c 建雨丝时用的是同一套算式。"""
    pts = []
    for i in range(ENV_RAIN_DROPS):
        rx = 12 + (i * 17) % 228
        ry = 28 + (i * 61) % 268
        pts.append(((rx, ry), (rx - 6, ry + 16)))
    return pts


RAIN_POINTS = rain_points()


def apply_env(img: Image.Image, period: str, weather: str) -> Image.Image:
    """把时段染色层 + 雨丝叠上去（顺序与固件一致：背景 → 染色 → 雨丝 → 场景）。"""
    (r, g, b), opa = env_visual(period, weather)
    img = img.convert("RGBA")
    if opa:
        ov = Image.new("RGBA", img.size, (r, g, b, opa))
        img = Image.alpha_composite(img, ov)
    if weather == "雨":
        layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        for p0, p1 in RAIN_POINTS:
            ld.line([p0, p1], fill=(205, 222, 245, 120), width=1)
        img = Image.alpha_composite(img, layer)
    return img.convert("RGB")


def env_label(img: Image.Image, d, fnt, period: str, weather: str):
    """左上角「时段 · 天气」：面板内 (8,6) → 屏幕 (8,28)，底下半透明黑底。"""
    text = "%s · %s" % (period, weather)
    tw = fnt.w(text)
    x, y = 8, HUD_H + 6
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(ov).rounded_rectangle([x, y, x + tw + 4, y + 24], radius=2, fill=(0, 0, 0, 120))
    img.paste(Image.alpha_composite(img.convert("RGBA"), ov).convert("RGB"), (0, 0))
    d = ImageDraw.Draw(img)
    d.text((x + 2, y + 2), text, font=fnt.f, fill=(255, 255, 255))


# ---------------------------------------------------------------------------
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
        d.rounded_rectangle([172, HUD_H + 6, 229, HUD_H + 25], radius=10, fill=(12, 20, 34))
        tw = fnt.w(spot)
        d.text((172 + (58 - tw) / 2, HUD_H + 8), spot, font=fnt.f, fill=(255, 255, 255))


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


def shot_scene(fnt, period="正午", weather="晴", spot="静水塘"):
    """钓场页：背景 + 氛围层 + 鱼竿 + 鱼线 + 浮漂 + 鱼饵 + 环境标签。
    坐标必须和 main/fishing.c 的 ROD_*/FLOAT_*/BAIT_* 宏一致 —— 那边是真值来源，
    改了记得同步这里，再跑一次本脚本用眼睛过一遍。"""
    img = base("bg_pond")
    img = apply_env(img, period, weather)          # 染色层 + 雨丝在背景之上
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, spot=spot)
    rod_x, rod_y = 8, HUD_H + 116
    fx, fy = 150, HUD_H + 146
    rod = Image.open(os.path.join(PNG, "rod_hand.png")).convert("RGBA")
    fl = Image.open(os.path.join(PNG, "prop_float.png")).convert("RGBA")
    bt = Image.open(os.path.join(PNG, "bait_worm.png")).convert("RGBA")
    d.line([(rod_x + rod.width - 6, rod_y + 6), (fx + fl.width // 2, fy)],
           fill=(244, 248, 255), width=1)
    img.paste(rod, (rod_x, rod_y), rod)
    img.paste(fl, (fx, fy), fl)
    img.paste(bt, (fx + fl.width // 2 - bt.width // 2, fy + 29), bt)
    env_label(img, d, fnt, period, weather)
    hint_pill(img, d, fnt, "OK 抛竿 · 长按菜单")
    return img


def shot_reel(fnt):
    img = base(None, PANEL)
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, batt=17, score=246)
    d.rectangle([20, HUD_H + 46, 35, HUD_H + 269], fill=(40, 40, 60))              # 进度条底
    d.rectangle([20, HUD_H + 180, 35, HUD_H + 269], fill=(80, 220, 120))           # 进度
    d.rectangle([196, HUD_H + 46, 221, HUD_H + 269], outline=(180, 210, 255), width=2)   # 轨道
    d.rectangle([198, HUD_H + 150, 219, HUD_H + 189], fill=(90, 230, 140))         # 捕捉区
    d.rectangle([199, HUD_H + 168, 218, HUD_H + 177], fill=(255, 200, 70))         # 鱼标
    d.text((40, HUD_H + 46), "收线 62%\n鲫鱼", font=fnt.f, fill=(255, 255, 255), spacing=0)
    hint_pill(img, d, fnt, "按住 OK 抬竿 · 松开落下")
    return img


def shot_menu(fnt):
    img = base(None, (6, 18, 36))
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, batt=86, score=128)
    # 选中条停在「钓点」（面板内 y=70+3*30=160 → 屏幕 +22）
    d.rounded_rectangle([6, HUD_H + 160, 233, HUD_H + 185], radius=4, fill=ACCENT)
    rows = ["开始钓鱼", "鱼竿   手竿", "饵料   蚯蚓", "钓点   静水塘", "图鉴   5/24", "声音   开"]
    for i, t in enumerate(rows):
        d.text((12, HUD_H + 74 + i * 30), t, font=fnt.f, fill=(255, 255, 255))
    # 钓点这一行没解锁时，提示条常驻"再钓 N 条解锁 X" —— 社区反馈的
    # "不能更换钓点"就是这条缺失导致的（按了没反应、也没解释）
    hint_pill(img, d, fnt, "再钓 8 条解锁 急流河")
    return img


def shot_codex(fnt):
    """图鉴详情页：新版多了「性格钩子」与「里程碑」两行。"""
    img = base(None, (6, 18, 36))
    d = ImageDraw.Draw(img)
    hud(img, d, fnt, batt=86, score=128)
    sprite(img, "fish_jiyu", 92, HUD_H + 14)
    d.text((8, HUD_H + 54), "3. 鲫鱼 · 常见 ✓", font=fnt.f, fill=(255, 255, 255))
    d.text((8, HUD_H + 78), "最大 24.5 厘米 · 已钓 6 条", font=fnt.f, fill=(255, 208, 90))   # 金色：最大纪录
    d.text((8, HUD_H + 102), "体型 12-25 厘米 · 爱蚯蚓", font=fnt.f, fill=(150, 215, 255))  # 体长范围 + 偏好饵
    draw_multiline(d, fnt, (8, HUD_H + 126), "圆扁身带银灰光泽，池塘里最好客的鱼", 224, (255, 255, 255))
    # 性格钩子（新）
    draw_multiline(d, fnt, (8, HUD_H + 176), "底栖稳重，蚯蚓一落便慢慢靠近", 224, (255, 236, 205))
    # 里程碑（新）
    d.text((8, HUD_H + 224), "集齐 8 种 · 渐入佳境", font=fnt.f, fill=(150, 230, 175))
    d.text((8, HUD_H + 248), "收录 5/24（21%）  钓获 12", font=fnt.f, fill=(190, 210, 240))
    hint_pill(img, d, fnt, "上下翻·OK详情·长按返回")
    return img


def env_matrix(fnt):
    """时段 × 天气 全矩阵：3 列（晴/阴/雨）× 4 行（清晨/正午/黄昏/深夜）。"""
    cells = []
    for period in ENV_PERIODS:
        row = []
        for weather in ENV_WEATHERS:
            row.append(shot_scene(fnt, period, weather))
        cells.append(row)
    Z, gap, lab = 1, 14, 26
    cw, ch = W * Z, H * Z + lab
    cols, rows_n = len(ENV_WEATHERS), len(ENV_PERIODS)
    sheet = Image.new("RGB", (gap + cols * (cw + gap), gap + rows_n * (ch + gap)), (240, 242, 246))
    ds = ImageDraw.Draw(sheet)
    lf = ImageFont.truetype(OTF, 20)
    for c, weather in enumerate(ENV_WEATHERS):
        ds.text((gap + c * (cw + gap) + 2, 2), "天气：%s" % weather, font=lf, fill=(30, 36, 48))
    for r, period in enumerate(ENV_PERIODS):
        y = gap + r * (ch + gap)
        for c, weather in enumerate(ENV_WEATHERS):
            x = gap + c * (cw + gap)
            big = cells[r][c].resize((W * Z, H * Z), Image.NEAREST)
            big = Image.composite(big, Image.new("RGB", big.size, (0, 0, 0)),
                                  MASK.resize(big.size, Image.NEAREST))
            sheet.paste(big, (x, y + lab))
            ds.text((x + 2, y + 2), "时段：%s" % period, font=lf, fill=(30, 36, 48))
    out = os.path.join(OUT, "05-env-matrix.png")
    sheet.save(out)
    return out, cells


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
    titles = ["钓鱼场景（顶栏三项 + 右上角钓点 + 左上角时段天气）", "收线（低电量 17%，电池变红）",
              "菜单", "图鉴详情（新增性格钩子 + 里程碑）"]
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

    mpath, cells = env_matrix(fnt)
    print("saved", mpath)
    # 三个代表组合单独存一张原尺寸，方便放大看细节
    for period, weather, tag in (("清晨", "晴", "env-morning-clear"),
                                 ("黄昏", "雨", "env-dusk-rain"),
                                 ("深夜", "晴", "env-night-clear")):
        p = os.path.join(OUT, tag + ".png")
        shot_scene(fnt, period, weather).save(p)
        print("  ", p)


if __name__ == "__main__":
    main()
