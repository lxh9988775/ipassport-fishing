#!/usr/bin/env python3
"""把 mage-game/assets/*.png 转成 ESP32-C3 可编译的 RGB565 + alpha C 数组。

策略（8MB Flash / 240x320 屏的现实约束）：
  - 只转固件真正用到的贴图。
  - 按「屏幕上实际显示尺寸的 2 倍」超采样降分辨率，再转 RGB565(2B) + alpha(1B)。
  - PNG 原图 39MB -> 转换后约 0.5~0.9MB，能塞进 factory 分区。

输出：assets/mage_sprites.c + assets/mage_sprites.h
"""
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "mage-game", "assets")
OUT_C = os.path.join(ROOT, "assets", "mage_sprites.c")
OUT_H = os.path.join(ROOT, "assets", "mage_sprites.h")

# key -> (源文件, 显示宽度, 显示高度)  显示尺寸按 HTML 里 drawImage/drawSprite 的实际取值。
# 8MB Flash 要同时装代码 + 1161 字中文子集字库 + 贴图，所以全屏大图按 1.0~1.25 倍、
# 小立绘按 ~2 倍超采样即可（屏本身只有 240x320，再高纯浪费 Flash）。
SPRITES = {
    # 全屏背景：尺寸必须与屏幕 1:1（240x320），否则 LVGL 会走缩放变换、
    # 需要额外的全屏 draw buffer，而 LV_MEM 只有几十 KB，必然分配失败 → Load access fault。
    "title": ("title_art.png", 240, 320),
    "town":  ("town_art.png", 240, 320),
    "battle_bg": ("battle_bg_k.png", 240, 132),
    # 战斗立绘：同样按屏上实际像素尺寸 1:1 生成，全程零缩放
    "sp_hero":   ("sp_hero_k.png", 84, 108),
    "sp_goblin": ("sp_goblin_k.png", 60, 80),
    "sp_slime":  ("sp_slime_k.png", 84, 54),
    "sp_cat":    ("sp_cat_k.png", 84, 76),
    # 图标/头像（HTML: 32~76px），一律取偶数宽高
    "hero_icon": ("hero.png", 80, 80),
    "cat_icon":  ("catspirit.png", 56, 56),
    "goblin_icon": ("goblin.png", 56, 56),
}

# 背景色：与 main/mage.c 的 C_BG (0x0d0b1a) 严格一致。
# 素材按 HTML 的 canvas 底色 #15102a 抠图，两者同为深紫黑，预乘后视觉一致。
BG_R, BG_G, BG_B = 0x0d, 0x0b, 0x1a

# 低于这个 alpha 视为全透明，写背景色即可（省字节 + 省绘制时间）
ALPHA_CUT = 12


def to_rgb565(img: Image.Image) -> Image.Image:
    return img.convert("RGB")


def emit(name: str, src_name: str, tw: int, th: int) -> dict:
    path = os.path.join(SRC, src_name)
    if not os.path.exists(path):
        print(f"  !! 缺少素材 {src_name}，跳过 {name}")
        return None

    im = Image.open(path)
    has_alpha = im.mode in ("RGBA", "LA") or "transparency" in im.info
    if has_alpha:
        im = im.convert("RGBA")
        alpha = im.getchannel("A")
        rgb = im.convert("RGB")
    else:
        rgb = im.convert("RGB")
        alpha = Image.new("L", rgb.size, 255)

    # 按显示尺寸降采样（LANCZOS 保持边缘干净）
    rgb = rgb.resize((tw, th), Image.LANCZOS)
    alpha = alpha.resize((tw, th), Image.LANCZOS)

    px = list(rgb.getdata())
    al = list(alpha.getdata())

    return {
        "name": name,
        "w": tw,
        "h": th,
        "rgb": px,
        "alpha": al,
    }


def c_array_of(vals, per_line=16):
    out = []
    for i in range(0, len(vals), per_line):
        chunk = vals[i:i + per_line]
        out.append("    " + ",".join(str(v) for v in chunk) + ",")
    return "\n".join(out)


def main():
    parts = []
    parts.append("/* 本文件由 tools/gen_mage_sprites.py 自动生成，请勿手改。")
    parts.append(" * 源素材：mage-game/assets 目录下的 PNG（与 HTML 原版 1:1 同一批图，不新增不重绘）")
    parts.append(" * 格式：LV_COLOR_FORMAT_RGB565 小端色。")
    parts.append(" * 透明处理：alpha 已按背景色 0x0d0b1a 预乘烘焙进 RGB —— 本工程 LVGL 为 16 色深，")
    parts.append(" *   逐像素 alpha 不可用，预乘是视觉无损的稳妥做法。")
    parts.append(" */")
    parts.append('#include "mage_sprites.h"')
    parts.append("")

    index = []
    total = 0
    for name, (src_name, tw, th) in SPRITES.items():
        print(f"处理 {name} <- {src_name} ({tw}x{th})")
        s = emit(name, src_name, tw, th)
        if s is None:
            continue

        # 预乘：out = src*a + bg*(1-a)，再用 RGB565 量化
        out = []
        for (r, g, b), a in zip(s["rgb"], s["alpha"]):
            if a < ALPHA_CUT:
                r2, g2, b2 = BG_R, BG_G, BG_B
            else:
                r2 = (r * a + BG_R * (255 - a)) // 255
                g2 = (g * a + BG_G * (255 - a)) // 255
                b2 = (b * a + BG_B * (255 - a)) // 255
            out.append(((r2 & 0xF8) << 8) | ((g2 & 0xFC) << 3) | (b2 >> 3))

        sym = s["name"]
        parts.append(f"/* ---- {sym}  {tw}x{th} ---- */")
        parts.append(f"static const uint16_t {sym}_rgb[{tw * th}] __attribute__((aligned(4))) = {{")
        parts.append(c_array_of(out))
        parts.append("};")
        parts.append("")

        index.append((sym, tw, th))
        total += tw * th * 2
        print(f"  -> {tw}x{th}, 约 {tw * th * 2 / 1024:.0f}KB")

    parts.append("const mage_sprite_t mage_sprites[] = {")
    for sym, w, h in index:
        parts.append(f"    {{ \"{sym}\", {w}, {h}, {sym}_rgb }},")
    parts.append("};")
    parts.append("")
    parts.append(f"const int mage_sprite_count = {len(index)};")
    parts.append("")

    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("""/* 本文件由 tools/gen_mage_sprites.py 自动生成，请勿手改。 */
#ifndef MAGE_SPRITES_H
#define MAGE_SPRITES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint16_t    w;
    uint16_t    h;
    const uint16_t *rgb;
} mage_sprite_t;

extern const mage_sprite_t mage_sprites[];
extern const int mage_sprite_count;

#ifdef __cplusplus
}
#endif
#endif
""")

    print(f"\n共 {len(index)} 张贴图，原始像素数据约 {total / 1024:.0f}KB")
    print(f"输出：{OUT_C}")
    print(f"输出：{OUT_H}")


if __name__ == "__main__":
    sys.exit(main())
