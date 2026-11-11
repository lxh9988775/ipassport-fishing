#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_ember_font.py - 生成《余烬》专用中文子集字库 ember_cjk_16.c。

与钓鱼线 gen_font.py 分开的原因：两条线的屏上文案完全不同，共用字库会互相
拖大 Flash；且《余烬》键提示用全角空格做中文间距（U+3000），临时命令派生时
容易漏，必须有可复跑的脚本兜底。

做法（改完屏上文案直接重跑本脚本，不需要手工维护字符清单）：
  1. 扫描 main/ember*.c / *.h 里所有字符串字面量的非 ASCII 字符；
  2. 并入 tools/_ember_symbols.txt 已有符号（防止历史已覆盖字形被挤掉）；
  3. 并入 EXTRA 保险字符与可打印 ASCII 全量；
  4. 调 lv_font_conv 生成 assets/fonts/ember_cjk_16.c（16px / bpp 3）。

前置：
  - assets/fonts/SourceHanSansCN-Normal.otf（思源黑体 CN，SIL OFL 1.1）
  - node + lv_font_conv（托管 node workspace 或 /tmp/emberfont 二选一）

生成后必须再跑一次校验（0 缺字才算过关）：
  python tools/check_ember_font.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OTF = os.path.join(ROOT, "assets", "fonts", "SourceHanSansCN-Normal.otf")
OUT_C = os.path.join(ROOT, "assets", "fonts", "ember_cjk_16.c")
SYMBOLS_TXT = os.path.join(ROOT, "tools", "_ember_symbols.txt")

# 所有会出现中文文案的固件源码；缺一个就可能在真机上出方框
SRC_FILES = [
    "main/ember.c",
    "main/ember_color.c",
    "main/ember_color.h",
    "main/ember_main.c",
]

# 屏上可能出现、但不一定写在字符串字面量里的字符
# U+3000 全角空格：键提示「◁ 观火　〇 添柴」的中文间距，漏了会显示方框
EXTRA = "　◁〇▷·—‘’“”…←↑→↓■◆○●、《》【】％～￥"

FONT_SIZE = 16
BPP = 3


def find_node():
    base = r"C:/Users/8605464/.workbuddy/binaries/node/versions"
    if os.path.isdir(base):
        for d in sorted(os.listdir(base), reverse=True):
            exe = os.path.join(base, d, "node.exe")
            if os.path.exists(exe):
                return exe
    return "node"


def find_conv():
    cands = [
        r"C:/Users/8605464/.workbuddy/binaries/node/workspace/node_modules/lv_font_conv/lv_font_conv.js",
        r"/tmp/emberfont/node_modules/lv_font_conv/lv_font_conv.js",
    ]
    for c in cands:
        if os.path.exists(c):
            return c
    return cands[0]


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def build_symbols():
    chars = set()
    for rel in SRC_FILES:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            raise SystemExit("找不到 %s" % p)
        with open(p, "r", encoding="utf-8") as f:
            src = strip_comments(f.read())
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for ch in lit:
                if ord(ch) > 0x7F:
                    chars.add(ch)
    # 已有清单并入：历史覆盖过的字形不因为这次重跑而丢失
    if os.path.exists(SYMBOLS_TXT):
        with open(SYMBOLS_TXT, "r", encoding="utf-8") as f:
            chars |= set(f.read())
    chars |= set(EXTRA)
    chars |= {chr(c) for c in range(0x20, 0x7F)}
    return "".join(sorted(chars))


def main():
    node, conv = find_node(), find_conv()
    if not os.path.exists(conv):
        print("[FAIL] 找不到 lv_font_conv：%s" % conv)
        return 2
    if not os.path.exists(OTF):
        print("[FAIL] 缺少源字体：%s" % OTF)
        return 2

    symbols = build_symbols()
    with open(SYMBOLS_TXT, "w", encoding="utf-8", newline="\n") as f:
        f.write(symbols)
    print("字符清单：%d 个 -> %s" % (len(symbols), SYMBOLS_TXT))

    cmd = [
        node, conv,
        "--font", OTF,
        "--format", "lvgl",
        "--size", str(FONT_SIZE),
        "--bpp", str(BPP),
        "--symbols", symbols,
        "--output", OUT_C,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.stdout:
        print(r.stdout.strip())
    if r.returncode != 0:
        print("[FAIL] lv_font_conv 退出码 %d\n%s" % (r.returncode, r.stderr))
        return 1

    with open(OUT_C, "r", encoding="utf-8", errors="replace") as f:
        tail = f.read()[-4000:]
    if "ember_cjk_16" not in tail:
        print("[FAIL] 产物里找不到符号 ember_cjk_16")
        return 1
    print("[PASS] 生成 %s  (%.1f KB)" % (OUT_C, os.path.getsize(OUT_C) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
