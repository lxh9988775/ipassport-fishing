#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
《余烬》电脑端对比图渲染器（M-08 闸门交付物）
复刻 C 端 main/ember_color.c 的 relightPixel，使用真实锁定母版：
  - 背景：generated-images/ember_bg_clean.png (240x320 RGB) -> 缩放/裁水印
  - 精灵：generated-images/ember_spirit_skeleton.png (240x320 RGBA, R=l0/G=s0/A=shape)
按火色(h,s)逐像素重染精灵为 RGB565A8 视觉，叠加发光层 + 文字，渲染 8 屏(S0~S7) x 5 火色对比图。
注意：对比图用的重染逻辑与固件 gen_ember_assets.py 同源，确保“屏上精灵不偏离已审定母版”。
"""
import os, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BG = os.path.join(ROOT, "generated-images", "ember_bg_clean.png")
SK = os.path.join(ROOT, "generated-images", "ember_spirit_skeleton.png")
FONT = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
OUT = os.path.join(ROOT, "build", "ember_compare")
os.makedirs(OUT, exist_ok=True)

W, H = 240, 320
RAMP_X = [0, .15, .32, .50, .70, .88, 1.0]
RAMP_L = [.05, .14, .31, .50, .68, .80, .86]
RAMP_S = [1, 1, .96, .90, .80, .62, .48]

# 5 种火色（一夜进程，来自 ember_facts.json q3to2.steps，末位为“热爱3工作2”答案）
FIRE = [
    ("本色·第一夜", "#DBA86B"),
    ("第2夜",       "#DCA266"),
    ("第3夜",       "#DD9B60"),
    ("第4夜",       "#DD945A"),
    ("热爱3·工作2", "#DF7C4F"),
]

# ---------------- 颜色工具 ----------------
def hex2hsl(hexv):
    hexv = hexv.lstrip("#")
    r, g, b = int(hexv[0:2],16)/255, int(hexv[2:4],16)/255, int(hexv[4:6],16)/255
    mx, mn = max(r,g,b), min(r,g,b)
    d = mx-mn
    l = (mx+mn)/2
    if d == 0:
        h = s = 0
    else:
        s = d/(2-d) if l>0.5 else d/(mx+mn)
        if mx==r: h = ((g-b)/d) % 6
        elif mx==g: h = (b-r)/d + 2
        else: h = (r-g)/d + 4
        h *= 60
    return h, s, l

def hsl2rgb_vec(h_deg, s, l):
    h = (np.asarray(h_deg, float)/360.0) % 1.0
    q = np.where(l < 0.5, l*(1+s), l+s-l*s)
    p = 2*l - q
    def chan(t):
        t = t - np.floor(t)
        v = np.zeros_like(t)
        v = np.where(t < 1/6, p + (q-p)*6*t, v)
        v = np.where((t >= 1/6) & (t < 1/2), q, v)
        v = np.where((t >= 1/2) & (t < 2/3), p + (q-p)*(2/3-t)*6, v)
        v = np.where(t >= 2/3, p, v)
        return v
    r = chan(h + 1/3); g = chan(h); b = chan(h - 1/3)
    return ((r*255+0.5).astype(np.uint8), (g*255+0.5).astype(np.uint8), (b*255+0.5).astype(np.uint8))

def relight(l0, s0, h, sat, tip):
    L = np.interp(l0, RAMP_X, RAMP_L)*0.86 + l0*0.14
    L = np.clip(L, 0, 1)
    S = np.interp(l0, RAMP_X, RAMP_S)*sat*(0.82 + 0.28*s0)
    S = np.clip(S, 0, 0.95)
    r, g, b = hsl2rgb_vec(np.full_like(l0, h), S, L)
    mask = l0 < 0.38
    w = np.where(mask, 0.15*0.5*(1 - l0/0.38), 0.0)
    r = np.clip(r*(1-w) + tip[0]*w, 0, 255).astype(np.uint8)
    g = np.clip(g*(1-w) + tip[1]*w, 0, 255).astype(np.uint8)
    b = np.clip(b*(1-w) + tip[2]*w, 0, 255).astype(np.uint8)
    return r, g, b

# ---------------- 资源 ----------------
def load_bg():
    im = Image.open(BG).convert("RGB").resize((W, H), Image.LANCZOS)
    a = np.asarray(im, np.float32)
    # 裁掉右下角“AI生成”水印：用附近森林色覆盖 50x20
    cover = a[290:305, 150:185].mean(axis=(0,1))
    a[300:320, 190:240] = cover
    return a

def load_skeleton():
    im = Image.open(SK).convert("RGBA")
    arr = np.asarray(im)
    R = arr[:,:,0].astype(np.float32)/255.0
    G = arr[:,:,1].astype(np.float32)/255.0
    A = arr[:,:,3]
    return R, G, A

def composite(fire_hex, brightness, glow_r, glow_c=(120,140), ember_dim=False):
    h, s, l = hex2hsl(fire_hex)
    tip = hsl2rgb_vec(44, 0.80, 0.64)
    tip = (int(tip[0]), int(tip[1]), int(tip[2]))
    l0, s0, A = SK_R, SK_G, SK_A
    r, g, b = relight(l0, s0, h, s, tip)
    # 基底（背景 * 亮度）
    base = BG_ARR.copy() * brightness
    # 发光层（径向）
    yy, xx = np.mgrid[0:H, 0:W]
    dist = np.sqrt((xx-glow_c[0])**2 + (yy-glow_c[1])**2)
    gm = np.clip(1 - dist/glow_r, 0, None)
    fr, fg, fb = hsl2rgb_vec(np.full((H,W), h), np.full((H,W), s), np.full((H,W), l))
    glow_rgb = np.stack([fr, fg, fb], -1).astype(np.float32)
    out = base + glow_rgb * (gm[...,None]*0.55)
    out = np.clip(out, 0, 255)
    # 精灵叠加（透明）
    a01 = A[...,None]/255.0
    sp = np.stack([r, g, b], -1).astype(np.float32)
    out = out*(1-a01) + sp*a01
    out = np.clip(out, 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGB")

# ---------------- 文字 ----------------
def font(sz):
    return ImageFont.truetype(FONT, sz)

def draw_screens():
    # 每屏配置：(id, name, brightness, glow_r, poems, hud, lamps, hint, ember_card, fireflies, sleeping)
    cfg = [
        dict(id="S0", name="首夜", br=0.72, gr=75,  poems=["今晚，火先亮着。"], hint="〇 开始", keyonly=True),
        dict(id="S1", name="今夜", br=1.00, gr=75,  poems=[], hud=True, lamps=True, hint="◁ 观　〇 添　▷ 相"),
        dict(id="S2", name="添柴", br=0.86, gr=85,  poems=[], hud=True, lamps=True, sel_lamp=2, fireflies=True, hint="◁ 选灯　〇 添柴"),
        dict(id="S3", name="火相", br=1.00, gr=75,  poems=["今天分得匀，火很稳。", "你不在的时候，它自己缓过来了。"], hint="任意键返回"),
        dict(id="S4", name="夜话·余烬", br=0.50, gr=48, poems=["你今天那点热，", "烧给了谁，又剩下了谁。"], hint="柴尽了·明早见", ember_card=True),
        dict(id="S5", name="复燃", br=1.12, gr=93,  poems=["你不在的时候，", "它自己缓过来了。"], hint="第2夜"),
        dict(id="S6", name="浮层", br=1.00, gr=60,  poems=["你又在硬撑了。"], overlay=True),
        dict(id="S7", name="借柴", br=0.76, gr=60,  poems=["要借明天的柴吗？", "火会亮，也会发灰。"], zero=True, hint="〇 借　▷ 算了"),
    ]
    return cfg

def render_screen(c, fire_hex):
    if c.get("ember_card"):
        return render_ember_card(fire_hex, c)   # 全图收藏卡版式，自带全部文字
    br, gr = c["br"], c["gr"]
    if c.get("sleeping"):
        pass
    img = composite(fire_hex, br, gr)
    d = ImageDraw.Draw(img)
    # HUD 大字 “5”
    if c.get("hud") and not c.get("zero"):
        d.text((13, 6), "5", font=font(34), fill=(245,225,190))
        d.text((16, 46), "柴", font=font(9), fill=(220,200,170))
        d.text((228, 8), "第1夜", font=font(9), fill=(210,190,160), anchor="ra")
    if c.get("zero"):
        d.text((13, 8), "0", font=font(30), fill=(150,140,120))
        d.text((16, 44), "柴", font=font(9), fill=(150,140,120))
    # 四灯
    if c.get("lamps"):
        lamps = [(53,264),(98,274),(144,268),(190,258)]
        for i,(lx,ly) in enumerate(lamps):
            col = (255,225,180) if not c.get("sel_lamp") or i!=c["sel_lamp"] else (255,200,120)
            if c.get("sel_lamp") is not None and i==c["sel_lamp"]:
                d.ellipse([lx-9,ly-9,lx+9,ly+9], outline=(255,200,120), width=1)
            d.ellipse([lx-4,ly-4,lx+4,ly+4], fill=col)
        if c.get("fireflies"):
            for fx,fy in [(43,244),(64,250),(70,238)]:
                d.ellipse([fx-2,fy-2,fx+2,fy+2], fill=(255,230,150))
    # 诗句
    poems = c.get("poems", [])
    y = 206
    for line in poems:
        d.text((22, y), line, font=font(12), fill=(235,220,200))
        y += 16
    # 键提示
    if c.get("hint"):
        d.text((120, 304), c["hint"], font=font(10), fill=(180,170,150), anchor="mm")
    return img

def hex2rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def render_ember_card(fire_hex, c):
    """S4 夜话·余烬：整屏一张「全图收藏卡」。

    取的是宝可梦 Full-Art 卡的设计语言——插画满版出血、卡名与编号只落在角上、
    底部仅一行 flavor text、圆角 + 暖金描边 + 火光晕做实体卡质感。
    画面本身全部来自《余烬》锁定的夜森林 + 火焰精灵母版，不借用任何外部素材。
    """
    # 桌面：压暗夜森林 + 暗角，让卡片像被火光照着摆在暗处
    desk = composite(fire_hex, 0.46, 58, glow_c=(120, 150))
    yy, xx = np.mgrid[0:H, 0:W]
    vig = np.clip(1 - np.sqrt(((xx-120)/155)**2 + ((yy-160)/195)**2)*0.55, 0.26, 1)
    arr = np.asarray(desk, np.float32) * vig[..., None]
    screen = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").convert("RGBA")

    # 卡片几何：212x284，接近实体卡竖版比例，占屏 ~88%
    cx0, cy0, cx1, cy1 = 14, 16, 226, 300
    cw, ch = cx1-cx0, cy1-cy0
    rad, edge = 12, 3
    iw, ih = cw-2*edge, ch-2*edge

    # 火色光晕垫在卡下（火光映在桌面上）
    fr, fg, fb = hex2rgb(fire_hex)
    halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(halo).rounded_rectangle(
        [cx0-7, cy0-7, cx1+7, cy1+7], radius=rad+7, fill=(fr, fg, fb, 95))
    screen.alpha_composite(halo.filter(ImageFilter.GaussianBlur(17)))

    # 卡面插画：满版、等比、略放大裁切，精灵是绝对主角
    art = composite(fire_hex, 0.96, 78).crop((18, 22, 222, 294)).resize((iw, ih), Image.LANCZOS)
    a = np.asarray(art, np.float32).copy()
    yy2 = np.arange(ih, dtype=np.float32)[:, None]
    top_a = np.clip(1 - yy2/44, 0, 1)*0.70
    bot_a = np.clip(1 - (ih-1-yy2)/50, 0, 1)*0.82
    amt = np.clip(np.maximum(top_a, bot_a), 0, 0.82)[..., None]
    a = a*(1-amt) + np.array([10, 14, 12], np.float32)*amt
    art = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), "RGB")

    mask = Image.new("L", (iw, ih), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, iw-1, ih-1], radius=rad-edge, fill=255)
    screen.paste(art.convert("RGBA"), (cx0+edge, cy0+edge), mask)

    # 卡边：暖金描边 + 内侧覆膜高光
    d = ImageDraw.Draw(screen)
    d.rounded_rectangle([cx0-2, cy0-2, cx1+2, cy1+2], radius=rad+2,
                        outline=(182, 140, 94, 255), width=2)
    d.rounded_rectangle([cx0-1, cy0-1, cx1+1, cy1+1], radius=rad+1,
                        outline=(242, 216, 176, 110), width=1)

    # 卡内文字：只有 4 处小字，插画占 ~85% 面积
    tx, ty = cx0+edge, cy0+edge
    d.text((tx+14, ty+11), "余 烬", font=font(17), fill=(248, 232, 205, 255))
    d.text((tx+14, ty+35), "第 1 夜", font=font(9), fill=(205, 182, 152, 255))
    d.ellipse([tx+iw-76, ty+16, tx+iw-68, ty+24], fill=(fr, fg, fb, 255))
    d.text((tx+iw-14, ty+14), fire_hex, font=font(9), fill=(238, 216, 184, 255), anchor="ra")
    flavor = "".join(c.get("poems", []))
    d.text((tx+iw//2, ty+ih-20), flavor, font=font(9), fill=(230, 210, 184, 255), anchor="mm")
    if c.get("hint"):
        d.text((120, 311), c["hint"], font=font(8), fill=(158, 144, 126, 255), anchor="mm")
    return screen.convert("RGB")

# ---------------- 主流程 ----------------
BG_ARR = load_bg()
SK_R, SK_G, SK_A = load_skeleton()
CFG = draw_screens()

# 1) 8x5 网格对比图
cell_w, cell_h = W, H
header_h, label_w = 36, 60
cols, rows = len(FIRE), len(CFG)
canvas_w = label_w + cols*cell_w
canvas_h = header_h + rows*cell_h
grid = Image.new("RGB", (canvas_w, canvas_h), (24,22,28))
gd = ImageDraw.Draw(grid)
# 列头（火色名）
for ci,(name,hexv) in enumerate(FIRE):
    x = label_w + ci*cell_w
    gd.rectangle([x,0,x+cell_w,header_h], fill=(40,36,44))
    gd.text((x+cell_w//2, header_h//2), f"{name}\n{hexv}", font=font(11), fill=(235,225,205), anchor="mm")
# 行标签 + 单元格
for ri,c in enumerate(CFG):
    y0 = header_h + ri*cell_h
    gd.rectangle([0,y0,label_w,y0+cell_h], fill=(34,30,38))
    gd.text((label_w//2, y0+cell_h//2), f"{c['id']}\n{c['name']}", font=font(13), fill=(220,210,190), anchor="mm")
    for ci,(name,hexv) in enumerate(FIRE):
        x0 = label_w + ci*cell_w
        cell = render_screen(c, hexv)
        grid.paste(cell, (x0, y0))
grid.save(os.path.join(OUT, "grid.png"))
print("saved grid.png", grid.size)

# 2) 8 张 hero（用“热爱3工作2”色，细节审查）
for c in CFG:
    img = render_screen(c, "#DF7C4F")
    img.save(os.path.join(OUT, f"{c['id']}_hero.png"))
print("saved 8 heroes")

# 3) 森林取样点恒定证据（背景与火色无关，3 点 5 色同值）
samples = [("林冠",(20,28)), ("树屋",(120,95)), ("栅栏",(212,292))]
legend = Image.new("RGB", (canvas_w, 70), (24,22,28))
ld = ImageDraw.Draw(legend)
ld.text((4, 8), "森林取样点(与火色无关,应恒值):", font=font(11), fill=(220,210,190))
for ci,(name,hexv) in enumerate(FIRE):
    x0 = label_w + ci*cell_w
    ld.rectangle([x0+4, 34, x0+cell_w-4, 62], fill=tuple(int(v) for v in BG_ARR[samples[1][1][1], samples[1][1][0]]))
    ld.text((x0+cell_w//2, 70-2), name, font=font(9), fill=(200,190,170), anchor="mm")
# 三取样点色值文字
txt = "  ".join(f"{n}={tuple(int(v) for v in BG_ARR[py,px])}" for n,(px,py) in samples)
ld.text((label_w, 8), txt, font=font(10), fill=(200,200,210))
legend.save(os.path.join(OUT, "forest_samples.png"))
print("saved forest_samples.png")

# 4) S4 全图卡 5 色并排（便于确认卡片在不同火色下的样子）
s4 = [c for c in CFG if c["id"] == "S4"][0]
strip = Image.new("RGB", (len(FIRE)*W, H+24), (24, 22, 28))
sd = ImageDraw.Draw(strip)
for ci, (name, hexv) in enumerate(FIRE):
    sd.text((ci*W + W//2, 12), f"{name}  {hexv}", font=font(11), fill=(235, 225, 205), anchor="mm")
    strip.paste(render_screen(s4, hexv), (ci*W, 24))
strip.save(os.path.join(OUT, "S4_cards_5colors.png"))
print("saved S4_cards_5colors.png")

# 5) 卡面精灵 vs 锁定母版 对照（审核用：核脸/焰尖是否同一色系）
master = Image.open(os.path.join(ROOT, "generated-images", "ember_spirit_master.png")).convert("RGB")
master = master.resize((W, H), Image.LANCZOS)
card = render_screen(s4, "#DF7C4F")
vs = Image.new("RGB", (W*2 + 12, H+24), (24, 22, 28))
vd = ImageDraw.Draw(vs)
vd.text((W//2, 12), "锁定母版（审定时）", font=font(11), fill=(235, 225, 205), anchor="mm")
vd.text((W+12+W//2, 12), "当前卡面渲染", font=font(11), fill=(235, 225, 205), anchor="mm")
vs.paste(master, (0, 24))
vs.paste(card, (W+12, 24))
vs.save(os.path.join(OUT, "S4_vs_master.png"))
print("saved S4_vs_master.png")
print("DONE")
