#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_sim_tackle.py —— 网页模拟器实测：钓场有没有画出竿/线/漂/饵、
换竿画面会不会变、菜单钓点行会不会给解锁提示。

这是「Device tests」的替代（模拟器实测，非真机）。判据全部走像素扫描，
不用画布坐标反推，绕开模拟器截图的十几像素偏移。

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
def px(img: Image.Image):
    return img.convert("RGB").load()


def dark_in_rod_roi(img: Image.Image) -> int:
    """数「左上那片天」里的近黑像素 —— 改之前这里一根竿都没有。"""
    im = img.convert("RGB")
    W, H = im.size
    p = px(im)
    x0, x1 = int(W * 0.00), int(W * 0.30)
    y0, y1 = int(H * 0.33), int(H * 0.58)
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            r, g, b = p[x, y]
            if r < 70 and g < 70 and b < 80:
                n += 1
    return n


def rod_roi_bytes(img: Image.Image) -> bytes:
    im = img.convert("RGB")
    W, H = im.size
    box = (int(W * 0.00), int(H * 0.33), int(W * 0.30), int(H * 0.58))
    return im.crop(box).resize((60, 40)).tobytes()


def red_bar_row(img: Image.Image) -> int:
    """同一行里命中锐胜红 (153,0,58)±22 的像素数（每 2px 采一次）。"""
    im = img.convert("RGB")
    W, H = im.size
    p = px(im)
    best = 0
    for y in range(0, H, 2):
        n = 0
        for x in range(0, W, 2):
            r, g, b = p[x, y]
            if abs(r - 153) <= 22 and g <= 22 and abs(b - 58) <= 22:
                n += 1
        best = max(best, n)
    return best


def track_pixels(img: Image.Image) -> int:
    im = img.convert("RGB")
    W, H = im.size
    p = px(im)
    n = 0
    for y in range(0, H, 2):
        for x in range(0, W, 2):
            r, g, b = p[x, y]
            if abs(r - 25) <= 14 and abs(g - 53) <= 14 and abs(b - 90) <= 14:
                n += 1
    return n


def classify(img: Image.Image) -> str:
    if red_bar_row(img) >= 75:
        return "menu"
    if track_pixels(img) >= 900:
        return "reeling"
    return "scene"


def hint_bar_region(img: Image.Image) -> bytes:
    """底部提示条那片区域（含药丸底 + 白字）。"""
    im = img.convert("RGB")
    W, H = im.size
    return im.crop((0, int(H * 0.88), W, H)).tobytes()


# --------------------------------------------------------------------------
def shot(page, name: str) -> tuple[Image.Image, str]:
    box = page.evaluate(
        """() => { const c = document.querySelector('#qemu-display');
             const r = c.getBoundingClientRect();
             return {x: r.x, y: r.y, width: r.width, height: r.height}; }"""
    )
    path = os.path.join(OUT, name + ".png")
    page.screenshot(path=path, clip=box)
    img = Image.open(path)
    return img, classify(img)


def long_press(page, ms=3000):
    page.keyboard.down("Enter")
    time.sleep(ms / 1000.0)
    page.keyboard.up("Enter")


def tap(page, key="Enter"):
    page.keyboard.press(key)


def wait_vm(page, max_wait=180):
    for _ in range(max_wait // 2):
        time.sleep(2)
        st = page.query_selector("#runtime-state")
        if st and "已运行" in st.inner_text():
            return True
    return False


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(FW):
        print("缺少固件:", FW)
        return 2

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
        if not wait_vm(page):
            print("模拟器没起来")
            return 2
        page.evaluate("document.activeElement && document.activeElement.blur()")

        # ---- 1) 钓场：竿/线/漂/饵画出来了没有 ----
        img, kind = shot(page, "sim_01_scene_rod0")
        print("场景分类:", kind)
        n_dark = dark_in_rod_roi(img)
        check(n_dark >= 60, "钓场左上方出现鱼竿（近黑像素 %d 个，改前为 0）" % n_dark)
        rod0 = rod_roi_bytes(img)

        # ---- 2) 长按开菜单（状态感知 + 重试）----
        opened = False
        for attempt in range(6):
            cur, k = shot(page, "_probe_%d" % (attempt + 1))
            if k != "scene":
                time.sleep(8)
                continue
            long_press(page)
            img, k = shot(page, "sim_02_menu")
            if k == "menu":
                opened = True
                break
            time.sleep(8)
        check(opened, "长按 OK 打开菜单")
        if not opened:
            browser.close()
            return 1

        # ---- 3) 菜单提示条：光标在「钓点」行 vs 其它行 应该不一样 ----
        h_row0 = hint_bar_region(img)
        for _ in range(3):                       # 0→1→2→3（钓点）
            tap(page, "ArrowDown")
            time.sleep(0.8)
        img_spot, k = shot(page, "sim_03_menu_spot")
        check(k == "menu", "方向键切到「钓点」行后仍在菜单")
        h_row3 = hint_bar_region(img_spot)
        check(h_row0 != h_row3,
              "「钓点」行的提示条与首行不同（=解锁进度提示按行变化）")

        # ---- 4) 换竿：站到「鱼竿」行改值，再回钓场比对竿的像素 ----
        for _ in range(2):                       # 3→2→1（鱼竿）
            tap(page, "ArrowUp")
            time.sleep(0.8)
        tap(page)                                # OK 进入编辑
        time.sleep(0.8)
        tap(page, "ArrowDown")                   # 换下一根竿
        time.sleep(0.8)
        tap(page)                                # OK 确认
        time.sleep(0.8)
        img_rod1_menu, k = shot(page, "sim_04_menu_rod_changed")
        check(k == "menu", "改完鱼竿仍停在菜单")

        tap(page, "ArrowUp")                     # 回到「开始钓鱼」
        time.sleep(0.8)
        tap(page)                                # OK 开始钓鱼 → 回钓场
        time.sleep(4)
        img1, k = shot(page, "sim_05_scene_rod1")
        check(k == "scene", "从菜单回到钓场（当前分类 %s）" % k)
        rod1 = rod_roi_bytes(img1)
        check(rod0 != rod1,
              "换竿后钓场上的鱼竿像素变了（=换竿看得见）")

        browser.close()

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
