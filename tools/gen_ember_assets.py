#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""《余烬》固件美术资源生成器（对齐手册 X-01 / X-02 / X-07）

产物：assets/ember/ember_assets.c / .h —— 全部为 Flash 常量图，零 LV_MEM 占用。

格式（关键，修正上一版真 bug）：
  * 背景 LV_COLOR_FORMAT_RGB565，uint16_t 小端数组（240x320，静态永不 recolor）。
    手册 X-02 要求；本工程 bg_sprites.c / pet_room.c 已上板验证过该格式。
  * 精灵 LV_COLOR_FORMAT_ARGB8888，uint32_t 小端数组。
    小端存 0xAARRGGBB -> 内存字节 [BB,GG,RR,AA] = [B,G,R,A]，
    正是 LVGL v9 lv_color32_t 的顺序（pet_costumes.c / fish_sprites.c 同款）。
    上一版逐字节写 [R,G,B,A]，红蓝整体互换（暖窗变蓝球、萤火变青点），本版修正。
  * 余烬卡整屏 RGB565 预烘：宝可梦 Full-Art 全图卡，插画满版、文字只留角标。

为什么不按 X-01 用运行时 RGB565A8 重染：
  240x320 RGB565A8 双平面需 W*H*3=230KB 可写 RAM；ESP32-C3 无 PSRAM，
  LV_MEM 仅 96KB，放不下。故改为「构建期按同源 relightPixel 逐像素点亮，
  多档火色预烘成 Flash 常量图，运行时只 lv_img_set_src 切档」。
  核心约束全部保住：静态背景不换色 / 精灵唯一换色对象 / 真实母版骨架 /
  脸属白热芯与焰尖同色 / 森林取样点恒值 / 零周期重绘。

分层（手册 M-03）：
  img_bg     静态夜森林，永不 recolor
  img_spirit 唯一换色对象：骨架 R=l0 / G=s0 / A=shape，同源 relightPixel 点亮
  img_card   S4 余烬卡整屏全图图（按火色档切），动态文字由 LVGL 叠加

