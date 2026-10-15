#!/usr/bin/env python3
"""生成魔法帝之路菜单条目图标（44x44 透明 PNG，4x 超采样抗锯齿）。
输出到 mage-game/assets/sp_icon_<name>.png，再由 gen_mage_sprites.py 烘焙进 C 数组。
图标清单：book(学魔法) / orb(魔力修行) / crown(晋升考核) /
         map(野外章节) / gate(遗迹秘境&随机副本) / herb(采药委托)
"""
import os
from PIL import Image, ImageDraw

S = 44          # 最终尺寸
SS = S * 4      # 超采样
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "mage-game", "assets")

GOLD = (255, 217, 61, 255)
WHITE = (242, 238, 251, 255)
CYAN = (94, 200, 242, 255)
GREEN = (127, 220, 138, 255)
GRAY = (154, 147, 181, 255)


def new():
    return Image.new("RGBA", (SS, SS), (0, 0, 0, 0))


def downscale(im):
    return im.resize((S, S), Image.LANCZOS)


def save(name, im):
    p = os.path.join(OUT, f"sp_icon_{name}.png")
    downscale(im).save(p)
    print("wrote", p)


def book():
    im = new(); d = ImageDraw.Draw(im)
    cx = SS // 2
    # 两页
    d.polygon([(cx - SS*0.34, SS*0.30), (cx - 4, SS*0.36),
               (cx - 4, SS*0.70), (cx - SS*0.34, SS*0.64)], fill=WHITE)
    d.polygon([(cx + SS*0.34, SS*0.30), (cx + 4, SS*0.36),
               (cx + 4, SS*0.70), (cx + SS*0.34, SS*0.64)], fill=WHITE)
    # 书脊
    d.rectangle([cx - 5, SS*0.34, cx + 5, SS*0.71], fill=GOLD)
    # 文字线
    for yy in (0.42, 0.50, 0.58):
        d.line([(cx - SS*0.28, SS*yy), (cx - 10, SS*yy)], fill=GOLD, width=3)
        d.line([(cx + 10, SS*yy), (cx + SS*0.28, SS*yy)], fill=GOLD, width=3)
    # 外描边
    d.line([(cx - SS*0.34, SS*0.30), (cx - 4, SS*0.36),
            (cx - 4, SS*0.70), (cx - SS*0.34, SS*0.64),
            (cx - SS*0.34, SS*0.30)], fill=GOLD, width=4)
    d.line([(cx + SS*0.34, SS*0.30), (cx + 4, SS*0.36),
            (cx + 4, SS*0.70), (cx + SS*0.34, SS*0.64),
            (cx + SS*0.34, SS*0.30)], fill=GOLD, width=4)
    save("book", im)


def orb():
    im = new(); d = ImageDraw.Draw(im)
    c = (SS // 2, SS // 2); r = SS * 0.32
    d.ellipse([c[0]-r, c[1]-r, c[0]+r, c[1]+r], fill=(120, 200, 255, 255))
    d.ellipse([c[0]-r*0.66, c[1]-r*0.66, c[0]+r*0.66, c[1]+r*0.66],
              fill=(200, 235, 255, 255))
    # 外金环
    d.ellipse([c[0]-r-6, c[1]-r-6, c[0]+r+6, c[1]+r+6], outline=GOLD, width=6)
    # 高光
    d.ellipse([c[0]-r*0.5, c[1]-r*0.6, c[0]-r*0.1, c[1]-r*0.2],
              fill=(255, 255, 255, 200))
    # 底部小星
    d.ellipse([c[0]+r*0.5, c[1]+r*0.55, c[0]+r*0.7, c[1]+r*0.75], fill=GOLD)
    save("orb", im)


def crown():
    im = new(); d = ImageDraw.Draw(im)
    y0, y1 = SS*0.34, SS*0.66
    x0, x1 = SS*0.22, SS*0.78
    # 底坐
    d.rectangle([x0, y1 - SS*0.10, x1, y1], fill=GOLD)
    # 三尖
    pts = [(x0, y1 - SS*0.10), (x0, y0 + SS*0.06), (x0 + (x1-x0)*0.25, y0 + SS*0.12),
           (x0 + (x1-x0)*0.5, y0), (x0 + (x1-x0)*0.75, y0 + SS*0.12),
           (x1, y0 + SS*0.06), (x1, y1 - SS*0.10)]
    d.polygon(pts, fill=(255, 235, 120, 255))
    # 尖顶宝石
    d.ellipse([x0 + (x1-x0)*0.5 - 6, y0 - 12, x0 + (x1-x0)*0.5 + 6, y0 + 6], fill=CYAN)
    d.ellipse([x0 - 6, y0, x0 + 8, y0 + 14], fill=CYAN)
    d.ellipse([x1 - 8, y0, x1 + 6, y0 + 14], fill=CYAN)
    save("crown", im)


def map():
    im = new(); d = ImageDraw.Draw(im)
    m = SS*0.16
    d.rounded_rectangle([m, m, SS-m, SS-m], radius=SS*0.06, fill=(230, 220, 190, 255))
    # 折角
    d.polygon([(SS-m-SS*0.18, m), (SS-m, m), (SS-m, m+SS*0.18)], fill=(200, 188, 158, 255))
    # 路线
    d.line([(m+SS*0.10, SS*0.55), (SS*0.45, SS*0.40), (SS*0.62, SS*0.62),
            (SS-m-SS*0.10, SS*0.50)], fill=GOLD, width=4)
    # 定位针
    px, py = SS*0.45, SS*0.40
    d.ellipse([px-8, py-20, px+8, py+4], fill=GOLD)
    d.polygon([(px-8, py-4), (px+8, py-4), (px, py+10)], fill=GOLD)
    d.ellipse([px-3, py-14, px+3, py-8], fill=(60, 40, 20, 255))
    save("map", im)


def gate():
    im = new(); d = ImageDraw.Draw(im)
    m = SS*0.20
    w = SS*0.16
    top = SS*0.30
    # 拱顶（粗灰弧）
    d.arc([m, top - SS*0.22, SS-m, top + SS*0.22], start=180, end=360,
          fill=GRAY, width=int(SS*0.10))
    # 两柱
    d.rectangle([m, top, m+w, SS-m], fill=GRAY)
    d.rectangle([SS-m-w, top, SS-m, SS-m], fill=GRAY)
    # 金边
    d.rectangle([m, top, m+w, SS-m], outline=GOLD, width=3)
    d.rectangle([SS-m-w, top, SS-m, SS-m], outline=GOLD, width=3)
    d.arc([m, top - SS*0.22, SS-m, top + SS*0.22], start=180, end=360,
          fill=GOLD, width=4)
    save("gate", im)


def herb():
    im = new(); d = ImageDraw.Draw(im)
    cx, cy = SS*0.46, SS*0.52
    # 叶片
    r = SS*0.30
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=GREEN)
    d.ellipse([cx-r, cy-r, cx+r, cy+r], outline=(60, 160, 90, 255), width=4)
    # 叶脉
    d.line([(cx, cy-r), (cx, cy+r)], fill=(60, 160, 90, 255), width=4)
    d.line([(cx, cy), (cx-r*0.7, cy-r*0.5)], fill=(60, 160, 90, 255), width=3)
    d.line([(cx, cy), (cx+r*0.7, cy-r*0.5)], fill=(60, 160, 90, 255), width=3)
    # 茎
    d.line([(cx, cy+r), (cx+SS*0.06, SS*0.86)], fill=GREEN, width=5)
    save("herb", im)


for fn in (book, orb, crown, map, gate, herb):
    fn()
print("done")
