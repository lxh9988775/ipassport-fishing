#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_ui_layout.py —— 屏幕布局护栏：出屏 / 被圆角涂黑 / 元素互相重叠。

为什么需要这个脚本：
这块屏只有 240x320，四角还有 30px 圆角遮罩（BSP_LVGL_SCREEN_RADIUS），被遮区域
一律涂纯黑；而中文是 16px 一个字，一行放不下几个字。以前只靠肉眼和"单行是否超
232px"来判断，看不出两类致命问题——"两个元素互相压住"和"文字落进被涂黑的圆角里"。
平台审核打回的「电量显示和文字重叠」正是前者。

本脚本所有判定都取源码真值，不用估计值：
  * 元素坐标：解析 main/fishing.c 的 make_label / lv_obj_create / set_pos / set_size /
              set_width / set_style_pad_all / lv_obj_align
  * 真实字宽：解析 assets/fonts/fishing_cjk_16.c 的逐字 adv_w（定点 1/16 px），
              不依赖 Pillow 或 OTF —— 和固件里跑的完全是同一份度量
  * 圆角判据：复刻 components/bsp/src/bsp_display_rounding.c 的逐行可见区间
  * 最坏情况：%d -> 8888、%s -> 名称表里的最长实例，按上限而不是样例判定

退出码：0 = 全部通过；1 = 有违规；2 = 源码解析失败（防止"解析不出来就静默放行"）。
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_C = os.path.join(ROOT, "main", "fishing.c")
LOGIC_C = os.path.join(ROOT, "main", "fishing_logic.c")
FONT_C = os.path.join(ROOT, "assets", "fonts", "fishing_cjk_16.c")
FONT_C12 = os.path.join(ROOT, "assets", "fonts", "fishing_cjk_12.c")
BSP_H = os.path.join(ROOT, "components", "bsp", "include", "bsp_display.h")

SCR_W, SCR_H = 240, 320

# 运行期互斥的面板（来源：show_only() 的 which 编号）
PANELS = {"g_scene": 0, "g_reel": 1, "g_result": 2, "g_menu": 3, "g_codex": 4,
          "g_spot_pick": 5, "g_sub_pick": 6, "g_cdx_list": 7}
# 铺满整屏的层：四角被圆角涂黑是设计的一部分（遮的是背景边缘，不是内容）
# g_tint 是氛围染色层，同样铺满整屏、同样只判出屏不参与压盖关系。
FULL_BLEED = set(PANELS) | {"g_bg", "hud", "g_tint", "hint_bar"}

# 最坏情况代入：把 snprintf 模板换成"能出现的最长一行"，按上限判定
SUBST = {
    "得分 %d": "得分 8888",
    # v6 鱼探：深度最多 12m、收线百分比最多 100%
    "%d.%dm": "12.9m",
    "%d%%": "100%",
    "最高 %d": "最高 8888",
    "%d": "100",                       # 电量百分比，上限 100（三位）
    "鱼竿   %s": "鱼竿   路亚竿",
    "饵料   %s": "饵料   蚯蚓",
    "钓点   %s": "钓点   静水塘",
    "钓点   %s(未解锁)": "钓点   静水塘(未解锁)",
    "再钓 %d 条解锁 %s": "再钓 8888 条解锁 路亚竿",
    "图鉴   %d/%d": "图鉴   24/24",
    # 源码里写的是 \n 这两个字符，所以键用 raw 字符串才匹配得上
    r"收线 %d%%\n%s": r"收线 100%\n小鲨鱼",
    "%s  %s": "小鲨鱼  传说",
    r"%d.%d 厘米 · %d 克\n本次 +%d 分%s": r"888.8 厘米 · 8888 克\n本次 +8888 分\n完美钓获！",
    "%d. %s · %s%s": "24. 小鲨鱼 · 传说 ✓",
    "%d. ???": "24. ???",
    # 最坏取自 SPECIES 表与 uint8 计数上限：最长个体旗鱼 2500mm=250.0 厘米、单种最多 255 条
    "最大 %d.%d 厘米 · 已钓 %d 条": "最大 250.0 厘米 · 已钓 255 条",
    # 体长范围 [min,max]：最短白条/马口 100mm=10 厘米，最长旗鱼 2500mm=250 厘米
    "体型 %d-%d 厘米 · 爱%s": "体型 10-250 厘米 · 爱蚯蚓",
    "体型 %d-%d 厘米": "体型 10-250 厘米",
    "%s · 栖息于%s": "传说 · 栖息于静水塘",
    "收录 %d/%d   累计钓获 %d": "收录 24/24   累计钓获 8888",
    # 图鉴底部两行（收集进度 + 里程碑），以及左上角环境标签
    "收录 %d/%d（%d%%）  钓获 %d": "收录 24/24（100%）  钓获 8888",
    "集齐 %d 种 · %s": "集齐 24 种 · 竿影大成",
    "已集齐 %d 种 · %s": "已集齐 24 种 · 竿影大成",
    # v4 环境标签变两行：第1行时段·天气，第2行当日鱼讯（取最长一句）
    r"%s · %s\n%s": "黄昏 · 雨\n水色微浑，正是钓底的好时候",
    # v4 履历行：首次第 N 日（最早=第1日位数最少，取第999日最坏）+ 最大重量（旗鱼 80000g=80.0千克）
    "首次 %s · 最大 %s": "首次 第999日 · 最大 80.0千克",
    # v5 C+2 新增页面文案
    "%d 处标点": "10 处标点",
    "%s · 选标点": "夜钓栈桥 · 选标点",
    "出鱼：%s": "出鱼：石斑鱼/小鲨鱼",
    "%s · %s": "夜钓栈桥 · 潮汐道",
    "%s·%s": "夜钓栈桥·潮汐道",
    "%d. %s": "27. 小鲨鱼",
    "%d.%dcm": "250.0cm",
    "%d-%dcm": "10-250cm",
    "最大 %d.%d 厘米 · %d.%d千克": "最大 250.0 厘米 · 80.0千克",
    "最大 %d.%d 厘米 · 重量未记录": "最大 250.0 厘米 · 重量未记录",
    "首次 %s · 标点未知": "首次 第999日 · 标点未知",
    "首次 %s · %s": "首次 第999日 · 潮汐道",
    "已收录 %d 种 · 标点 %d 处": "已收录 27 种 · 标点 10 处",
    "%s": "夜钓栈桥",
}
WIDEST_S = "路亚竿"
WIDEST_D = "8888"

