# -*- coding: utf-8 -*-
"""按 main/fishing.c 当前坐标渲染主菜单对比图（左=旧红条大圆角，右=概念图5条复刻）。
坐标与固件逐项对齐：面板起点 HUD_H=22、卡片 y=46+i*38、pad_all=0。
仅电脑端预览，不改固件。
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "fish_ui", "fishing_v6_menu_compare.png")
FONT = "C:/Windows/Fonts/simhei.ttf"
if not os.path.exists(FONT):
    FONT = "C:/Windows/Fonts/msyh.ttc"
SW, SH, SCALE = 240, 320, 2

_ft = {}
def f(sz):
    if sz not in _ft:
        _ft[sz] = ImageFont.truetype(FONT, sz)
    return _ft[sz]

INK = (8, 16, 30); PANEL = (16, 32, 54); PANEL2 = (24, 46, 74)
HI = (120, 156, 190); SHADOW = (27, 46, 66); RED = (198, 76, 104)
WHITE = (232, 240, 248); SOFT = (206, 220, 234); MUTED = (140, 164, 190)
OLDRED = (153, 0, 58); OLDBG = (6, 18, 36)

def bevel(d, x, y, w, h, fill):
    d.rectangle([x-1, y-1, x+w, y+h], fill=INK)
    d.rectangle([x, y, x+w-1, y+h-1], fill=fill)
    d.line([(x, y), (x+w-1, y)], fill=HI, width=1)
    d.line([(x, y), (x, y+h-1)], fill=HI, width=1)
    d.line([(x, y+h-1), (x+w-1, y+h-1)], fill=SHADOW, width=1)
    d.line([x+w-1, y, x+w-1, y+h-1], fill=SHADOW, width=1)

def hud(d):
    d.rectangle([0, 0, SW, 24], fill=(0, 0, 0))
    d.line([0, 24, SW, 24], fill=(46, 72, 100), width=1)
    d.text((6, 3), "得分 0", font=f(11), fill=(238, 190, 96))
    d.text((86, 3), "钓手", font=f(11), fill=(108, 190, 174))
    d.rectangle([196, 5, 218, 14], outline=MUTED, width=1)
    d.rectangle([198, 6, 208, 12], fill=(90, 220, 120))

def hint(d, txt):
    d.rectangle([2, 295, 238, 317], fill=INK)
    d.rectangle([2, 295, 238, 317], outline=HI, width=1)
    d.text((52, 300), txt, font=f(10), fill=MUTED)

def draw_old(d):
    d.rectangle([0, 24, SW, 295], fill=OLDBG)
    hud(d)
    d.rectangle([6, 32, 234, 62], fill=OLDRED)
    d.line([6, 32, 234, 32], fill=(236, 72, 153), width=2)
    d.line([6, 32, 6, 62], fill=(236, 72, 153), width=2)
    d.line([6, 62, 234, 62], fill=(236, 72, 153), width=2)
    d.line([234, 32, 234, 62], fill=(236, 72, 153), width=2)
    ys = [35, 77, 119, 161, 203, 245]
    rows = [("开始钓鱼",""),("鱼竿","手竿"),("饵料","蚯蚓"),("钓点","静水塘"),("图鉴","0/27"),("声音","开")]
    for y,(k,v) in zip(ys,rows):
        d.text((14, y), k + ("   " + v if v else ""), font=f(12), fill=WHITE)
    hint(d, "上下选·OK进入·长按返回")

def draw_new(d):
    d.rectangle([0, 24, SW, 295], fill=INK)
    hud(d)
    # 标题 y=22+10=32，16px 字高20；副标题 y=22+30=52，12px 字高15；卡片从 y=68 起
    d.text((14, 32), "钓鱼准备", font=f(16), fill=WHITE)
    d.text((14, 52), "选对竿，才知道去哪下钩", font=f(10), fill=MUTED)
    rows = [("开始钓鱼",""),("鱼竿","手竿"),("饵料","蚯蚓"),("钓点","静水塘"),("图鉴","0/27"),("声音","开")]
    for i,(k,v) in enumerate(rows):
        cy = 22 + 46 + i*38          # 面板内 y=46+i*38 → 屏幕
        bevel(d, 10, cy, 220, 34, PANEL)
        if i == 0:                    # 选中态：整行浅底 + 左侧 3px 红竖条
            d.rectangle([11, cy+1, 228, cy+32], fill=PANEL2)
            d.rectangle([10, cy, 12, cy+33], fill=RED)
        d.text((20, cy+9), k, font=f(11), fill=WHITE if i==0 else SOFT)
        if v:
            d.text((82, cy+10), v, font=f(11), fill=MUTED)
    hint(d, "上下选·OK进入·长按返回")

def main():
    cw, ch = SW*SCALE, SH*SCALE
    head, lab, gap = 66, 46, 26
    W = lab + 2*cw + gap*3
    H = head + 40 + ch + 92
    im = Image.new("RGB", (W, H), (243, 246, 250))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W, head], fill=INK)
    d.text((22, 10), "钓鱼 · 主菜单复刻对比", font=f(22), fill=WHITE)
    d.text((22, 40), "左＝旧版（整条红 + 圆角描边 + 单行文案）  右＝概念图5条复刻（bevel卡片 + 左红竖条 + 标题 + 双列）",
           font=f(12), fill=MUTED)
    x0 = lab + gap
    for j,(t,c) in enumerate([("旧版", (198,76,104)), ("复刻后", (108,190,174))]):
        d.text((x0 + j*(cw+gap), head+12), t, font=f(16), fill=c)
    for j,drawer in enumerate([draw_old, draw_new]):
        xx = x0 + j*(cw+gap)
        yy = head + 40
        d.rectangle([xx-2, yy-2, xx+cw+2, yy+ch+2], outline=(200,210,224), width=2)
        tmp = Image.new("RGB", (SW, SH))
        drawer(ImageDraw.Draw(tmp))
        im.paste(tmp.resize((cw, ch), Image.NEAREST), (xx, yy))
    fy = head + 40 + ch + 14
    d.text((22, fy),
           "五条：①深色硬边底 ②6行bevel卡片(上左亮/下右暗) ③选中=整行浅底+左3px红竖条 ④标题+副标题 ⑤右侧灰色真实状态",
           font=f(13), fill=(120, 80, 10))
    d.text((22, fy + 26), "坐标与固件同源：面板起点HUD_H=22 · 卡片y=46+i*38 · pad_all=0 · 未提交", font=f(12), fill=(110,120,135))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    im.save(OUT)
    print("saved", OUT, im.size)

if __name__ == "__main__":
    main()