火色档位一律取自 tools/ember_facts.json，禁止手写 hex。
"""
import os, json
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BG = os.path.join(ROOT, "generated-images", "ember_bg_clean.png")
SK = os.path.join(ROOT, "generated-images", "ember_spirit_skeleton.png")
MASTER = os.path.join(ROOT, "generated-images", "ember_spirit_master.png")
FONT = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
FACTS = os.path.join(ROOT, "tools", "ember_facts.json")
OUT_C = os.path.join(ROOT, "assets", "ember", "ember_assets.c")
OUT_H = os.path.join(ROOT, "assets", "ember", "ember_assets.h")

W, H = 240, 320
SP_W, SP_H = 190, 320          # 骨架 alpha>8 的 bbox = x[0,188] y[0,319]
RAMP_X = [0, .15, .32, .50, .70, .88, 1.0]
RAMP_L = [.05, .14, .31, .50, .68, .80, .86]
RAMP_S = [1, 1, .96, .90, .80, .62, .48]


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
    if mx == r:   h = ((g-b)/d) % 6
    elif mx == g: h = (b-r)/d + 2
    else:         h = (r-g)/d + 4
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
    """与 tools/ember_color_mirror.py（C/JS 对拍 468 用例 0 偏差）同源。"""
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
    """静态夜森林：裁右下角 AI 水印。永不 recolor。"""
    im = Image.open(BG).convert("RGB").resize((W, H), Image.LANCZOS)
    a = np.asarray(im, np.uint8).copy()
    cover = a[290:305, 150:185].mean(axis=(0, 1)).astype(np.uint8)
    a[300:320, 190:240] = cover
    return a


def load_skeleton():
    """骨架：R=色阶位置 l0, G=笔触密度 s0, A=形状。裁到实体 bbox。"""
    im = Image.open(SK).convert("RGBA").resize((W, H), Image.LANCZOS)
    arr = np.asarray(im)[0:SP_H, 0:SP_W]
    return (arr[:, :, 0].astype(np.float32)/255.0,
            arr[:, :, 1].astype(np.float32)/255.0,
            arr[:, :, 3].astype(np.uint8))


def pack_rgb565(rgb):
    """RGB uint8[H,W,3] -> uint16 小端数组（LV_COLOR_FORMAT_RGB565 native）。"""
    r = rgb[:, :, 0].astype(np.uint16)
    g = rgb[:, :, 1].astype(np.uint16)
    b = rgb[:, :, 2].astype(np.uint16)
    return (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)).reshape(-1)


def pack_argb8888(rgb, alpha):
    """uint32 小端 -> 内存 [B,G,R,A]，LVGL ARGB8888 正确顺序。
    全透明像素写 0，显著压缩 C 文本体积。"""
    r = rgb[:, :, 0].astype(np.uint32)
    g = rgb[:, :, 1].astype(np.uint32)
    b = rgb[:, :, 2].astype(np.uint32)
    a = alpha.astype(np.uint32)
    v = (a << 24) | (r << 16) | (g << 8) | b
    v[a == 0] = 0
    return v.reshape(-1)


def composite(fire_hex, brightness, glow_r, bg, l0, s0, alpha, tip):
    """与电脑端对比图渲染器同源的合成：背景*亮度 + 火色光晕 + 重染精灵。"""
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
    blended = out[:, 0:SP_W]*(1-a01) + sp*a01
    out[:, 0:SP_W] = np.clip(blended, 0, 255)
    return np.clip(out, 0, 255).astype(np.uint8)


def font(sz):
    return ImageFont.truetype(FONT, sz)


def render_card(fire_hex, bg, l0, s0, alpha, tip):
    """S4 余烬卡：整屏一张宝可梦 Full-Art 全图卡。

    插画满版出血、卡名与编号只落角上、圆角+暖金描边+火光晕做实体卡质感。
    只烘固定视觉；第N夜 / 色号 / 夜话 / 明早见 由固件 LVGL 动态叠加。
    """
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
    bot_a = np.clip(1 - (ih-1-yy2)/50, 0, 1)*0.82
    amt = np.clip(np.maximum(top_a, bot_a), 0, 0.82)[..., None]
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
    d.text((tx+14, ty+11), "余 烬", font=font(17), fill=(248, 232, 205, 255))
    return np.asarray(screen.convert("RGB"), np.uint8)


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


def main():
    facts = json.load(open(FACTS, encoding="utf-8"))
    steps = facts["q3to2"]["steps"]
    frames = [("F0_暮橙", steps[0]),
              ("F1", steps[2]),
              ("F2", steps[3]),
              ("F3", steps[4]),
              ("F4_热爱3工作2", steps[5]),
              ("F5_借柴黑蓝", facts["borrowed"]["engineHex"])]
    N = len(frames)

    bg = load_bg()
    l0, s0, alpha = load_skeleton()
    t = hsl2rgb_vec(44.0, 0.80, 0.64)
    tip = (int(t[0]), int(t[1]), int(t[2]))

    bg_flat = pack_rgb565(bg)
    spirit_flats, card_flats, meta = [], [], []
    for name, hexv in frames:
        h, s, _ = hex2hsl(hexv)
        r, g, b = relight(l0, s0, h, s, tip)
        spirit_flats.append(pack_argb8888(np.stack([r, g, b], -1), alpha))
        card_flats.append(pack_rgb565(render_card(hexv, bg, l0, s0, alpha, tip)))
        meta.append((name, hexv, round(h, 2), round(s, 3)))

    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("/* 由 tools/gen_ember_assets.py 自动生成，请勿手工编辑。\n")
        f.write(" * 背景 RGB565(uint16 小端) / 精灵 ARGB8888(uint32 小端 = 内存 B,G,R,A)。\n")
        f.write(" * 色号取自 tools/ember_facts.json；精灵经同源 relightPixel 逐像素点亮。 */\n")
        f.write('#include "lvgl.h"\n\n')
        write_arr(f, "uint16_t ember_bg_map", bg_flat, "u16")
        for i, flat in enumerate(spirit_flats):
            write_arr(f, "uint32_t ember_spirit_map_%d" % i, flat, "u32")
        for i, flat in enumerate(card_flats):
            write_arr(f, "uint16_t ember_card_map_%d" % i, flat, "u16")
        dsc(f, "ember_bg_dsc", "RGB565", W, H, "ember_bg_map")
        f.write("const lv_img_dsc_t ember_spirit_dsc[%d] = {\n" % N)
        for i in range(N):
            f.write("    { .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
            f.write("      .header.cf = LV_COLOR_FORMAT_ARGB8888,\n")
            f.write("      .header.w = %d, .header.h = %d, .header.stride = %d,\n"
                    % (SP_W, SP_H, SP_W*4))
            f.write("      .data_size = sizeof(ember_spirit_map_%d),\n" % i)
            f.write("      .data = (const uint8_t *)ember_spirit_map_%d },\n" % i)
        f.write("};\n\n")
        f.write("const lv_img_dsc_t ember_card_dsc[%d] = {\n" % N)
        for i in range(N):
            f.write("    { .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
            f.write("      .header.cf = LV_COLOR_FORMAT_RGB565,\n")
            f.write("      .header.w = %d, .header.h = %d, .header.stride = %d,\n"
                    % (W, H, W*2))
            f.write("      .data_size = sizeof(ember_card_map_%d),\n" % i)
            f.write("      .data = (const uint8_t *)ember_card_map_%d },\n" % i)
        f.write("};\n")

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("/* 由 tools/gen_ember_assets.py 自动生成，请勿手工编辑。 */\n")
        f.write("#pragma once\n#include \"lvgl.h\"\n\n")
        f.write("#define EMBER_FIRE_FRAMES %d\n" % N)
        f.write("#define EMBER_SCR_W %d\n#define EMBER_SCR_H %d\n" % (W, H))
        f.write("#define EMBER_SPIRIT_W %d\n#define EMBER_SPIRIT_H %d\n\n" % (SP_W, SP_H))
        f.write("extern const lv_img_dsc_t ember_bg_dsc;\n")
        f.write("extern const lv_img_dsc_t ember_spirit_dsc[EMBER_FIRE_FRAMES];\n")
        f.write("extern const lv_img_dsc_t ember_card_dsc[EMBER_FIRE_FRAMES];\n")

    print("wrote %s (%.2f MB)" % (OUT_C, os.path.getsize(OUT_C)/1024/1024))
    print("frames:")
    for m in meta:
        print("   ", m)
    print("bg samples (20,28)(120,95)(212,292):",
          tuple(int(v) for v in bg[28, 20]),
          tuple(int(v) for v in bg[95, 120]),
          tuple(int(v) for v in bg[292, 212]))
    print("spirit bbox:", SP_W, "x", SP_H, "nonzero px:", int((alpha > 8).sum()))


if __name__ == "__main__":
    main()