# 无法从源码解析出文案的表达式 -> 最坏实例
EXPR_FALLBACK = [
    ("spot_name", "静水塘"),
    ("rod_name", "路亚竿"),
    ("bait_name", "蚯蚓"),
    ("rarity_name", "传说"),
    ("f->name", "小鲨鱼"),
    ("res->name", "小鲨鱼"),
]

# 刻意的重叠（选中高亮条压在文字下、轨道里放捕捉区与鱼标）
ALLOW_OVERLAP = {
    frozenset(("g_menu_selbar", "g_menu_rows")),
    frozenset(("g_menu_selbar", "menu_row0")),
    frozenset(("g_menu_selbar", "menu_row1")),
    frozenset(("g_menu_selbar", "menu_row2")),
    frozenset(("g_menu_selbar", "menu_row3")),
    frozenset(("g_menu_selbar", "menu_row4")),
    frozenset(("g_menu_selbar", "menu_row5")),
    # 概念图 bevel 卡片三件套：卡片底 / 选中整行浅底 / 左侧红竖条刻意层叠，
    # 文字（menu_keyN / menu_rowN）压在浅底之上是设计本身。
    *[frozenset(("g_menu_selbg", "menu_card%d" % i)) for i in range(6)],
    *[frozenset(("g_menu_selbar", "menu_card%d" % i)) for i in range(6)],
    frozenset(("g_menu_selbg", "g_menu_selbar")),
    *[frozenset(("g_menu_selbg", "menu_key%d" % i)) for i in range(6)],
    *[frozenset(("g_menu_selbg", "menu_row%d" % i)) for i in range(6)],
    *[frozenset(("g_menu_selbar", "menu_key%d" % i)) for i in range(6)],
    *[frozenset(("g_menu_selbar", "menu_row%d" % i)) for i in range(6)],
    frozenset(("g_zone", "g_fishmark")),      # 鱼在捕捉区内是玩法本身
    frozenset(("prg_bg", "g_prg_fill")),
    frozenset(("g_batt_body", "g_batt_fill")),
    # v6 鱼探：竿速文字叠在选中绿块上（绿块是按钮底、文字在上，设计如此）
    frozenset(("lbl_gear0", "g_gear0")),
    frozenset(("lbl_gear1", "g_gear1")),
}
# 容器/背景层：只判出屏，不参与"谁压住谁"（它们本来就是铺满的底）
CONTAINERS = set(PANELS) | {"g_bg", "hud", "g_tint", "hint_bar"}

NOTES: list[str] = []


def note(msg: str) -> None:
    if msg not in NOTES:
        NOTES.append(msg)


# ---------------------------------------------------------------------------
# 字体度量（直接读固件里那份 C 字库）
# ---------------------------------------------------------------------------
def load_font(path=None):
    src = open(path or FONT_C, encoding="utf-8").read()
    if "glyph_bitmap[]" not in src or "glyph_dsc[]" not in src:
        return None
    seg = src[src.index("glyph_bitmap[]"):src.index("glyph_dsc[]")]
    cps = [int(m, 16) for m in re.findall(r"/\*\s*U\+([0-9A-Fa-f]{4,6})\s+\"", seg)]
    dsc = src[src.index("glyph_dsc[]"):]
    dsc = dsc[:dsc.index("};")]
    advs = [int(x) for x in re.findall(r"\.adv_w\s*=\s*(\d+)", dsc)]
    if advs:
        advs = advs[1:]
    if not cps or len(cps) != len(advs):
        return None
    lh = re.search(r"\.line_height\s*=\s*(\d+)", src)
    return {
        "w": {chr(c): a / 16.0 for c, a in zip(cps, advs)},
        "line_height": int(lh.group(1)) if lh else 20,
    }


