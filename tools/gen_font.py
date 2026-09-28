#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py - 用 lv_font_conv 生成「竿影浮标」专用的中文子集字体。

为什么不用 LVGL 内置的 CJK 字体：内置 lv_font_source_han_sans_sc_16_cjk 只覆盖
1187 个字形，实测缺「钓 鱼 饵 蚯 蚓 咬 抛 竿 塘 钩」等本游戏核心用字，屏上会变方框。
所以只把**屏上真正会出现的字符**做进字库：约 130 个字形，代价 ≈ 15 KB Flash。

前置：
  - assets/fonts/SourceHanSansCN-Normal.otf   （思源黑体 CN，SIL OFL 1.1）
    下载见 assets/fonts/README.md
  - node + lv_font_conv（装在托管 node workspace）

用法：
    python tools/gen_font.py
产物：
    assets/fonts/fishing_cjk_16.c   （符号名 fishing_cjk_16）
    tools/font_symbols.txt          （本次使用的字符清单，便于复现与校验）

字符集是**自动**从 main/fishing.c 里抓的（所有字符串字面量中的非 ASCII 字符），
外加可打印 ASCII 全量。所以改完屏上文案只要重跑本脚本即可，不需要手工维护清单。
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OTF = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
OUT_C = os.path.join(ROOT, "assets", "fonts", "fishing_cjk_16.c")
SYMBOLS_TXT = os.path.join(ROOT, "tools", "font_symbols.txt")
SRC_C = os.path.join(ROOT, "main", "fishing.c")
NODE = r"C:/Users/8605464/.workbuddy/binaries/node/versions/22.22.2-3/node.exe"
CONV = r"C:/Users/8605464/.workbuddy/binaries/node/workspace/node_modules/lv_font_conv/lv_font_conv.js"

FONT_NAME = "fishing_cjk_16"
SIZE = 16
BPP = 4

# 源码里没有、但屏上可能出现的字符（保险起见一并打进字库）
EXTRA = "％"


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def build_symbols():
    if not os.path.exists(SRC_C):
        raise SystemExit("找不到 %s" % SRC_C)
    with open(SRC_C, "r", encoding="utf-8") as f:
        src = strip_comments(f.read())
    s = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
        for ch in lit:
            if ord(ch) > 0x7F:          # 非 ASCII = 需要中文字库
                s.add(ch)
    s |= set(EXTRA)
    # 可打印 ASCII 全量（"OK"、数字、%、括号等）
    s |= {chr(c) for c in range(0x20, 0x7F)}
    return "".join(sorted(s))



def main():
    if not os.path.exists(NODE) or not os.path.exists(CONV):
        print("[FAIL] 找不到 node 或 lv_font_conv：\n  %s\n  %s" % (NODE, CONV))
        print("       安装： 在 node workspace 里 npm install lv_font_conv")
        return 2
    if not os.path.exists(OTF):
        print("[FAIL] 缺少源字体：%s" % OTF)
        print("       下载见 assets/fonts/README.md")
        return 2

    symbols = build_symbols()
    with open(SYMBOLS_TXT, "w", encoding="utf-8", newline="\n") as f:
        f.write(symbols)
    print("字符清单：%d 个 -> %s" % (len(symbols), SYMBOLS_TXT))

    cmd = [
        NODE, CONV,
        "--font", OTF,
        "--symbols", symbols,
        "--size", str(SIZE),
        "--bpp", str(BPP),
        "--format", "lvgl",
        "--no-compress",
        "--lv-font-name", FONT_NAME,
        "--lv-include", "lvgl.h",
        "--output", OUT_C,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.stdout:
        print(r.stdout.strip())
    if r.returncode != 0:
        print("[FAIL] lv_font_conv 退出码 %d\n%s" % (r.returncode, r.stderr))
        return 1

    sz = os.path.getsize(OUT_C)
    with open(OUT_C, "r", encoding="utf-8", errors="replace") as f:
        head = f.read(4000)
    if FONT_NAME not in head:
        print("[FAIL] 产物里找不到符号 %s" % FONT_NAME)
        return 1
    print("[PASS] 生成 %s  (%.1f KB)" % (OUT_C, sz / 1024.0))
    print("       符号名：lv_font_%s" % FONT_NAME)
    return 0


if __name__ == "__main__":
    sys.exit(main())
