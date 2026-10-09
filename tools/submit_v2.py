#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""submit_v2.py —— 组装并执行「竿影浮标 v2」的上架提交。

为什么单独写一个脚本：中英标题/简介/更新日志里有中文标点与空格，直接在 shell
里拼参数容易被打乱；这里用列表 argv 直接交给 publisher.py，一个字都不会错。

用法：
    python tools/submit_v2.py                 # 只打印「新建」预览（不上传）
    python tools/submit_v2.py --update        # 打印「更新」预览（目标 733）
    python tools/submit_v2.py --update --confirmed   # 预览确认无误后，真正提交更新

注意（平台规则）：
  - 更新要「完整版本」：固件 + 3:4 封面 + 全部配图 + 中英更新日志一次交齐，
    端点是整版替换，不是打补丁，漏传的图片不会被保留。
  - 更新不带任何 remix 标记；不传 --tag（保留平台已有标签）。
  - 授权分两种，先跑 `whoami` 看 scope：
      · scope=update_once 且 projectId 匹配 → 用 --auto（官方一次性授权，无需再问）
      · scope=publisher（浏览器授权，长期有效）→ 先出预览，创作者点头后再 --confirmed
    两种都不能拿 create 授权去更新别人，反之亦然。
  - 固件必须取自 build/（CI 产物）。仓库根目录那份是 10-01 的旧镜像，
    正是线上已公开的 1740 本体，误传会「更新成没更新」。
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
        "--firmware", str(ROOT / "build" / "FoloToy-AI-Passport-full.bin"),
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
    elif "--confirmed" in sys.argv:
        argv.append("--confirmed")
    return argv


def main() -> int:
    update = "--update" in sys.argv
    upload = "--auto" in sys.argv or "--confirmed" in sys.argv
    os.environ["FOLOTOY_AI_PASSPORT_URL"] = BASE_URL
    if not PUBLISHER.is_file():
        raise SystemExit("publisher.py not found: %s" % PUBLISHER)

    argv = build(update)
    print("模式：%s%s" % ("更新 project " + UPDATE_PROJECT_ID if update else "新建",
                         "（真上传）" if upload else "（仅预览，不上传）"))
    for p in argv:
        if p.startswith(str(ROOT)):
            ok = Path(p).is_file()
            extra = ""
            if ok and p.endswith(".bin"):
                import hashlib
                h = hashlib.sha256(Path(p).read_bytes()).hexdigest()
                extra = "  %d bytes  sha256 %s..." % (Path(p).stat().st_size, h[:16])
            print("   文件 %-32s %s%s" % (Path(p).name, "存在" if ok else "<<< 缺失!", extra))
    sys.argv = argv
    runpy.run_path(str(PUBLISHER), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
