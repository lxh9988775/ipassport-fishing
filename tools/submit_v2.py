#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""submit_v2.py —— 组装并执行「竿影浮标 v2」的上架提交。

为什么单独写一个脚本：中英标题/简介/更新日志里有中文标点与空格，直接在 shell
里拼参数容易被打乱；这里用列表 argv 直接交给 publisher.py，一个字都不会错。

用法：
    python tools/submit_v2.py                 # 只打印「新建」预览（不上传）
    python tools/submit_v2.py --auto          # 带 --auto 真正新建上传
    python tools/submit_v2.py --update        # 打印「更新」预览（目标 733）
    python tools/submit_v2.py --update --auto # 真正提交更新

注意（平台规则）：
  - 更新必须拿到 scope=update_once 且 projectId 匹配的**新**授权码，
    上次那张「新建」授权不能复用（SKILL.md:159）。跑之前先 `whoami` 确认。
  - 更新不带任何 remix 标记；不传 --tag（保留平台已有标签）。
  - 更新必须提交完整版本：固件 + 3:4 封面 + 全部配图 + 中英更新日志。
"""
from __future__ import annotations

import os
import runpy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PUB = ROOT / "assets" / "publish" / "v2"
BASE_URL = "https://ai-passport.folotoy.cn"
REPO_URL = "https://github.com/lxh9988775/ipassport-fishing"

# 被审核打回的那个玩法；更新时必须与授权码里的 projectId 一致
UPDATE_PROJECT_ID = "733"

PUBLISHER = (
    Path.home() / ".workbuddy" / "skills" / "folotoy-ai-passport-publisher"
    / "scripts" / "publisher.py"
)


def read(name: str) -> str:
    return (PUB / name).read_text(encoding="utf-8").strip()


def build(update: bool) -> list[str]:
    argv = [
        "publisher.py", "submit",
        "--kind", "firmware",
        "--title-zh", "竿影浮标 · 钓鱼图鉴",
        "--title-en", "Rod & Bobber: The Fishing Codex",
        "--description-zh", read("description_zh.txt"),
        "--description-en", read("description_en.txt"),
        "--instructions-zh-file", str(PUB / "instructions_zh.txt"),
        "--instructions-en-file", str(PUB / "instructions_en.txt"),
        "--category", "games",
        "--firmware", str(ROOT / "FoloToy-AI-Passport-full.bin"),
        "--cover", str(PUB / "fishing-cover-3x4.png"),
        "--image", str(PUB / "fishing-shot-codex-3x4.png"),
        "--image", str(PUB / "fishing-shot-scenes-3x4.png"),
        "--source-url", REPO_URL,
    ]

    if update:
        # 更新：指定目标 + 中英更新日志；不带 tag（保留已有）、不带 remix 标记
        argv += [
            "--project-id", UPDATE_PROJECT_ID,
            "--changelog-zh-file", str(PUB / "changelog_zh.txt"),
            "--changelog-en-file", str(PUB / "changelog_en.txt"),
        ]
    else:
        # 新建：带上创作者可选标签
        argv += ["--tag", "family"]

    if "--auto" in sys.argv:
        argv.append("--auto")
    return argv


def main() -> int:
    update = "--update" in sys.argv
    os.environ["FOLOTOY_AI_PASSPORT_URL"] = BASE_URL
    if not PUBLISHER.is_file():
        raise SystemExit("publisher.py not found: %s" % PUBLISHER)

    argv = build(update)
    print("模式：%s%s" % ("更新 project " + UPDATE_PROJECT_ID if update else "新建",
                         "（--auto 真上传）" if "--auto" in sys.argv else "（仅预览）"))
    for p in argv:
        if p.startswith(str(ROOT)):
            print("   文件 %s  %s" % (Path(p).name,
                                      "存在" if Path(p).is_file() else "<<< 缺失!"))
    sys.argv = argv
    runpy.run_path(str(PUBLISHER), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
