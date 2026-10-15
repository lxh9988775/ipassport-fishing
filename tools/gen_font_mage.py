#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font_mage.py - 生成《魔法帝之路》专用中文子集字体 mage_cjk_16.c。

与钓鱼用的 gen_font.py 同源逻辑，但扫描魔法师源码（main/mage*.c）并输出
独立的 mage_cjk_16.c，避免污染钓鱼字库（两条线互不牵连）。

前提：
  - assets/fonts/SourceHanSansCN-Normal.otf
  - node + lv_font_conv（托管 node workspace）
用法：
  python tools/gen_font_mage.py
产物：
  assets/fonts/mage_cjk_16.c  （符号名 mage_cjk_16）
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OTF = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
OUT_C = os.path.join(ROOT, "assets", "fonts", "mage_cjk_16.c")
SYMBOLS_TXT = os.path.join(ROOT, "tools", "font_symbols_mage.txt")
SRC_FILES = [
    os.path.join(ROOT, "main", "mage.c"),
    os.path.join(ROOT, "main", "mage_state.c"),
    os.path.join(ROOT, "main", "mage_data.c"),
    os.path.join(ROOT, "main", "mage_draw.c"),
    os.path.join(ROOT, "main", "mage_hud.c"),
    os.path.join(ROOT, "main", "mage_fx.c"),
    os.path.join(ROOT, "main", "mage_pages.c"),
]
CONV = r"C:/Users/8605464/.workbuddy/binaries/node/workspace/node_modules/lv_font_conv/lv_font_conv.js"


def find_node():
    base = r"C:/Users/8605464/.workbuddy/binaries/node/versions"
    cands = []
    if os.path.isdir(base):
        for d in sorted(os.listdir(base), reverse=True):
            exe = os.path.join(base, d, "node.exe")
            if os.path.exists(exe):
                cands.append(exe)
    return cands[0] if cands else os.path.join(base, "node.exe")


NODE = find_node()

FONT_NAME = "mage_cjk_16"
SIZE = 16
BPP = 4

# 多字号层级（对照 HTML 排版）：16=标题/正文主字号，12=HUD/行内小字（HTML 8-10px 的等价物）
VARIANTS = [
    (16, "mage_cjk_16", OUT_C),
    (12, "mage_cjk_12", os.path.join(ROOT, "assets", "fonts", "mage_cjk_12.c")),
]

# 源码里没有、但屏上可能作为格式串出现的字符（保险起见一并打进字库）
EXTRA = "％·×▶"


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def build_symbols():
    s = set()
    for p in SRC_FILES:
        if not os.path.exists(p):
            # mage_draw.c 等可能尚未生成；缺失则跳过，不阻塞
            continue
        with open(p, "r", encoding="utf-8") as f:
            src = strip_comments(f.read())
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for ch in lit:
                if ord(ch) > 0x7F:
                    s.add(ch)
    s |= set(EXTRA)
    s |= {chr(c) for c in range(0x20, 0x7F)}
    return "".join(sorted(s))


def gen_one(node, symbols, size, font_name, out_c):
    cmd = [
        node, CONV,
        "--font", OTF,
        "--symbols", symbols,
        "--size", str(size),
        "--bpp", str(BPP),
        "--format", "lvgl",
        "--no-compress",
        "--lv-font-name", font_name,
        "--lv-include", "lvgl.h",
        "--output", out_c,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.stdout:
        print(r.stdout.strip())
    if r.returncode != 0:
        print("[FAIL] lv_font_conv(%dpx) 退出码 %d\n%s" % (size, r.returncode, r.stderr))
        return False
    sz = os.path.getsize(out_c)
    with open(out_c, "r", encoding="utf-8", errors="replace") as f:
        head = f.read(4000)
    if font_name not in head:
        print("[FAIL] %dpx 产物里找不到符号 %s" % (size, font_name))
        return False
    print("[PASS] 生成 %s  (%.1f KB)  符号名：lv_font_%s" % (out_c, sz / 1024.0, font_name))
    return True


def main():
    if not os.path.exists(NODE) or not os.path.exists(CONV):
        print("[FAIL] 找不到 node 或 lv_font_conv：\n  %s\n  %s" % (NODE, CONV))
        return 2
    if not os.path.exists(OTF):
        print("[FAIL] 缺少源字体：%s" % OTF)
        return 2

    symbols = build_symbols()
    if not symbols:
        print("[FAIL] 未从源码抽取到任何字符，请确认 mage 源文件存在")
        return 2
    with open(SYMBOLS_TXT, "w", encoding="utf-8", newline="\n") as f:
        f.write(symbols)
    print("字符清单：%d 个 -> %s" % (len(symbols), SYMBOLS_TXT))

    ok = True
    for size, name, out_c in VARIANTS:
        ok = gen_one(NODE, symbols, size, name, out_c) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
