#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""《余烬》固件美术资源生成器（对齐手册 X-01 / X-02 / X-07）

产物：assets/ember/ember_assets.c / .h —— 全部为 Flash 常量图，零 LV_MEM 占用。

格式（关键，修正上一版真 bug）：
  * 背景 LV_COLOR_FORMAT_RGB565，uint16_t 小端数组（240x320，静态永不 recolor）。
  * 精灵 LV_COLOR_FORMAT_ARGB8888，uint32_t 小端数组。
    小端存 0xAARRGGBB -> 内存字节 [BB,GG,RR,AA] = [B,G,R,A]，
    正是 LVGL v9 lv_color32_t 的顺序。上一版逐字节写 [R,G,B,A] 红蓝互换，已修正。
  * 余烬卡整屏 RGB565 预烘：宝可梦 Full-Art 全图卡，插画满版、文字极少。

UI 层两次真机返工：
  1) 2026-11-11 第一次：真机只有 lv_img 可靠绘制，lv_label / lv_obj 经
     HIDDEN 恢复后全部不显示。故把所有 HUD / 四灯 / 键提示 / 覆盖屏文字
     预烘成 ARGB8888 透明贴图，运行时只切 src。
  2) 2026-11-11 第二次（板子发烫）：8 层 ARGB8888 里有两个整屏 240x320
     半透明层，且所有对象被强制固定 240x320 —— 即使隐藏成 4x4 透明图，
     对象仍占满全屏，LVGL 每帧要逐像素 alpha 混合几十万像素，
     ESP32-C3 单核 / 无 PSRAM 持续满载。现改为：
       - wood / hint / ovl / none 全部 RGB565 不透明（LVGL 直接 memcpy，零混合）
       - 覆盖屏由「整屏淡遮罩」改为 240x80 不透明文字条，底部提示复用 hint 图
       - 隐藏图 1x1，对象随之缩小，不再常驻整屏区域
       - 四灯 / 选中圈保留 ARGB8888（只有小圆点有 alpha），尺寸裁到 160x40
       - 固件不再调用 lv_obj_set_size 固定尺寸
     运行时唯一需要 alpha 混合的只剩精灵层（190x320）。

白边修复（真机反馈「右边还有白线条」）：
  ember_bg_clean.png 右侧 x=236..239 四列是纯白 (255,255,255)，320 行全白，
  是 AI 出图留白。这里按镜像邻接列回填 8 列，杜绝白线随图上板。

为什么不按 X-01 用运行时 RGB565A8 重染：
  240x320 RGB565A8 双平面需 W*H*3=230KB 可写 RAM；ESP32-C3 无 PSRAM，
  LV_MEM 仅 96KB，放不下。故改为「构建期按同源 relightPixel 逐像素点亮，
  多档火色预烘成 Flash 常量图，运行时只 lv_img_set_src 切档」。

