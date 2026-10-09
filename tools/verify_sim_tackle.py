#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_sim_tackle.py —— 网页模拟器实测：钓场有没有画出竿/线/漂/饵、
换竿画面会不会变、菜单钓点行会不会给解锁提示。

这是「Device tests」的替代（模拟器实测，非真机）。判据全部走像素扫描，
不用画布坐标反推，绕开模拟器截图的十几像素偏移。

模拟器侧三个必须记住的现实（2026-10-09 重新核对 app.js / runtime.js 后确认）：
  1) 按键表 = {ArrowUp:UP, ArrowDown:DOWN, Enter:OK, p:POWER}，且
     `if (event.target.closest("input, select, button")) return;`
     —— 焦点不在 body 上按键会被整条丢掉，所以启动后必须先 blur。
  2) 只有 CLICK/DOUBLE 走 `sendButtonGesture`，LONG 才会 `setButton(true)`
     真正把 OK 电压拉到 595mV；**光有 PRESS 不会送到运行时**。
     换句话说：想按到固件，要么短按（合成一次点击），要么按住 ≥300ms。
  3) 这个投递不稳定（同一份脚本不同轮次结果会变），所以长按一律"重试 +
     按住期间连拍"，不要写死时间轴。

用法：python tools/verify_sim_tackle.py
产出：assets/verify/sim_*.png（证据图）
"""
from __future__ import annotations

import os
import sys
import time

from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "build", "FoloToy-AI-Passport-full.bin")
OUT = os.path.join(ROOT, "assets", "verify")
URL = "https://openswiftuiproject.github.io/FoloToy-Passport-Simulator/"
CHROME = r"C:\Users\8605464\AppData\Local\ms-playwright\chromium-1228\chrome-win64\chrome.exe"

RESULTS: list[tuple[bool, str]] = []


def check(ok: bool, label: str) -> None:
    RESULTS.append((bool(ok), label))
    print(("  [PASS] " if ok else "  [FAIL] ") + label, flush=True)


# --------------------------------------------------------------------------
# 像素判据
# --------------------------------------------------------------------------
def _stats(img: Image.Image):
    im = img.convert("RGB")
    W, H = im.size
    p = im.load()
    total = W * H
    dark = sky = 0
    best_red_row = 0
    track = 0
    for y in range(0, H, 2):
        red_row = 0
        for x in range(0, W, 2):
            r, g, b = p[x, y]
            if max(r, g, b) < 70:
                dark += 1
            elif b > 150 and r > 110:
                sky += 1
            if abs(r - 25) <= 14 and abs(g - 53) <= 14 and abs(b - 90) <= 14:
                track += 1
            if abs(r - 153) <= 22 and g <= 22 and abs(b - 58) <= 22:
                red_row += 1
        best_red_row = max(best_red_row, red_row)
    n = max(1, (W // 2) * (H // 2))
    return dark / n, sky / n, best_red_row, track


def classify(img: Image.Image) -> str:
    dark_r, sky_r, red_row, track = _stats(img)
    # 顺序很重要：收线页底色也是深蓝，只看"深色占比"会把它认成菜单。
    # 菜单独有的标记是那条锐胜红选中胶囊；收线页独有的是深蓝轨道色块。
    if red_row >= 40:
        return "menu"
    if track >= 300:
        return "reeling"
    if dark_r > 0.85 and sky_r < 0.02:
        return "blank"                     # 开机/重启中：整屏黑
    if sky_r > 0.90 and dark_r < 0.02:
        return "blank"                     # 还没画出第一帧：整屏浅色
    return "scene"


def dark_in_rod_roi(img: Image.Image) -> int:
    """数「左上那片天」里的近黑像素 —— 改之前这里一根竿都没有。

    ROI 按 main/fishing.c 的 ROD_X/ROD_Y（8,116）+ 60x55 的竿估出来，再放宽一圈
    容掉模拟器截图的偏移；用比例而不是绝对像素，画布被缩放（240x320 -> 178x237）
    也不受影响。钓场那片天是浅蓝的，改前 ROI 里近黑像素应该是 0。"""
    im = img.convert("RGB")
    W, H = im.size
    p = im.load()
    n = 0
    for y in range(int(H * 0.33), int(H * 0.58)):
        for x in range(0, int(W * 0.30)):
            r, g, b = p[x, y]
            if max(r, g, b) < 70:
                n += 1
    return n


def rod_roi_bytes(img: Image.Image) -> bytes:
    im = img.convert("RGB")
    W, H = im.size
    box = (0, int(H * 0.33), int(W * 0.30), int(H * 0.58))
    return im.crop(box).resize((60, 40)).tobytes()


def hint_bar_region(img: Image.Image) -> bytes:
    """底部提示条那片区域（药丸底 + 白字）。"""
    im = img.convert("RGB")
    W, H = im.size
    return im.crop((0, int(H * 0.88), W, H)).tobytes()


def menu_index(img: Image.Image):
    """读回菜单光标在第几行 —— 不靠任何绝对坐标，纯自校准。

    为什么不能"红条中心 ÷ 30 再取整"：菜单行和选中胶囊都是 `g_menu` 容器的
    子对象，用的是容器内的相对坐标，容器本身还在 HUD 之下并带内边距
    （实测多出约 12px），再叠上模拟器截图的偏移 —— 直接换算会整体差一行。
    所以这里改成：先找红条中心，再逐行扫出左侧那 5 条文字带的真实位置，
    谁离红条最近就是第几行。容器怎么挪都不影响。
    """
    im = img.convert("RGB")
    W, H = im.size
    p = im.load()

    red_ys = []
    for y in range(H):
        n = 0
        for x in range(0, W, 2):
            r, g, b = p[x, y]
            if abs(r - 153) <= 22 and g <= 22 and abs(b - 58) <= 22:
                n += 1
        if n >= 20:
            red_ys.append(y)
    if not red_ys:
        return None
    bar_cy = (red_ys[0] + red_ys[-1]) / 2.0

    # 左侧那一列行的标题（开始钓鱼/鱼竿/饵料/钓点/图鉴）是屏上唯一的五条白字带
    bands: list[list[int]] = []
    cur = None
    for y in range(int(H * 0.08), int(H * 0.86)):
        n = 0
        for x in range(int(W * 0.03), int(W * 0.55)):
            if min(p[x, y]) > 150:
                n += 1
        if n >= 3:
            cur = [y, y] if cur is None else [cur[0], y]
        elif cur is not None:
            bands.append(cur)
            cur = None
    if cur is not None:
        bands.append(cur)
    # 同一条文字被拆成几段就并回去（间距 < 6px 视为同一条）
    merged: list[list[int]] = []
    for b in bands:
        if merged and b[0] - merged[-1][1] < 6:
            merged[-1][1] = b[1]
        else:
            merged.append(list(b))
    merged = [b for b in merged if b[1] - b[0] >= 3]
    if len(merged) != 5:
        return None                      # 不是干净的菜单画面，别猜
    centers = [(b[0] + b[1]) / 2.0 for b in merged]
    return min(range(5), key=lambda i: abs(centers[i] - bar_cy))


def goto_row(page, want, tries=8):
    """把菜单光标挪到第 want 行（0..4）。每步都读回真实位置，只走最短方向。"""
    img = None
    for _ in range(tries):
        img, kind = shot(page, "_probe_nav")
        if kind != "menu":
            return None, img, kind
        cur = menu_index(img)
        if cur is None:
            time.sleep(1.0)
            continue
        if cur == want:
            return cur, img, kind
        fwd = (want - cur) % 5
        tap(page, "ArrowDown" if fwd <= 2 else "ArrowUp")
        time.sleep(1.3)
    return None, img, "stuck"


def tap_to_scene(page, tries=4):
    """在第 0 行点 OK「开始钓鱼」回钓场。

    模拟器会丢按键，丢了就还停在菜单 —— 所以每轮先确认光标在第 0 行再点，
    点完等一小会儿；没回到钓场就重来。返回 (img, kind)。"""
    img, kind = None, ""
    for _ in range(tries):
        cur, img, kind = goto_row(page, 0)
        if kind != "menu":
            img, kind = wait_kind(page, "scene", tries=8, gap=2.0, tag="_probe_back")
            if kind == "scene":
                return img, kind
            continue
        tap(page)
        img, kind = wait_kind(page, "scene", tries=6, gap=1.5, tag="_probe_back")
        if kind == "scene":
            return img, kind
    return img, kind


def in_menu(page) -> bool:
    _, kind = shot(page, "_probe_nav")
    return kind == "menu"


def settle_row(page, want, tries=6):
    """把光标挪到第 want 行，并确认它「停住了」才算到位。

    为什么非要再确认一次：模拟器的按键会迟到。刚读到「光标在第 1 行」时，
    上一步的余键可能还在路上，读完才落地 —— 光标随即飘走，后面那几下 OK/上下
    就全部打偏。实测出现过：以为在第 1 行按 OK 进编辑，其实光标已经回到第 0 行，
    那下 OK 变成「开始钓鱼」，菜单直接关掉、竿根本没换。

    所以这里挪到位后再空等一拍复读一次，两次一致才返回 want，否则返回 None。
    """
    for _ in range(tries):
        cur, _, kind = goto_row(page, want)
        if kind != "menu" or cur != want:
            return None
        time.sleep(1.0)
        img2, kind2 = shot(page, "_probe_nav")
        if kind2 == "menu" and menu_index(img2) == want:
            return want
    return None


# --------------------------------------------------------------------------
def clean_scratch() -> int:
    """删掉本次运行产生的临时取景帧（_probe*.png），只留 sim_* 证据图。

    轮询/导航时会反复截图，那些帧只用来判状态，不该留在证据目录里。
    """
    n = 0
    for fn in os.listdir(OUT):
        if fn.startswith("_probe") and fn.endswith(".png"):
            os.remove(os.path.join(OUT, fn))
            n += 1
    return n


def shot(page, name: str):
    box = page.evaluate(
        """() => { const c = document.querySelector('#qemu-display');
             const r = c.getBoundingClientRect();
             return {x: r.x, y: r.y, width: r.width, height: r.height}; }"""
    )
    path = os.path.join(OUT, name + ".png")
    page.screenshot(path=path, clip=box)
    img = Image.open(path)
    return img, classify(img)


def wait_kind(page, want, tries=25, gap=3.0, tag="_probe"):
    img, kind = None, ""
    for _ in range(tries):
        img, kind = shot(page, tag)
        if kind == want:
            return img, kind
        time.sleep(gap)
    return img, kind


def long_press_to_menu(page, tries=6):
    """按住期间连拍 —— 菜单是"按住时就开"的，松手后再拍会错过。

    模拟器这份按键投递不稳，所以：① 每次按住 6 秒（固件阈值 700ms，留足余量）；
    ② 按到第 2/4/6 秒各拍一张，任一张命中就算成功；③ 失败就等回到钓场再重来
    （乱按会把状态推进鱼咬钩/收线，那时长按不参与，必须等回钓场）。"""
    for i in range(tries):
        img, kind = wait_kind(page, "scene", tries=20, gap=3.0, tag="_probe_scene")
        if kind != "scene":
            print("  第 %d 轮：等不到钓场（%s），继续" % (i + 1, kind))
            continue
        page.keyboard.down("Enter")
        hit = None
        for wait_s, name in ((2.0, "sim_02_menu"), (2.0, "sim_02b_menu"), (2.0, "sim_02c_menu")):
            time.sleep(wait_s)
            img, kind = shot(page, name)
            if kind == "menu":
                hit = img
                break
        page.keyboard.up("Enter")
        if hit is not None:
            print("  第 %d 次长按命中菜单" % (i + 1))
            return True, hit
        time.sleep(2.0)
    return False, img


def tap(page, key="Enter"):
    page.keyboard.press(key)


# --------------------------------------------------------------------------
def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(FW):
        print("缺少固件:", FW)
        return 2
    print("固件:", FW, os.path.getsize(FW), "bytes")

    with sync_playwright() as pw:
        browser = pw.chromium.launch(
            headless=True, executable_path=CHROME,
            args=["--autoplay-policy=no-user-gesture-required", "--no-sandbox",
                  "--use-gl=swiftshader"],
        )
        page = browser.new_page(viewport={"width": 1000, "height": 900})
        page.goto(URL, wait_until="load", timeout=120000)
        page.wait_for_function(
            "() => { const b = document.querySelector('#firmware-upload');"
            " return b && !b.disabled; }", timeout=120000)
        try:
            page.get_by_role("button", name="我知道了").click(timeout=8000)
        except Exception:  # noqa: BLE001
            pass
        page.set_input_files("#firmware-file", FW)
        for _ in range(90):
            time.sleep(2)
            st = page.query_selector("#runtime-state")
            if st and "已运行" in st.inner_text():
                break
        page.evaluate("document.activeElement && document.activeElement.blur()")
        print("runtime:", page.inner_text("#runtime-state"))

        # ---- 0) 等开机画面走完，落到钓场 ----
        img, kind = wait_kind(page, "scene", tries=30, gap=4.0, tag="_probe_boot")
        check(kind == "scene", "开机后落到钓场画面（当前分类 %s）" % kind)
        if kind != "scene":
            browser.close()
            clean_scratch()
            return 1

        # ---- 1) 钓场：竿/线/漂/饵画出来了没有 ----
        # 注意：开机阶段会先出现几帧"整屏浅色"的空白，那时什么也没有。
        # 所以这里不是拍一张就走，而是等到钓场里真的出现竿为止。
        img, kind, n_dark = None, "", 0
        for _ in range(30):
            img, kind = shot(page, "sim_01_scene_rod0")
            if kind == "scene":
                n_dark = dark_in_rod_roi(img)
                if n_dark >= 40:
                    break
            time.sleep(4)
        check(kind == "scene" and n_dark >= 40,
              "钓场左上方出现鱼竿（分类 %s，ROI 内近黑像素 %d 个，改前该区域恒为 0）"
              % (kind, n_dark))
        rod0 = rod_roi_bytes(img)

        # ---- 2) 长按开菜单（重试 + 按住期间连拍）----
        opened, img_menu = long_press_to_menu(page)
        check(opened, "长按 OK 打开菜单")
        if not opened:
            browser.close()
            clean_scratch()
            return 1

        # ---- 3) 菜单提示条：光标在「钓点」行 vs 首行 应该不一样 ----
        # 用 settle_row 而不是 goto_row：要到"确认停住"为止，否则读到的是
        # 延迟按键落地之前的旧位置，后面的断言全部对着错的画面。
        r0 = settle_row(page, 0)
        img0, _ = shot(page, "_probe_nav")
        print("  菜单光标回到第 0 行 ->", r0)
        h_row0 = hint_bar_region(img0) if r0 == 0 else b""

        r3 = settle_row(page, 3)
        print("  菜单光标移到第 3 行（钓点）->", r3)
        check(r3 == 3, "方向键能把光标挪到「钓点」行（实际 %s）" % r3)
        img3 = None
        if r3 == 3:
            img3 = Image.open(os.path.join(OUT, "_probe_nav.png"))
            img3.save(os.path.join(OUT, "sim_03_menu_spot.png"))
        check(img3 is not None and hint_bar_region(img3) != h_row0,
              "「钓点」行提示条与首行不同（=解锁进度提示按行变化）")

        # ---- 4) 换竿：站到「鱼竿」行改值，再回钓场比对竿的像素 ----
        # 这一整块可重试。成功判据只有一条：回到钓场后竿的像素真的变了。
        # 不能拿"按过了"当成功 —— 模拟器会丢键、会迟到，实测出现过光标飘回第 0 行
        # 导致那下 OK 变成"开始钓鱼"：菜单关掉、竿没换，但流程看着"走完了"。
        row1_ok = stayed_menu = back_ok = rod_changed = False
        n_dark1 = 0
        for attempt in range(3):
            if not in_menu(page):
                long_press_to_menu(page, tries=2)
            if settle_row(page, 1) != 1:
                print("  第 %d 次走不到「鱼竿」行，重开菜单再来" % (attempt + 1))
                continue
            row1_ok = True
            tap(page)                                # OK 进入编辑
            time.sleep(1.4)
            tap(page, "ArrowDown")                   # 换下一根竿
            time.sleep(1.4)
            tap(page)                                # OK 确认
            time.sleep(1.4)
            _, k = shot(page, "sim_04_menu_rod_changed")
            if k != "menu":
                print("  第 %d 次改完掉出了菜单（%s），重开菜单再来" % (attempt + 1, k))
                continue
            stayed_menu = True
            if settle_row(page, 0) != 0:
                print("  第 %d 次改完回不到「开始钓鱼」行，再来" % (attempt + 1))
                continue
            img_b, k = tap_to_scene(page)
            if k != "scene":
                print("  第 %d 次没能回到钓场（%s），再来" % (attempt + 1, k))
                continue
            back_ok = True
            shot(page, "sim_05_scene_rod1")
            img1s = Image.open(os.path.join(OUT, "sim_05_scene_rod1.png"))
            if classify(img1s) != "scene":
                continue
            n_dark1 = dark_in_rod_roi(img1s)
            # 必须先确认画面是钓场再比竿：菜单底色也是深蓝，ROI 里近黑像素一大把，
            # 拿菜单画面来比会"看起来通过了"，其实什么都没测。
            if n_dark1 >= 40 and rod0 != rod_roi_bytes(img1s):
                rod_changed = True
                break
            print("  第 %d 次竿没换成功（近黑像素 %d），再来" % (attempt + 1, n_dark1))

        check(row1_ok, "方向键能把光标挪到「鱼竿」行")
        check(stayed_menu, "改完鱼竿仍停在菜单")
        check(back_ok, "从菜单回到钓场")
        check(rod_changed,
              "换竿后钓场上的鱼竿像素变了（=换竿看得见；近黑像素 %d）" % n_dark1)

        browser.close()

    clean_scratch()
    print()
    bad = [l for ok, l in RESULTS if not ok]
    print("=" * 60)
    if bad:
        print("FAILED %d/%d" % (len(bad), len(RESULTS)))
        for l in bad:
            print("  -", l)
        return 1
    print("ALL %d SIMULATOR CHECKS PASSED" % len(RESULTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