def text_w(font, s: str) -> float:
    total = 0.0
    for ch in s:
        if ch in font["w"]:
            total += font["w"][ch]
        else:
            if ch not in "\x00":
                note("字库缺字 %r，按 16px 计" % ch)
            total += 16.0
    return total


def wrap_lines(font, text: str, width: float):
    """LVGL 对 CJK 会在行宽处逐字断行；这里按同样规则贪心折行。"""
    out = []
    for para in text.split("\n"):
        cur, cur_w = "", 0.0
        for ch in para:
            cw = font["w"].get(ch, 16.0)
            if cur and cur_w + cw > width:
                out.append(cur)
                cur, cur_w = ch, cw
            else:
                cur += ch
                cur_w += cw
        out.append(cur)
    return out


# ---------------------------------------------------------------------------
# 圆角可见区间（复刻 bsp_display_rounded_row_span）
# ---------------------------------------------------------------------------
def rounded_row_span(y: int, radius: int, w: int = SCR_W, h: int = SCR_H):
    if y < 0 or y >= h:
        return None
    r = min(radius, min(w, h) // 2)
    if r <= 0 or (y >= r and y < h - r):
        return 0, w - 1
    edge = r - y if y < r else y - (h - 1 - r)
    inset = 0
    while (inset + 1) ** 2 + edge ** 2 <= r * r:
        inset += 1
    x1, x2 = max(0, r - inset), min(w - 1, w - r + inset - 1)
    return (x1, x2) if x1 <= x2 else None


def read_radius() -> int:
    src = open(BSP_H, encoding="utf-8").read()
    m = re.search(r"BSP_LVGL_SCREEN_RADIUS\s+(\d+)", src)
    if not m:
        raise SystemExit("check_ui_layout: BSP_LVGL_SCREEN_RADIUS not found")
    return int(m.group(1))


def longest_logic_text() -> str:
    """g_cdx_desc 的内容来自 SPECIES[].desc：拿最长的一条当最坏值算折行高度。"""
    if not os.path.isfile(LOGIC_C):
        return ""
    src = open(LOGIC_C, encoding="utf-8").read()
    cand = [s for s in re.findall(r'"((?:[^"\\]|\\.)*)"', src)
            if len(s) >= 8 and re.search(r"[\u4e00-\u9fff]", s)]
    return max(cand, key=len) if cand else ""


LONG_TEXT = longest_logic_text()


# ---------------------------------------------------------------------------
# 解析 fishing.c
# ---------------------------------------------------------------------------
def split_functions(src: str):
    """按花括号平衡切出每个函数体，避免"跨函数串文案"。"""
    out = []
    for m in re.finditer(r"^[A-Za-z_][\w \t*]*\s+\**(\w+)\s*\([^;{]*\)\s*\{", src, re.M):
        depth, i = 0, m.end() - 1
        while i < len(src):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out.append((m.group(1), src[m.end():i]))
    return out


def parse_source():
    raw = open(SRC_C, encoding="utf-8").read()
    src = re.sub(r"/\*.*?\*/", "", raw, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)

    macros = {m.group(1): m.group(2)
              for m in re.finditer(r"#define\s+(\w+)\s+([-\w\s()]+?)\s*$", src, re.M)}

    def num(expr):
        e = (expr or "").strip()
        if not e or not re.fullmatch(r"[\w\s+\-*/()]+", e):
            return None
        for _ in range(6):
            new = re.sub(r"\b([A-Za-z_]\w*)\b",
                         lambda m: macros.get(m.group(1), m.group(0)), e)
            if new == e:
                break
            e = new
        if re.search(r"[A-Za-z_]", e):
            return None
        try:
            return float(eval(e, {"__builtins__": {}}, {}))
        except Exception:
            return None

    objs: dict[str, dict] = {}

    def get(name):
        if name not in objs:
            objs[name] = {"parent": None, "x": None, "y": None, "w": None, "h": None,
                          "pad": 0, "wrap": False, "dot": False, "auto": None,
                          "texts": set(), "kind": "obj", "line_space": 0,
                          "long_text": False, "small": False}
        return objs[name]

    # 元素创建：make_label(parent, "文案", x, y, color)
    for m in re.finditer(
            r"(\w+)\s*=\s*make_label\(\s*(\w+)\s*,\s*\"((?:[^\"\\]|\\.)*)\"\s*,"
            r"\s*([^,]+?)\s*,\s*([^,]+?)\s*,", src):
        o = get(m.group(1))
        o["parent"], o["kind"] = m.group(2), "label"
        o["x"], o["y"] = num(m.group(4)), num(m.group(5))
        if m.group(3):
            o["texts"].add(m.group(3))

    # 元素创建：lv_obj_create(parent) / lv_img_create(parent)
    # 支持 `lv_obj_t *hud = lv_obj_create(scr)` 这种带类型声明的写法
    # （底栏 hint_bar 就用了这种，原先漏登记导致 g_lbl_hint 被当 scr 直属）。
    for m in re.finditer(r"(?:\w+\s+\*?\s*)?(\w+)\s*=\s*(lv_obj_create|lv_img_create|lv_label_create)\(\s*(\w+)\s*\)", src):
        o = get(m.group(1))
        o["parent"] = m.group(3)
        o["kind"] = "img" if "img" in m.group(2) else "obj"

    # v5 像素面板：make_panel(parent, x, y, w, h, color)
    for m in re.finditer(
            r"(\w+)\s*=\s*make_panel\(\s*(\w+)\s*,\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),", src):
        o = get(m.group(1))
        o["parent"] = m.group(2)
        o["kind"] = "obj"
        o["x"], o["y"] = num(m.group(3)), num(m.group(4))
        o["w"], o["h"] = num(m.group(5)), num(m.group(6))

    # v6 鱼探：make_box(parent, x, y, w, h, color) —— 与 make_panel 一样要登记父子关系，
    # 否则这些色块会被当成常驻层，和选钓场/图鉴面板误报 90 处重叠。
    for m in re.finditer(
            r"(\w+)\s*=\s*make_box\(\s*(\w+)\s*,\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),", src):
        o = get(m.group(1))
        o["parent"] = m.group(2)
        o["kind"] = "obj"
        o["x"], o["y"] = num(m.group(3)), num(m.group(4))
        o["w"], o["h"] = num(m.group(5)), num(m.group(6))

    # v5 双行胶囊用 12px 子集字库，不能用 16px 的行高判定
    for m in re.finditer(r"use_cjk12\(\s*(\w+)\s*\)", src):
        if m.group(1) in objs:
            objs[m.group(1)]["small"] = True

    for m in re.finditer(r"lv_obj_set_pos\(\s*(\w+)\s*,\s*([^,]+),\s*([^)]+)\)", src):
        # 动态定位（如按进度算 y）求不出值时保留上一次的静态值，别把元素整条丢掉
        o = get(m.group(1))
        o["x"] = num(m.group(2)) if num(m.group(2)) is not None else o["x"]
        o["y"] = num(m.group(3)) if num(m.group(3)) is not None else o["y"]
    for m in re.finditer(r"lv_obj_set_size\(\s*(\w+)\s*,\s*([^,]+),\s*([^)]+)\)", src):
        o = get(m.group(1)); o["w"], o["h"] = num(m.group(2)), num(m.group(3))
    for m in re.finditer(r"lv_obj_set_width\(\s*(\w+)\s*,\s*([^)]+)\)", src):
        o = get(m.group(1)); o["w"] = num(m.group(2)) or o["w"]
    for m in re.finditer(r"lv_obj_set_style_pad_all\(\s*(\w+)\s*,\s*([^,]+),", src):
        o = get(m.group(1)); o["pad"] = num(m.group(2)) or 0
    for m in re.finditer(r"lv_obj_set_style_text_line_space\(\s*(\w+)\s*,\s*([^,]+),", src):
        o = get(m.group(1)); o["line_space"] = num(m.group(2)) or 0
    for m in re.finditer(r"lv_label_set_long_mode\(\s*(\w+)\s*,\s*(LV_LABEL_LONG_\w+)", src):
        o = get(m.group(1))
        o["wrap"] = m.group(2) == "LV_LABEL_LONG_WRAP"
        o["dot"] = m.group(2) == "LV_LABEL_LONG_DOT"
    for m in re.finditer(r"lv_obj_align\(\s*(\w+)\s*,\s*(LV_ALIGN_\w+)\s*,\s*([^,]+),\s*([^)]+)\)", src):
        o = get(m.group(1)); o["auto"] = (m.group(2), num(m.group(3)) or 0, num(m.group(4)) or 0)

    # 文案：逐函数体按【出现顺序】关联，避免同一个 buf 被顺序复用时串台
    #   snprintf(buf, ...)  -> buf 当前模板集合
    #   ident = "字面量"    -> 累积（switch 里同名变量会被赋多次）
    #   ident = buf         -> 累积 buf 当前模板
    #   set_text(t, buf)    -> 取用后立即清空 buf（对应源码里"写完就用"的节奏）
    for _, body in split_functions(src):
        events = []
        for m in re.finditer(r"snprintf\(\s*(\w+)\s*,[^,]+,\s*\"((?:[^\"\\]|\\.)*)\"", body):
            events.append((m.start(), "snprintf", m.group(1), m.group(2)))
        for m in re.finditer(r"\b(\w+)\s*=\s*\"((?:[^\"\\]|\\.)*)\"", body):
            events.append((m.start(), "lit", m.group(1), m.group(2)))
        for m in re.finditer(r"\b(\w+)\s*=\s*([A-Za-z_]\w*)\s*;", body):
            events.append((m.start(), "alias", m.group(1), m.group(2)))
        # set_text_cached() 是 fishing.c 里的"只在变了才写"包装（内部才调 lv_label_set_text），
        # 主循环里的文案现在都走它；漏掉这个别名等于把这些标签静默移出布局检查。
        for m in re.finditer(r"(?:lv_label_set_text|set_text_cached)\(\s*([\w\[\]]+)\s*,\s*([^;]+?)\)\s*;", body):
            events.append((m.start(), "set", m.group(1), m.group(2).strip()))
        events.sort(key=lambda e: e[0])

        buf_set, ident_sets = {}, {}

        def take(src_name):
            """取用即清空：源码里 buf 是"写完马上用"的顺序复用。"""
            if src_name in buf_set:
                got = set(buf_set[src_name])
                buf_set[src_name] = set()
                return got
            return set(ident_sets.get(src_name, set()))

        for _, kind, a, b in events:
            if kind == "snprintf":
                buf_set.setdefault(a, set()).add(b)
            elif kind == "lit":
                ident_sets.setdefault(a, set()).add(b)
            elif kind == "alias":
                ident_sets.setdefault(a, set()).update(buf_set.get(b, set()))
            else:
                if a in ("l", "hud", "lbl"):
                    continue
                if a not in objs:
                    note("未纳入检查的元素：%s（数组元素或坐标是动态表达式，建议人工过一眼）" % a)
                    continue
                o = objs[a]
                lits = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", b)
                if lits:
                    o["texts"] |= set(lits)
                    continue
                got = take(b)
                if got:
                    o["texts"] |= got
                    continue
                if "desc" in b or "lore" in b:
                    # f->desc / f->lore 的内容都来自 SPECIES 表：取 fishing_logic.c 里
                    # 最长的那条中文文案，按实际折行算高度（lore 最长 22 字 = 2 行）。
                    o["long_text"] = True
                    continue
                for key, val in EXPR_FALLBACK:
                    if key in b:
                        o["texts"].add(val)
                        break
                else:
                    note("无法解析 %s 的文案表达式：%s" % (a, b[:60]))
                    o["texts"].add("?\x00")

    elems = {n: o for n, o in objs.items()
             if n not in ("hud", "l", "lbl", "parent")
             and (o["x"] is not None and o["y"] is not None or o["auto"])
             and (o["kind"] != "label" or o["texts"] or o["long_text"])}
    # align 定位的容器（如 hint_bar）x/y 缺省 0，真实位置在 boxes 里按 align 算
    for o in elems.values():
        if o["x"] is None:
            o["x"] = 0
        if o["y"] is None:
            o["y"] = 0

    # 面板内边距防线：lv_obj_create() 的默认样式带约 13px 内边距，子元素坐标
    # 相对【内容区】。不清零的话，所有面板子元素都会比源码写的往右下偏 13px，
    # 而本检查器只加父级 x/y、不计父级内边距 —— 于是"检查通过"但实机错位。
    # （实测症状：菜单选中条低 12~13px、右上角胶囊被屏幕右缘切掉、
    #   图鉴最后一行被底部提示条压住。）所以这里强制要求每个面板显式清零。
    for panel in sorted(PANELS):
        if not re.search(r"lv_obj_set_style_pad_all\(\s*%s\s*," % re.escape(panel), raw):
            note("面板 %s 没有显式 lv_obj_set_style_pad_all(...,0,0)："
                 "lv_obj_create 默认约 13px 内边距会让它的子元素整体偏 13px，"
                 "而本检查器不计父级内边距，会误判为「通过」" % panel)

    # 覆盖率防线：源码里有几处设置文案，就得解析出几处。
    # 少解析=漏检，必须报出来，不能"解析不出来就当没这回事"。
    # 注意 total 只数【调用点】：set_text_cached 的【函数定义】也长成
    # "set_text_cached(...) {"，但它不是调用点，数进去会天天误报一条。
    call = r"(?:lv_label_set_text|set_text_cached)"
    total = len(re.findall(call + r"\([^;{]*\)\s*;", src))
    parsed = len(re.findall(call + r"\(\s*[\w\[\]]+\s*,\s*[^;]+?\)\s*;", src))
    if parsed < total:
        note("源码有 %d 处 lv_label_set_text，只解析出 %d 处 —— 其余被跳过了，检查不完整"
             % (total, parsed))
    return elems


def worst_text(font, tmpl: str) -> str:
    if tmpl in SUBST:
        return SUBST[tmpl]
    s = tmpl.replace("%%", "\x01")
    s = re.sub(r"%[-0-9.]*[diu]", WIDEST_D, s)
    s = re.sub(r"%[-0-9.]*s", WIDEST_S, s)
    s = s.replace("\x01", "%")
    if not (tmpl in SUBST) and ("%d" in tmpl or "%s" in tmpl):
        note("模板未在最坏情况表内，按通用规则代入：%s" % tmpl)
    return s


def check_spot_row_line_spacing() -> list:
    """X-01 防线：选钓场 5 行内，同一列的文字 y 间距必须 >= 实际字号行高。

    数组元素（g_spot_names[i] 等）被 parse_source 当动态表达式跳过，
    这是真机三行叠字逃过检查的根因。这里直接解析 fishing.c 里
    spot_rowN 段的 make_label(x,y) + use_cjk12() 补上这道断言。
    """
    problems = []
    src = open(os.path.join(ROOT, "main", "fishing.c"), encoding="utf-8").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    lh12 = (load_font(FONT_C12) or {}).get("line_height", 15)
    lh16 = (load_font() or {}).get("line_height", 20)
    for n in range(5):
        m = re.search(r"spot_row%d\s*=\s*make_panel\([^)]*\);(.*?)(?=lv_obj_t\s*\*spot_row|\Z)" % n,
                      src, re.S)
        if not m:
            continue
        block = m.group(1)
        entries = []
        for mm in re.finditer(
                r"(g_spot_\w+)\[\d+\]\s*=\s*make_label\(\s*\w+\s*,[^,]+,\s*([^,]+),\s*([^,]+),", block):
            name, xe, ye = mm.group(1), mm.group(2), mm.group(3)
            try:
                x, y = int(xe.strip()), int(ye.strip())
            except ValueError:
                continue
            tail = block[mm.end():mm.end() + 160]
            small = ("use_cjk12(%s[" % name) in tail
            entries.append((name, x, y, lh12 if small else lh16))
        for i in range(len(entries)):
            for j in range(i + 1, len(entries)):
                a, b = entries[i], entries[j]
                if abs(a[1] - b[1]) > 8:      # x 差大 = 左右并排，不是叠字
                    continue
                gap = abs(a[2] - b[2])
                need = max(a[3], b[3])
                if gap < need:
                    problems.append("[叠字] spot_row%d: %s(y=%d) 与 %s(y=%d) 行距 %d < 行高 %d"
                                    % (n, a[0], a[2], b[0], b[2], gap, need))
    return problems


def check_panel_titles_vs_hud() -> list:
    """X-04 防线：页面标题（匿名 make_label，未赋值→不进 parse_source）
    在屏幕上的实际 y = 面板 y + 标题面板内 y，必须落在 HUD 底边之下。

    真机/对比图曾出现"选钓场"压在 HUD"得分 18"上：
    根因是画图时把面板内坐标(y=10)当屏幕坐标用，漏加面板起点 HUD_H=22。
    这里直接从 fishing.c 解析 make_panel(g_XXX,...) 的 y + 紧随其后的
    make_label("标题", x, y) 的第二参数，算出屏幕 y 并断言 > HUD 底边。
    """
    problems = []
    src = open(os.path.join(ROOT, "main", "fishing.c"), encoding="utf-8").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    hud_bottom = 24  # HUD_H(22) + 2，见 hud 底板 set_size
    for m in re.finditer(
            r"(\w+)\s*=\s*make_panel\(\s*scr\s*,\s*(-?\d+)\s*,\s*(\w+|-?\d+)\s*,", src):
        name, px, py = m.group(1), int(m.group(2)), m.group(3)
        # 面板起点 y：常量 HUD_H=22 或数字
        panel_y = 22 if py.strip() == "HUD_H" else (int(py) if py.lstrip("-").isdigit() else None)
        if panel_y is None:
            continue
        # 面板创建后紧接的 200 字符内找匿名 make_label(parent, "标题", x, y, ...)
        tail = src[m.end():m.end() + 240]
        for lm in re.finditer(
                r"make_label\(\s*%s\s*,\s*\"([^\"]+)\"\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*," % name, tail):
            title, ly = lm.group(1), int(lm.group(3))
            screen_y = panel_y + ly
            if screen_y < hud_bottom:
                problems.append("[标题压HUD] %s 标题「%s」屏幕 y=%d < HUD 底边 %d"
                                % (name, title, screen_y, hud_bottom))
            elif screen_y < hud_bottom + 4:
                problems.append("[标题贴HUD] %s 标题「%s」屏幕 y=%d，距 HUD 底边不足 4px"
                                % (name, title, screen_y))
    return problems


def main() -> int:
    radius = read_radius()
    font = load_font()
    if font is None:
        print("check_ui_layout: 无法解析字体 %s" % FONT_C)
        return 2
    font12 = load_font(FONT_C12) or font
    elems = parse_source()
    if len(elems) < 8:
        print("check_ui_layout: 只解析到 %d 个 UI 元素，源码结构可能变了" % len(elems))
        return 2

    def absolute(name):
        x, y = elems[name]["x"], elems[name]["y"]
        p, guard = elems[name]["parent"], 0
        while p in elems and p not in PANELS and guard < 8:
            x += elems[p]["x"] or 0
            y += elems[p]["y"] or 0
            p = elems[p]["parent"]
            guard += 1
        if p in PANELS:
            x += elems[p]["x"] or 0
            y += elems[p]["y"] or 0
        return x, y

    boxes, texts = {}, {}
    dims = {}                       # name -> (w, h)，先统一算完尺寸再算 align 位置
    for name, o in elems.items():
        pad = o["pad"]
        if o["kind"] == "label":
            f = font12 if o["small"] else font
            if o["long_text"] or not o["texts"]:
                # 鱼简介：长度取自 fishing_logic.c 里最长的一条，按实际折行算高度
                avail = o["w"] or SCR_W
                lines = wrap_lines(f, LONG_TEXT, avail)
                w, h = avail + pad * 2, len(lines) * f["line_height"] + \
                    max(0, len(lines) - 1) * o["line_space"] + pad * 2
                texts[name] = "<鱼简介 %d 字 / %d 行>" % (len(LONG_TEXT), len(lines))
            else:
                worst = max((worst_text(f, t) for t in o["texts"]),
                            key=lambda s: max(text_w(f, ln) for ln in s.split("\n")))
                texts[name] = worst.replace("\n", "\\n")
                avail = o["w"] or SCR_W
                natural = max(text_w(f, ln) for ln in worst.split("\n"))
                if o["wrap"]:
                    w = avail + pad * 2
                    lines = wrap_lines(f, worst, avail)
                    h = len(lines) * f["line_height"] + \
                        max(0, len(lines) - 1) * o["line_space"] + pad * 2
                elif o["dot"]:
                    # LONG_DOT：超宽就在末尾打省略号，绝不会出屏，取两者较小值
                    w = min(natural, avail) + pad * 2
                    h = f["line_height"] + pad * 2
                else:
                    lines = worst.split("\n")
                    w = natural + pad * 2
                    h = len(lines) * f["line_height"] + \
                        max(0, len(lines) - 1) * o["line_space"] + pad * 2
        else:
            w, h = o["w"] or 0, o["h"] or 0
            texts[name] = "<graphic>"
        dims[name] = (w, h)

    # 位置计算：align 对象可能依赖另一个 align 父容器（底栏 label 居中于
    # hint_bar，而 hint_bar 自己 BOTTOM_MID 于 scr）。按依赖深度排序，
    # 保证父容器先算好。
    def chain_len(name, guard=0):
        """父链长度：父一定先于子计算（底栏 label 依赖 hint_bar 的 box）。"""
        p = elems[name]["parent"]
        return 0 if (p not in elems or guard > 8) else 1 + chain_len(p, guard + 1)

    for name in sorted(elems, key=chain_len):
        o = elems[name]
        x, y = absolute(name)
        w, h = dims[name]
        if o["auto"]:                       # lv_obj_align 定位
            mode, dx, dy = o["auto"]
            parent = o["parent"]
            if parent and parent != "scr" and parent in boxes:
                px, py, pw, ph = boxes[parent]
            else:
                px, py, pw, ph = 0, 0, SCR_W, SCR_H
            if "RIGHT" in mode:
                x = px + pw - w + dx
            elif "CENTER" in mode or ("MID" in mode and "LEFT" not in mode and "RIGHT" not in mode):
                x = px + (pw - w) / 2 + dx
            elif "LEFT" in mode:
                x = px + dx
            if "BOTTOM" in mode:
                y = py + ph - h + dy
            elif ("MID" in mode or "CENTER" in mode) and "TOP" not in mode and "BOTTOM" not in mode:
                # LV_ALIGN_CENTER 是纯 CENTER（无 MID 字样），也要垂直居中
                y = py + (ph - h) / 2 + dy
            elif "TOP" in mode:
                y = py + dy
        if w and h:
            boxes[name] = (x, y, w, h)

    problems = []

    # ---- 收线方向契约（屏幕坐标）
    # fishing.c 用 top = TRK_Y + pos * TRK_H / 1000 把逻辑坐标映射到屏幕 y，
    # 也就是 pos 越大越靠屏幕下方。逻辑层 reel_update() 里「按住 = 抬竿」因此
    # 必须是 bar_pos 变小（tests/test_fishing_reel_hold.c 钉住逻辑侧）。
    # 渲染侧一旦被改回反向（例如 TRK_Y + (REEL_TRACK - pos) * ...），按住就会
    # 变成往下钻 —— v2 正是这么错的，玩起来像「按了没反应 / 上不上下不下」。
    # 两边各钉一半，缺一不可。
    flat = re.sub(r"\s+", "", open(SRC_C, encoding="utf-8").read())
    for expr, why in (
            ("TRK_Y+st->reel_bar_pos*TRK_H/1000",
             "捕捉区必须按 reel_bar_pos 递增映射到屏幕 y（数值越大越靠下）"),
            ("TRK_Y+st->reel_fish_pos*TRK_H/1000",
             "鱼标必须与捕捉区共用同一条映射，否则区跟鱼会对不上"),
    ):
        if expr not in flat:
            problems.append("[收线方向] fishing.c 里找不到 `%s` —— %s"
                            % (expr.replace("*", " * ").replace("->", "->"), why))


    # ---- 出屏 + 圆角涂黑
    for name, (x, y, w, h) in boxes.items():
        if x < -0.01 or y < -0.01 or x + w > SCR_W + 0.01 or y + h > SCR_H + 0.01:
            problems.append("[出屏] %-14s %-30s @(%.0f,%.0f) %.1fx%.1f 超出 240x320"
                            % (name, texts[name][:30], x, y, w, h))
            continue
        if name in FULL_BLEED:
            continue
        bad = None
        for yy in range(int(round(y)), int(round(min(y + h, SCR_H))) + 1):
            span = rounded_row_span(yy, radius)
            if span and (x < span[0] - 0.01 or x + w > span[1] + 1.01):
                bad = (yy, span)
                break
        if bad:
            yy, span = bad
            problems.append("[圆角遮挡] %-12s %-30s @(%.0f,%.0f) 宽%.1f 第 y=%d 行只剩 x∈[%d,%d]"
                            % (name, texts[name][:30], x, y, w, yy, span[0], span[1]))

    # ---- 同屏重叠：面板互斥，常驻层压在每个面板之上
    def group_of(name):
        if name in PANELS:
            return name
        p, guard = elems[name]["parent"], 0
        while p and guard < 8:
            if p in PANELS:
                return p
            if p not in elems:
                break
            p = elems[p]["parent"]
            guard += 1
        return "HUD"

    groups = {}
    for name in boxes:
        groups.setdefault(group_of(name), []).append(name)

    def one_pair(a, b, tag):
        if frozenset((a, b)) in ALLOW_OVERLAP:
            return
        # 选中条刻意压在行卡片左侧；底部提示条刻意压在最后一行卡片下沿
        pair = {a, b}
        if "g_spot_selbar" in pair and any(x.startswith("spot_row") for x in pair):
            return
        if "g_lbl_hint" in pair and any(x.startswith(("spot_row", "cdx_row")) for x in pair):
            return
        # 祖先/后代关系：底板包着控件是排版本身，不是"压盖"。
        # 原来只判直接父子，v6 控鱼台多了一层嵌套（g_ctrl_panel -> g_subpanel
        # -> g_reel_bg）就会把自家祖孙误报成重叠。
        def is_ancestor(anc, node):
            p, guard = elems[node]["parent"], 0
            while p and p in elems and guard < 8:
                if p == anc:
                    return True
                p, guard = elems[p]["parent"], guard + 1
            return False
        if elems[a]["parent"] == b or elems[b]["parent"] == a:
            return
        if is_ancestor(a, b) or is_ancestor(b, a):
            return
        ax, ay, aw, ah = boxes[a]
        bx, by, bw, bh = boxes[b]
        ox = min(ax + aw, bx + bw) - max(ax, bx)
        oy = min(ay + ah, by + bh) - max(ay, by)
        if ox > 0.5 and oy > 0.5:
            problems.append("[重叠]%-6s %s「%s」 压住 %s「%s」 %.1fx%.1f px"
                            % (tag, a, texts[a][:20], b, texts[b][:20], ox, oy))

    def live(names):
        return [n for n in names if n not in CONTAINERS]

    def overlaps(names, tag):                     # 组内两两
        names = live(names)
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                one_pair(names[i], names[j], tag)

    def cross(A, B, tag):                         # 跨组（不重复报组内）
        for a in live(A):
            for b in live(B):
                one_pair(a, b, tag)

    hud = groups.get("HUD", [])
    overlaps(hud, "HUD")
    for panel in PANELS:
        if panel in groups:
            overlaps(groups[panel], panel)
            cross(hud, groups[panel], "HUD/")

    # X-01：数组元素不在 parse_source 范围，补一道行距断言，防止叠字复发
    problems += check_spot_row_line_spacing()
    # X-04：匿名 make_label（未赋值的页面标题）不进 parse_source，
    # 曾让"选钓场"压住 HUD 逃过检查。补一道面板标题 vs HUD 的独立断言。
    problems += check_panel_titles_vs_hud()

    print("屏幕 %dx%d，圆角半径 %d px，字库 line_height=%d" %
          (SCR_W, SCR_H, radius, font["line_height"]))
    print("元素 %d 个：常驻层 %d，面板 %s" %
          (len(boxes), len(hud), {k: len(v) for k, v in groups.items() if k != "HUD"}))
    for w in NOTES:
        print("  [warn] %s" % w)
    print()
    if problems:
        for p in problems:
            print(p)
        print("\n布局检查未通过：%d 处问题" % len(problems))
        return 1
    print("布局检查通过：无出屏、无圆角遮挡、无同屏重叠")
    return 0


if __name__ == "__main__":
    sys.exit(main())