火色档位一律取自 tools/ember_facts.json，禁止手写 hex。
"""
import os, json
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BG = os.path.join(ROOT, "generated-images", "ember_bg_clean.png")
SK = os.path.join(ROOT, "generated-images", "ember_spirit_skeleton.png")
FONT = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
FACTS = os.path.join(ROOT, "tools", "ember_facts.json")
OUT_C = os.path.join(ROOT, "assets", "ember", "ember_assets.c")
OUT_H = os.path.join(ROOT, "assets", "ember", "ember_assets.h")

W, H = 240, 320
SP_W, SP_H = 190, 320
RAMP_X = [0, .15, .32, .50, .70, .88, 1.0]
RAMP_L = [.05, .14, .31, .50, .68, .80, .86]
RAMP_S = [1, 1, .96, .90, .80, .62, .48]

# UI 贴图尺寸（2026-11-11 二次返工：不透明 RGB565 小图 + 1x1 隐藏图）
WOOD_W, WOOD_H = 96, 54
HINT_W, HINT_H = 240, 30
OVL_W, OVL_H = 240, 80       # 覆盖屏文字条（贴到 y=100），不再整屏半透明
LAMP_W, LAMP_H = 160, 40    # 四灯 + 选中圈，裁到真实占用区域
LAMP_OX, LAMP_OY = 40, 247  # 上板坐标
NONE_W, NONE_H = 1, 1       # 隐藏图 1x1：对象缩到最小，不再常驻整屏
LAMP_XY = [(53, 264), (98, 274), (144, 268), (190, 258)]

C_CREAM = (250, 238, 214, 255)
C_GOLD = (255, 214, 150, 255)
C_DIM = (206, 194, 172, 255)
DEEP = (10, 14, 16)         # 不透明深色底板

HINTS = [
    "〇 开始",                                    # 0 S0 首夜
    "◁ 观火　〇 添柴　▷ 看火相",                  # 1 S1 今夜
    "◁ 上一盏　〇 添柴　▷ 下一盏",                # 2 S2 添柴
    "任意键返回",                                 # 3 S3 火相
    "柴尽了·明早见",                              # 4 S4 余烬卡
    "〇 回到今夜",                                # 5 S5 复燃
    "你又在硬撑了。",                             # 6 S6 浮层
    "〇 借　▷ 算了",                              # 7 S7 借柴确认
    "柴尽了　〇 借明天的柴　▷ 算了",               # 8 S2 柴尽
]

OVERLAYS = [
    ("今晚，火先亮着。", "小余在等你。", "〇 开始"),                    # S0
    ("今天分得匀，火很稳。", "四盏灯，各有各的热。", "任意键返回"),      # S3
    ("你不在的时候，", "它自己缓过来了。", "〇 回到今夜"),              # S5
    ("要借明天的柴吗？", "火会亮，也会发灰。", "〇 借　▷ 算了"),        # S7
]

CARD_WORDS_BY_FRAME = [
    ("今天你把热留给了手头的事，", "歇下来时，它替你亮着。"),
    ("你又为人操心了。", "被记挂的人，夜里不冷。"),
    ("那点热爱你还留着，", "没被日子磨掉，挺好的。"),
    ("你终于没忘了身体。", "它悄悄多烧了一截。"),
    ("四盏都点到一点，不偏不倚，", "这样睡得最安稳。"),
    ("借来的火会发灰，", "明早记得还自己一点热。"),
]


# ---------------- 颜色（与 C 端 ember_color.c / JS 端同源） ----------------
def hex2hsl(hexv):
    hexv = hexv.lstrip("#")
    r, g, b = int(hexv[0:2], 16)/255, int(hexv[2:4], 16)/255, int(hexv[4:6], 16)/255
    mx, mn = max(r, g, b), min(r, g, b)
    d = mx - mn
    l = (mx + mn) / 2
    if d == 0:
        return 0.0, 0.0, l
    s = d/(2-d) if l > 0.5 else d/(mx+mn)
    if mx == r:
        h = ((g-b)/d) % 6
    elif mx == g:
        h = (b-r)/d + 2
    else:
        h = (r-g)/d + 4
    return h * 60, s, l


def hex2rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def hsl2rgb_vec(h_deg, s, l):
    h = (np.asarray(h_deg, float)/360.0) % 1.0
    q = np.where(l < 0.5, l*(1+s), l+s-l*s)
    p = 2*l - q
    def chan(t):
        t = t - np.floor(t)
        v = np.where(t < 1/6, p + (q-p)*6*t, 0.0)
        v = np.where((t >= 1/6) & (t < 1/2), q, v)
        v = np.where((t >= 1/2) & (t < 2/3), p + (q-p)*(2/3-t)*6, v)
        v = np.where(t >= 2/3, p, v)
        return v
    r = chan(h + 1/3); g = chan(h); b = chan(h - 1/3)
    return ((r*255+0.5).astype(np.uint8),
            (g*255+0.5).astype(np.uint8),
            (b*255+0.5).astype(np.uint8))


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
    """静态夜森林：裁右下角 AI 水印 + 修右侧 4 列纯白边。永不 recolor。"""
    im = Image.open(BG).convert("RGB").resize((W, H), Image.LANCZOS)
    a = np.asarray(im, np.uint8).copy()
    cover = a[290:305, 150:185].mean(axis=(0, 1)).astype(np.uint8)
    a[300:320, 190:240] = cover
    # 真机白线根因：源图右缘 x=233..239 是渐变到纯白的 AI 留白
    # （x=235 均值 231/217/196，x=236..239 全 320 行纯白）。按邻接列镜像回填 8 列，
    # 彻底消除任何 1px 浅线，且不动森林内部取样点。
    edge = 8
    for x in range(W - edge, W):
        src = 2*(W - edge - 1) - x
        a[:, x] = a[:, src]
    return a


def load_skeleton():
    im = Image.open(SK).convert("RGBA").resize((W, H), Image.LANCZOS)
    arr = np.asarray(im)[0:SP_H, 0:SP_W]
    return (arr[:, :, 0].astype(np.float32)/255.0,
            arr[:, :, 1].astype(np.float32)/255.0,
            arr[:, :, 3].astype(np.uint8))


def pack_rgb565(rgb):
    r = rgb[:, :, 0].astype(np.uint16)
    g = rgb[:, :, 1].astype(np.uint16)
    b = rgb[:, :, 2].astype(np.uint16)
    return (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)).reshape(-1)


def pack_argb8888(rgb, alpha):
    r = rgb[:, :, 0].astype(np.uint32)
    g = rgb[:, :, 1].astype(np.uint32)
    b = rgb[:, :, 2].astype(np.uint32)
    a = alpha.astype(np.uint32)
    v = (a << 24) | (r << 16) | (g << 8) | b
    v[a == 0] = 0
    return v.reshape(-1)


def pack_rgba(rgba):
    """RGBA uint8[H,W,4] -> uint32 小端（内存 B,G,R,A）。透明像素写 0。"""
    arr = np.asarray(rgba, np.uint8)
    return pack_argb8888(arr[:, :, :3], arr[:, :, 3])


def composite(fire_hex, brightness, glow_r, bg, l0, s0, alpha, tip):
    h, s, _ = hex2hsl(fire_hex)
    r, g, b = relight(l0, s0, h, s, tip)
    base = bg.astype(np.float32) * brightness
    yy, xx = np.mgrid[0:SP_H, 0:SP_W]
    dist = np.sqrt((xx-120)**2 + (yy-150)**2)
    gm = np.clip(1 - dist/glow_r, 0, None)
    fr, fg, fb = hsl2rgb_vec(np.full((SP_H, SP_W), h),
                             np.full((SP_H, SP_W), s),
                             np.full((SP_H, SP_W), 0.60))
    glow = np.stack([fr, fg, fb], -1).astype(np.float32)
    out = base.copy()
    out[:, 0:SP_W] += glow * (gm[..., None]*0.55)
    out = np.clip(out, 0, 255)
    a01 = (alpha[..., None]/255.0)
    sp = np.stack([r, g, b], -1).astype(np.float32)
    out[:, 0:SP_W] = np.clip(out[:, 0:SP_W]*(1-a01) + sp*a01, 0, 255)
    return np.clip(out, 0, 255).astype(np.uint8)


def font(sz):
    return ImageFont.truetype(FONT, sz)


def center_text(d, txt, y, fnt, fill, width=W):
    bb = d.textbbox((0, 0), txt, font=fnt)
    tw = bb[2] - bb[0]
    d.text(((width - tw)//2, y), txt, font=fnt, fill=fill)


def render_card(fire_hex, bg, l0, s0, alpha, tip, idx):
    """S4 余烬卡：整屏 Full-Art 全图卡。卡名/色号/夜话/收尾语全部烘进图。"""
    desk = composite(fire_hex, 0.46, 58, bg, l0, s0, alpha, tip)
    yy, xx = np.mgrid[0:H, 0:W]
    vig = np.clip(1 - np.sqrt(((xx-120)/155)**2 + ((yy-160)/195)**2)*0.55, 0.26, 1)
    arr = np.asarray(desk, np.float32) * vig[..., None]
    screen = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").convert("RGBA")

    cx0, cy0, cx1, cy1 = 14, 16, 226, 300
    cw, ch = cx1-cx0, cy1-cy0
    rad, edge = 12, 3
    iw, ih = cw-2*edge, ch-2*edge

    fr, fg, fb = hex2rgb(fire_hex)
    halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(halo).rounded_rectangle(
        [cx0-7, cy0-7, cx1+7, cy1+7], radius=rad+7, fill=(fr, fg, fb, 95))
    screen.alpha_composite(halo.filter(ImageFilter.GaussianBlur(17)))

    art = composite(fire_hex, 0.96, 78, bg, l0, s0, alpha, tip)
    art = Image.fromarray(art, "RGB").crop((18, 22, 222, 294)).resize((iw, ih), Image.LANCZOS)
    a = np.asarray(art, np.float32).copy()
    yy2 = np.arange(ih, dtype=np.float32)[:, None]
    top_a = np.clip(1 - yy2/44, 0, 1)*0.70
    bot_a = np.clip(1 - (ih-1-yy2)/62, 0, 1)*0.88
    amt = np.clip(np.maximum(top_a, bot_a), 0, 0.88)[..., None]
    a = a*(1-amt) + np.array([10, 14, 12], np.float32)*amt
    art = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), "RGB")

    mask = Image.new("L", (iw, ih), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, iw-1, ih-1], radius=rad-edge, fill=255)
    screen.paste(art.convert("RGBA"), (cx0+edge, cy0+edge), mask)

    d = ImageDraw.Draw(screen)
    d.rounded_rectangle([cx0-2, cy0-2, cx1+2, cy1+2], radius=rad+2,
                        outline=(182, 140, 94, 255), width=2)
    d.rounded_rectangle([cx0-1, cy0-1, cx1+1, cy1+1], radius=rad+1,
                        outline=(242, 216, 176, 110), width=1)
    tx, ty = cx0+edge, cy0+edge
    d.text((tx+14, ty+9), "余 烬", font=font(17), fill=C_CREAM)
    d.text((cx1-82, ty+13), fire_hex, font=font(12), fill=C_GOLD)
    w1, w2 = CARD_WORDS_BY_FRAME[idx]
    center_text(d, w1, 246, font(12), C_CREAM)
    center_text(d, w2, 262, font(12), C_CREAM)
    center_text(d, "柴尽了·明早见", 280, font(12), C_DIM)
    return np.asarray(screen.convert("RGB"), np.uint8)


# ---------------- UI 贴图 ----------------
# 二次返工：除精灵外全部「不透明 RGB565」，LVGL 直接 memcpy，不做 alpha 混合。
# 四灯 / 选中圈因只有小圆点有内容，保留小尺寸 ARGB8888（160x40）。
def ui_none():
    """1x1 隐藏图：取背景左上近似色，切到它时对象缩到 1x1，视觉无痕。"""
    return np.array([[[16, 26, 20]]], np.uint8)


def ui_wood(n):
    """左上 HUD：34px 大数字 +『柴』，不透明深底板。"""
    im = Image.new("RGB", (WOOD_W, WOOD_H), DEEP)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, WOOD_W-1, WOOD_H-1], radius=10, fill=DEEP)
    d.text((8, 0), str(n), font=font(38), fill=C_CREAM[:3])
    d.text((56, 26), "柴", font=font(15), fill=C_DIM[:3])
    return np.asarray(im, np.uint8)


def ui_hint(txt):
    """底部键提示条：不透明深底 + 居中亮字。"""
    im = Image.new("RGB", (HINT_W, HINT_H), DEEP)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([4, 1, HINT_W-5, HINT_H-2], radius=8, fill=DEEP)
    center_text(d, txt, 6, font(15), C_CREAM[:3])
    return np.asarray(im, np.uint8)


def ui_lamps(bits):
    """四灯：按 4 bit 亮灭组合，坐标与界面规范 V1 一致。贴到 (40,247)。"""
    im = Image.new("RGBA", (LAMP_W, LAMP_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for i, (x, y) in enumerate(LAMP_XY):
        lx, ly = x - LAMP_OX, y - LAMP_OY
        on = (bits >> i) & 1
        col = (255, 206, 140, 255) if on else (74, 68, 60, 235)
        d.ellipse([lx-5, ly-5, lx+5, ly+5], fill=col,
                  outline=(20, 18, 16, 210), width=1)
    return np.asarray(im, np.uint8)


def ui_sel(i):
    """选中灯的金圈。贴到 (40,247)，叠在四灯之上。"""
    im = Image.new("RGBA", (LAMP_W, LAMP_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x, y = LAMP_XY[i]
    lx, ly = x - LAMP_OX, y - LAMP_OY
    d.ellipse([lx-9, ly-9, lx+9, ly+9], outline=C_GOLD, width=2)
    return np.asarray(im, np.uint8)


def ui_overlay(kind):
    """覆盖屏文字条（240x80，贴到 y=100）。

    不再整屏半透明遮罩：夜森林与火灵在文字条上下完整可见，
    「场景即界面」的设计意图保留，同时消除 76,800 像素/帧的全屏混合。
    底部键提示复用 ember_hint_dsc，由固件按屏切图。
    """
    a, b, _ = OVERLAYS[kind]
    im = Image.new("RGB", (OVL_W, OVL_H), DEEP)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([10, 0, OVL_W-11, OVL_H-1], radius=12, fill=DEEP)
    center_text(d, a, 16, font(19), C_CREAM[:3])
    center_text(d, b, 48, font(15), C_DIM[:3])
    return np.asarray(im, np.uint8)


# ---------------- C 写出 ----------------
def write_arr(f, decl, flat, fmt):
    f.write("static const %s[%d] = {\n" % (decl, len(flat)))
    per = 12 if fmt == "u16" else 8
    for i in range(0, len(flat), per):
        chunk = flat[i:i+per]
        if fmt == "u16":
            f.write("    " + ", ".join("0x%04X" % int(v) for v in chunk) + ",\n")
        else:
            f.write("    " + ", ".join("0" if int(v) == 0 else "0x%08X" % int(v)
                                       for v in chunk) + ",\n")
    f.write("};\n\n")


def dsc(f, name, cf, w, h, arr):
    stride = w*2 if cf == "RGB565" else w*4
    f.write("const lv_img_dsc_t %s = {\n" % name)
    f.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
    f.write("    .header.cf = LV_COLOR_FORMAT_%s,\n" % cf)
    f.write("    .header.w = %d, .header.h = %d, .header.stride = %d,\n" % (w, h, stride))
    f.write("    .data_size = sizeof(%s),\n" % arr)
    f.write("    .data = (const uint8_t *)%s,\n" % arr)
    f.write("};\n\n")


def dsc_arr(f, name, cf, w, h, prefix, n):
    stride = w*2 if cf == "RGB565" else w*4
    f.write("const lv_img_dsc_t %s[%d] = {\n" % (name, n))
    for i in range(n):
        arr = "%s_%d" % (prefix, i)
        f.write("    { .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write("      .header.cf = LV_COLOR_FORMAT_%s,\n" % cf)
        f.write("      .header.w = %d, .header.h = %d, .header.stride = %d,\n"
                % (w, h, stride))
        f.write("      .data_size = sizeof(%s),\n" % arr)
        f.write("      .data = (const uint8_t *)%s },\n" % arr)
    f.write("};\n\n")


def write_group_rgb(f, group, images, w, h):
    """不透明 RGB565 组：LVGL 直接拷贝，零 alpha 混合。"""
    for i, im in enumerate(images):
        write_arr(f, "uint16_t %s_%d" % (group, i),
                  pack_rgb565(np.asarray(im, np.uint8)), "u16")
    dsc_arr(f, "%s_dsc" % group, "RGB565", w, h, group, len(images))


def write_group(f, group, images, w, h):
    """小尺寸 ARGB8888 组（仅四灯 / 选中圈，透明区占绝大多数）。

    group 本身已带 ember_ 前缀（如 ember_lamp），dsc 名必须是
    <group>_dsc = ember_lamp_dsc，与头文件 / 固件 extern 完全一致。
    """
    for i, im in enumerate(images):
        write_arr(f, "uint32_t %s_%d" % (group, i), pack_rgba(im), "u32")
    dsc_arr(f, "%s_dsc" % group, "ARGB8888", w, h, group, len(images))


def main():
    facts = json.load(open(FACTS, encoding="utf-8"))
    steps = facts["q3to2"]["steps"]
    frames = [("F0_mu_cheng", steps[0]),
              ("F1", steps[2]),
              ("F2", steps[3]),
              ("F3", steps[4]),
              ("F4_love3_work2", steps[5]),
              ("F5_borrowed", facts["borrowed"]["engineHex"])]
    N = len(frames)

    bg = load_bg()
    l0, s0, alpha = load_skeleton()
    t = hsl2rgb_vec(44.0, 0.80, 0.64)
    tip = (int(t[0]), int(t[1]), int(t[2]))

    bg_flat = pack_rgb565(bg)
    spirit_flats, card_flats, meta = [], [], []
    for i, (name, hexv) in enumerate(frames):
        h, s, _ = hex2hsl(hexv)
        r, g, b = relight(l0, s0, h, s, tip)
        spirit_flats.append(pack_argb8888(np.stack([r, g, b], -1), alpha))
        card_flats.append(pack_rgb565(render_card(hexv, bg, l0, s0, alpha, tip, i)))
        meta.append((name, hexv, round(h, 2), round(s, 3)))

    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by tools/gen_ember_assets.py -- do not edit.\n")
        f.write(" * bg/card/UI RGB565 (uint16 LE, opaque); spirit ARGB8888 (uint32 LE = B,G,R,A).\n")
        f.write(" * Only spirit + small lamp/sel layers need alpha blending.\n")
        f.write(" * UI layers are pre-rendered bitmaps: no lv_label / HIDDEN on device.\n")
        f.write(" * Fire colors come from tools/ember_facts.json only. */\n")
        f.write('#include "lvgl.h"\n\n')

        write_arr(f, "uint16_t ember_bg_map", bg_flat, "u16")
        for i, flat in enumerate(spirit_flats):
            write_arr(f, "uint32_t ember_spirit_map_%d" % i, flat, "u32")
        for i, flat in enumerate(card_flats):
            write_arr(f, "uint16_t ember_card_map_%d" % i, flat, "u16")

        # UI：不透明 RGB565（零混合）
        write_group_rgb(f, "ember_none", [ui_none()], NONE_W, NONE_H)
        write_group_rgb(f, "ember_wood", [ui_wood(n) for n in range(6)], WOOD_W, WOOD_H)
        write_group_rgb(f, "ember_hint", [ui_hint(t) for t in HINTS], HINT_W, HINT_H)
        write_group_rgb(f, "ember_ovl", [ui_overlay(k) for k in range(4)], OVL_W, OVL_H)
        # 小圆点 / 金圈：小尺寸 ARGB8888
        write_group(f, "ember_lamp", [ui_lamps(b) for b in range(16)], LAMP_W, LAMP_H)
        write_group(f, "ember_sel", [ui_sel(i) for i in range(4)], LAMP_W, LAMP_H)

        dsc(f, "ember_bg_dsc", "RGB565", W, H, "ember_bg_map")
        dsc_arr(f, "ember_spirit_dsc", "ARGB8888", SP_W, SP_H, "ember_spirit_map", N)
        dsc_arr(f, "ember_card_dsc", "RGB565", W, H, "ember_card_map", N)

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by tools/gen_ember_assets.py -- do not edit. */\n")
        f.write("#pragma once\n#include \"lvgl.h\"\n\n")
        f.write("#define EMBER_FIRE_FRAMES %d\n" % N)
        f.write("#define EMBER_UI_WOOD_N 6\n")
        f.write("#define EMBER_UI_HINT_N %d\n" % len(HINTS))
        f.write("#define EMBER_UI_LAMP_N 16\n")
        f.write("#define EMBER_UI_SEL_N 4\n")
        f.write("#define EMBER_UI_OVL_N 4\n")
        f.write("#define EMBER_SCR_W %d\n#define EMBER_SCR_H %d\n" % (W, H))
        f.write("#define EMBER_SPIRIT_W %d\n#define EMBER_SPIRIT_H %d\n" % (SP_W, SP_H))
        f.write("#define EMBER_LAMP_OX %d\n#define EMBER_LAMP_OY %d\n" % (LAMP_OX, LAMP_OY))
        f.write("#define EMBER_OVL_Y 100\n\n")
        f.write("extern const lv_img_dsc_t ember_bg_dsc;\n")
        f.write("extern const lv_img_dsc_t ember_spirit_dsc[EMBER_FIRE_FRAMES];\n")
        f.write("extern const lv_img_dsc_t ember_card_dsc[EMBER_FIRE_FRAMES];\n")
        f.write("extern const lv_img_dsc_t ember_none_dsc[1];\n")
        f.write("extern const lv_img_dsc_t ember_wood_dsc[EMBER_UI_WOOD_N];\n")
        f.write("extern const lv_img_dsc_t ember_hint_dsc[EMBER_UI_HINT_N];\n")
        f.write("extern const lv_img_dsc_t ember_lamp_dsc[EMBER_UI_LAMP_N];\n")
        f.write("extern const lv_img_dsc_t ember_sel_dsc[EMBER_UI_SEL_N];\n")
        f.write("extern const lv_img_dsc_t ember_ovl_dsc[EMBER_UI_OVL_N];\n")

    print("wrote %s (%.2f MB)" % (OUT_C, os.path.getsize(OUT_C)/1024/1024))
    print("frames:")
    for m in meta:
        print("   ", m)
    print("bg right edge after fix:", tuple(int(v) for v in bg[160, 239]),
          tuple(int(v) for v in bg[160, 235]))
    print("UI: none1 wood6 hint%d lamp16 sel4 ovl4 | RGB565 opaque except lamp/sel" % len(HINTS))
    print("render load: spirit is the ONLY full-size ARGB layer; hidden objects are 1x1")


if __name__ == "__main__":
    main()
