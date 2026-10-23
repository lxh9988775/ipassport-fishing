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


RED = (255, 107, 107, 255)
BLUE = (94, 200, 242, 255)
PURPLE = (185, 140, 255, 255)
DARK = (40, 30, 70, 255)
LAV = (200, 180, 235, 255)


def heart():
    im = new(); d = ImageDraw.Draw(im)
    cx, cy = SS // 2, int(SS * 0.56); r = SS * 0.22
    d.ellipse([cx - r, cy - r * 1.4, cx, cy - r * 0.2], fill=RED)
    d.ellipse([cx, cy - r * 1.4, cx + r, cy - r * 0.2], fill=RED)
    d.polygon([(cx - r, cy - r * 0.4), (cx + r, cy - r * 0.4), (cx, cy + r * 1.2)], fill=RED)
    d.line([(cx - r, cy - r * 0.4), (cx, cy + r * 1.2), (cx + r, cy - r * 0.4)], fill=GOLD, width=4)
    save("heart", im)


def shield():
    im = new(); d = ImageDraw.Draw(im)
    x0, x1 = SS * 0.30, SS * 0.70; y0, y1 = SS * 0.24, SS * 0.74
    pts = [(x0, y0), (x1, y0), (x1, y1 - SS * 0.06), (SS // 2, y1), (x0, y1 - SS * 0.06)]
    d.polygon(pts, fill=BLUE)
    d.line(pts + [pts[0]], fill=GOLD, width=4)
    save("shield", im)


def coin():
    im = new(); d = ImageDraw.Draw(im); c = (SS // 2, SS // 2); r = SS * 0.32
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=GOLD)
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=(180, 140, 20, 255), width=5)
    d.ellipse([c[0] - r * 0.6, c[1] - r * 0.6, c[0] + r * 0.6, c[1] + r * 0.6], outline=(255, 240, 160, 255), width=3)
    d.rectangle([c[0] - r * 0.12, c[1] - r * 0.34, c[0] + r * 0.12, c[1] + r * 0.34], fill=(150, 110, 10, 255))
    save("coin", im)


def paw():
    im = new(); d = ImageDraw.Draw(im); c = (SS // 2, SS // 2)
    d.ellipse([c[0] - SS * 0.18, c[1] - SS * 0.02, c[0] + SS * 0.18, c[1] + SS * 0.22], fill=LAV)
    for dx, dy in [(-0.24, -0.18), (0.24, -0.18), (-0.11, -0.27), (0.11, -0.27)]:
        cx2, cy2, r = c[0] + dx * SS, c[1] + dy * SS, SS * 0.085
        d.ellipse([cx2 - r, cy2 - r, cx2 + r, cy2 + r], fill=LAV)
    save("paw", im)


def bolt():
    im = new(); d = ImageDraw.Draw(im)
    pts = [(SS * 0.56, SS * 0.18), (SS * 0.33, SS * 0.56), (SS * 0.48, SS * 0.56),
           (SS * 0.40, SS * 0.82), (SS * 0.67, SS * 0.42), (SS * 0.52, SS * 0.42)]
    d.polygon(pts, fill=(255, 225, 77, 255))
    d.line(pts + [pts[0]], fill=GOLD, width=3)
    save("bolt", im)


def star():
    im = new(); d = ImageDraw.Draw(im); c = (SS // 2, SS // 2); R = SS * 0.33; r = R * 0.45
    import math
    pts = []
    for i in range(10):
        ang = math.pi / 2 + i * math.pi / 5
        rad = R if i % 2 == 0 else r
        pts.append((c[0] + rad * math.cos(ang), c[1] - rad * math.sin(ang)))
    d.polygon(pts, fill=GOLD)
    d.line(pts + [pts[0]], fill=(200, 160, 20, 255), width=3)
    save("star", im)


def bed():
    im = new(); d = ImageDraw.Draw(im)
    m = SS * 0.18
    d.rounded_rectangle([m, SS * 0.46, SS - m, SS * 0.76], radius=SS * 0.04, fill=LAV)
    d.rectangle([m, SS * 0.46, m + SS * 0.18, SS * 0.62], fill=WHITE)
    d.rectangle([m, SS * 0.40, SS - m, SS * 0.50], fill=(110, 90, 170, 255))
    d.line([(m, SS * 0.40), (SS - m, SS * 0.40)], fill=GOLD, width=3)
    save("bed", im)


def flask():
    im = new(); d = ImageDraw.Draw(im)
    cx = SS // 2; top = SS * 0.22; neck = SS * 0.10
    d.polygon([(cx - neck, top), (cx + neck, top), (cx + neck, SS * 0.44),
               (SS * 0.74, SS * 0.78), (SS * 0.26, SS * 0.78), (cx - neck, SS * 0.44)], fill=(120, 200, 255, 110))
    d.polygon([(cx - neck, SS * 0.58), (SS * 0.70, SS * 0.78), (SS * 0.30, SS * 0.78)], fill=(90, 200, 255, 220))
    d.line([(cx - neck, top), (cx + neck, top), (cx + neck, SS * 0.44),
            (SS * 0.74, SS * 0.78), (SS * 0.26, SS * 0.78), (cx - neck, SS * 0.44), (cx - neck, top)], fill=GOLD, width=3)
    save("flask", im)


def hammer():
    im = new(); d = ImageDraw.Draw(im)
    d.rectangle([SS * 0.28, SS * 0.28, SS * 0.72, SS * 0.46], fill=GRAY)
    d.rectangle([SS * 0.44, SS * 0.46, SS * 0.56, SS * 0.80], fill=(150, 110, 60, 255))
    d.line([(SS * 0.28, SS * 0.28), (SS * 0.72, SS * 0.28), (SS * 0.72, SS * 0.46),
            (SS * 0.28, SS * 0.46), (SS * 0.28, SS * 0.28)], fill=GOLD, width=3)
    save("hammer", im)


def robe():
    im = new(); d = ImageDraw.Draw(im)
    cx = SS // 2
    d.polygon([(cx - 26, 70), (cx, 34), (cx + 26, 70)], fill=(120, 90, 175, 255))     # 兜帽
    d.polygon([(cx - 22, 68), (cx + 22, 68), (cx + 36, 152), (cx - 36, 152)],
              fill=(150, 110, 210, 255))                                              # 身
    d.polygon([(cx - 10, 66), (cx + 10, 66), (cx + 15, 152), (cx - 15, 152)],
              fill=(196, 160, 235, 255))                                              # 前襟
    d.line([(cx - 22, 68), (cx - 36, 152), (cx + 36, 152), (cx + 22, 68)], fill=GOLD, width=4)  # 侧金边
    d.line([(cx - 26, 70), (cx, 34), (cx + 26, 70)], fill=GOLD, width=4)              # 帽金边
    d.line([(cx - 12, 66), (cx + 12, 66)], fill=GOLD, width=5)                        # 领口
    d.ellipse([cx - 8, 92, cx + 8, 108], fill=CYAN)                                   # 胸前宝石
    d.ellipse([cx - 3, 95, cx + 3, 101], fill=(255, 255, 255, 180))
    save("robe", im)


def sword():
    im = new(); d = ImageDraw.Draw(im)
    cx = SS // 2
    d.polygon([(cx - 7, 100), (cx + 7, 100), (cx + 4, 44), (cx - 4, 44)], fill=(205, 210, 224, 255))  # 刃
    d.polygon([(cx - 4, 44), (cx + 4, 44), (cx, 24)], fill=(205, 210, 224, 255))                        # 尖
    d.polygon([(cx - 7, 100), (cx, 100), (cx, 44), (cx - 4, 44)], fill=(245, 248, 255, 255))            # 高光
    d.line([(cx, 44), (cx, 100)], fill=(150, 158, 178, 255), width=2)                                    # 血槽
    d.rectangle([cx - 30, 100, cx + 30, 116], fill=GOLD)                                                  # 护手
    d.rectangle([cx - 30, 100, cx + 30, 116], outline=(180, 140, 20, 255), width=3)
    d.rectangle([cx - 7, 116, cx + 7, 150], fill=(150, 110, 60, 255))                                     # 握把
    for y in (124, 134, 144):
        d.line([(cx - 7, y), (cx + 7, y)], fill=(110, 80, 40, 255), width=3)                             # 缠绳
    d.ellipse([cx - 11, 150, cx + 11, 172], fill=GOLD)                                                    # 圆头
    d.ellipse([cx - 11, 150, cx + 11, 172], outline=(180, 140, 20, 255), width=3)
    save("sword", im)


def staff():
    im = new(); d = ImageDraw.Draw(im)
    cx = SS // 2
    d.rectangle([cx - 5, 40, cx + 5, 162], fill=(150, 110, 60, 255))                                      # 杖身
    d.line([(cx - 5, 40), (cx - 5, 162)], fill=(110, 80, 40, 255), width=2)
    d.ellipse([cx - 18, 22, cx + 18, 58], fill=(120, 200, 245, 255))                                       # 顶珠
    d.ellipse([cx - 18, 22, cx + 18, 58], outline=GOLD, width=4)
    d.ellipse([cx - 9, 28, cx + 4, 41], fill=(255, 255, 255, 180))                                        # 高光
    d.rectangle([cx - 9, 58, cx + 9, 66], fill=GOLD)                                                      # 金环
    d.ellipse([cx + 20, 30, cx + 26, 36], fill=GOLD)                                                      # 火花
    d.ellipse([cx - 26, 40, cx - 20, 46], fill=GOLD)
    save("staff", im)


def potion():
    im = new(); d = ImageDraw.Draw(im)
    cx = SS // 2
    d.rectangle([cx - 11, 36, cx + 11, 78], fill=(170, 200, 220, 120))                                     # 瓶颈
    d.rectangle([cx - 11, 28, cx + 11, 42], fill=(150, 110, 60, 255))                                      # 塞
    d.line([(cx - 11, 42), (cx + 11, 42)], fill=GOLD, width=3)
    d.ellipse([cx - 30, 70, cx + 30, 150], fill=(170, 200, 220, 130))                                      # 瓶身玻璃
    d.ellipse([cx - 30, 70, cx + 30, 150], outline=GOLD, width=4)
    d.chord([cx - 28, 96, cx + 28, 148], start=0, end=180, fill=(90, 200, 210, 235))                       # 液体
    d.rectangle([cx - 28, 118, cx + 28, 148], fill=(90, 200, 210, 235))
    d.line([(cx - 22, 108), (cx + 22, 108)], fill=(200, 255, 255, 200), width=3)                           # 液面
    d.arc([cx - 26, 76, cx + 2, 144], start=200, end=320, fill=(255, 255, 255, 160), width=5)              # 玻璃高光
    save("potion", im)


def crystal():
    im = new(); d = ImageDraw.Draw(im)
    pts = [(SS // 2, SS * 0.18), (SS * 0.70, SS * 0.46), (SS // 2, SS * 0.82), (SS * 0.30, SS * 0.46)]
    d.polygon(pts, fill=(120, 220, 210, 255))
    d.line([(SS // 2, SS * 0.18), (SS * 0.30, SS * 0.46), (SS // 2, SS * 0.82),
            (SS * 0.70, SS * 0.46), (SS // 2, SS * 0.18)], fill=GOLD, width=3)
    d.line([(SS * 0.30, SS * 0.46), (SS * 0.70, SS * 0.46)], fill=GOLD, width=2)
    save("crystal", im)


def gear():
    im = new(); d = ImageDraw.Draw(im); c = (SS // 2, SS // 2); r = SS * 0.24
    import math
    for i in range(8):
        a = i * 2 * math.pi / 8
        x0 = c[0] + r * 0.85 * math.cos(a); x1 = c[0] + r * 1.3 * math.cos(a)
        y0 = c[1] + r * 0.85 * math.sin(a); y1 = c[1] + r * 1.3 * math.sin(a)
        d.rectangle([min(x0, x1) - SS * 0.04, min(y0, y1) - SS * 0.04,
                     max(x0, x1) + SS * 0.04, max(y0, y1) + SS * 0.04], fill=GRAY)
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=GRAY)
    d.ellipse([c[0] - r * 0.4, c[1] - r * 0.4, c[0] + r * 0.4, c[1] + r * 0.4], fill=DARK)
    save("gear", im)


def bag():
    im = new(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([SS * 0.28, SS * 0.42, SS * 0.72, SS * 0.80], radius=SS * 0.06, fill=(180, 150, 100, 255))
    d.line([(SS * 0.40, SS * 0.42), (SS * 0.40, SS * 0.52), (SS * 0.60, SS * 0.52), (SS * 0.60, SS * 0.42)],
           fill=(150, 120, 80, 255), width=4)
    d.line([(SS * 0.28, SS * 0.58), (SS * 0.72, SS * 0.58)], fill=GOLD, width=3)
    save("bag", im)


def scroll():
    im = new(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([SS * 0.22, SS * 0.30, SS * 0.78, SS * 0.70], radius=SS * 0.04, fill=(235, 225, 195, 255))
    for yy in (0.42, 0.52, 0.62):
        d.line([(SS * 0.30, SS * yy), (SS * 0.70, SS * yy)], fill=(150, 130, 90, 255), width=3)
    d.ellipse([SS * 0.16, SS * 0.26, SS * 0.30, SS * 0.40], fill=(200, 180, 140, 255))
    d.ellipse([SS * 0.70, SS * 0.60, SS * 0.84, SS * 0.74], fill=(200, 180, 140, 255))
    save("scroll", im)


def bone():
    im = new(); d = ImageDraw.Draw(im)
    cx, cy = SS // 2, SS // 2; r = SS * 0.12
    d.line([(cx - r * 1.5, cy - r * 0.2), (cx + r * 1.5, cy + r * 0.2)], fill=(230, 230, 210, 255), width=int(SS * 0.09))
    for dx, dy in [(-1.5, -0.2), (-1.5, 0.2), (1.5, -0.2), (1.5, 0.2)]:
        x, y = cx + dx * r, cy + dy * r
        d.ellipse([x - r * 0.5, y - r * 0.5, x + r * 0.5, y + r * 0.5], fill=(230, 230, 210, 255))
    save("bone", im)


def seal():
    im = new(); d = ImageDraw.Draw(im); c = (SS // 2, SS // 2); r = SS * 0.26
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=(200, 90, 120, 255))
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=GOLD, width=4)
    d.ellipse([c[0] - r * 0.5, c[1] - r * 0.5, c[0] + r * 0.5, c[1] + r * 0.5], outline=(255, 200, 220, 255), width=2)
    save("seal", im)


for fn in (book, orb, crown, map, gate, herb, heart, shield, coin, paw, bolt,
           star, bed, flask, hammer, robe, crystal, gear, bag, scroll, bone, seal,
           sword, staff, potion):
    fn()
print("done")
