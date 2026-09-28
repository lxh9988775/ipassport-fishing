# 钓鱼玩法「竿影浮标」— 构建与上传手册

本目录是一个**完整可编译的 ESP-IDF 项目**：在官方 FoloToy/ai-passport 基础上，
把 `main/` 换成了钓鱼小游戏（逻辑层 + 应用层 + 音效），其余 BSP / 分区 / 构建配置
沿用官方。目标板：**AI Passport（ESP32-C3 / 8MB Flash / 240×320 屏）**。

游戏操作（三键，无触屏，**界面全中文**）：
- **OK 短按**：待机时抛竿；咬钩窗口内提竿（关键！）
- **OK 长按**：进入 / 退出「饵料 / 钓点」菜单
- **上 / 下**：菜单内切换要编辑的项（OK 在「饵料↔钓点」间切换），再上下调具体值
- 饵料：蚯蚓 / 面团 / 亮片；钓点：静水塘 / 急流河 / 深海（深海分高但咬钩窗口更短）
- 最高分存 NVS，断电不丢

### 屏上中文是怎么实现的（重要）

LVGL 自带字体都不够用：`Montserrat` 只有拉丁字母；LVGL 内置的
`lv_font_source_han_sans_sc_16_cjk` **看着像中文全字库，其实只是 1187 字形的子集**——
实测它缺本游戏核心用字（`钓 鱼 饵 蚯 蚓 咬 抛 竿 塘 钩 选 择 按 编 辑`），屏上会显示方框。

所以改用 `lv_font_conv` 自己生成**只含屏上用字的专用子集字库**：
`assets/fonts/fishing_cjk_16.c`（150 字形 / 88 KB，符号 `lv_font_fishing_cjk_16`）。

- 字符集**自动**从 `main/fishing.c` 抓取，改文案不用手工同步清单
- 改完文案后重新生成 + 校验（两步都必须跑）：
  ```bash
  python tools/gen_font.py            # 重新生成字库
  python tools/check_cjk_coverage.py  # 端到端查缺字，通过才可提交
  ```
- 源字体来源、授权与复现细节见 `assets/fonts/README.md`

---

## 路线：云端自动编译（已选定，电脑不用装任何工具）

链路：**推代码到 GitHub → GitHub 云端自动编译 → 下载 full.bin → 官网发布技能上传社区**

编译由工作流 `.github/workflows/firmware-checks.yml` 完成（push 到 `main` 自动触发）：
- 环境：GitHub ubuntu-latest 跑 **`espressif/idf:v5.5.3`** 容器 + target `esp32c3`
- 命令：`./tools/validate.sh --firmware`（编译 → `merge-bin` 合并整镜像 → 校验分区/≤8MB）
- 产物：`build/FoloToy-AI-Passport-full.bin`，同时自动发布到 `build-artifacts` 分支（方便直取）

---

## 第 1 步：把代码推到 GitHub（公开仓库）

你的仓库已经建好并且是公开的：
```
https://github.com/lxh9988775/ipassport-fishing
```
本地已有 2 个提交等待推送。用 GitHub Desktop：
1. 打开 GitHub Desktop（当前仓库应为 `AIpassport`）
2. 顶部 **Sign in**（或右上角 Push 时提示登录）→ 浏览器打开 **github.com** 登录并授权
3. 回到 Desktop 点 **Push origin**
4. 推完在浏览器打开上面那个链接，应能看到 `main/`、`components/`、`docs/` 等文件

> ⚠️ **推不上时的排查**：本机曾有一条"GitHub 加速"全局规则
> `url.https://ghproxy.net/https://github.com/.insteadOf = https://github.com/`，
> 它会把**登录地址和推送地址**都改写成镜像站 `ghproxy.net`，导致：
> ① 登录页打不开（Invalid input）；② 推送时"找不到 ghproxy 的凭据"而失败。
> 该规则已删除（原配置备份在 `C:/Users/8605464/.gitconfig.backup-aipassport`）。
> 如日后还想为 `git clone` 加速而恢复，请只对 clone 单次使用，不要再设全局规则。

## ✅ 编译状态：中文版已通过（2026-09-28，commit 82ddc54）

云端编译跑出**绿勾**，固件校验全过：
- app 分区占用：**706448 / 8323072 字节**（factory 分区，剩余 92%）
- 合并整镜像 `FoloToy-AI-Passport-full.bin` = **771984 字节（753.9 KB）**，
  从 `0x0` 起，日志 `Merged firmware: PASS`
- SHA-256：`9e45d0b0deee6acd9e76a40eadb90f94e885cf6455c48a2863ddf156b975237a`
- 产物已**自动落到你本地工作区**：
  ```
  C:\Users\8605464\Desktop\AIpassport\FoloToy-AI-Passport-full.bin
  ```
  这就是能上传社区的最终固件，**不用再去 Actions 下载**了。

> 中文版比英文版只大 **14 KB**（150 个字形的子集字库实际只占约 14 KB Flash），
> 距离 8 MB 上限还差得远。

> 备用取回方式（万一本地丢了）：固件同时发布在 `build-artifacts` 分支，
> 用 SSH `git clone --branch build-artifacts git@github.com:lxh9988775/ipassport-fishing.git` 取回。

## 第 2 步：确认固件在手上

检查工作区里有这个文件即可（已自动生成）：
```
FoloToy-AI-Passport-full.bin   ≈ 740KB
```
> ⚠️ 千万别用 `idf.py build` 单独产出的 **app-only** bin（缺 bootloader/分区表），
> 社区校验会拒。必须是上面这个从 `0x0` 起的整镜像。

## 第 3 步：上传到社区（官网发布技能）

按官方 `docs/development/release/publish-to-community.md`，上传靠官网的**发布技能**，
不是手动拖文件。流程：
1. 安装发布技能：`https://ai-passport.folotoy.cn/skills/folotoy-ai-passport-publisher.zip`
2. 四样东西**都已备好**（下面附可直接粘贴的文案）：
   - **固件**：`C:\Users\8605464\Desktop\AIpassport\FoloToy-AI-Passport-full.bin`（753.9 KB）
   - **封面图**：`assets/cover/fishing-cover.png`（1024×1024，582 KB，已去掉 AI 生成水印）
   - **双语标题 + 简介**：见下方「可直接粘贴的文案」
   - **Git 源码地址**：`https://github.com/lxh9988775/ipassport-fishing`
3. 在官网注册 / 登录**授权**，逐项预览确认后再上传（助手不碰你的密码）

> 📋 逐字段核对清单见 `上架材料清单.html`（双击用浏览器打开即可）。

---

### 可直接粘贴的文案（中英双语）

**标题（中文）**：竿影浮标 · 钓鱼忙
**标题（English）**：Rod & Float — Fishing Mini-Game

**简介（中文）**：
> 一款为 AI Passport 打造的掌上钓鱼小游戏。抛竿 → 等鱼咬钩 → 限时提竿，钓到的鱼越大分越高。
> 全中文界面，三键操作无需触屏：OK 抛竿 / 提竿，长按 OK 进入菜单，上下键切换饵料与钓点。
> 三种饵料（蚯蚓 / 面团 / 亮片）× 三种钓点（静水塘 / 急流河 / 深海）——深海分高但咬钩窗口更短，
> 亮片更容易钓上大鱼。含限时咬钩窗口、计分、最高分掉电不丢（NVS）、音效与实时电量显示。
> 纯 LVGL 绘制、零外部素材，适配 8MB Flash。

**简介（English）**：
> A pocket fishing mini-game for the AI Passport. Cast, wait for the bite, then strike in time
> to reel in fish — the bigger the catch, the higher the score. Chinese UI, three buttons, no
> touchscreen: OK to cast / strike, long-press OK for the menu, UP / DOWN to switch baits and
> fishing spots. Three baits (worm / bread / lure) × three spots (pond / river / sea) — the sea
> pays more but the bite window is shorter, and the lure attracts bigger fish. Includes a timed
> bite window, scoring, a persistent high score saved to NVS, sound effects and a live battery
> indicator. Pure LVGL UI with zero external assets, tuned for 8 MB flash.

---

## 附：本地编译（可选，不装也能走完全流程）

若哪天要脱离网络编译：装 **ESP-IDF 5.5.3** 后
```bash
cd AIpassport
idf.py build
idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin
./tools/validate.sh --firmware        # 可选，官方校验
```
真机烧录：`idf.py flash` 或 `esptool.py write_flash 0x0 build/FoloToy-AI-Passport-full.bin`。
浏览器模拟器（不用真机）：https://openswiftuiproject.github.io/FoloToy-Passport-Simulator

本地逻辑自测（零依赖，随时可跑）：
```bash
python tools/verify_logic.py      # 7 项逻辑测试，全过即可放心
```

---

## 已知待办（不影响上传，按需做）
- ~~中文显示~~ ✅ **已完成**：屏上文案已全部汉化，字库走自算子集（见上文「屏上中文是怎么实现的」）。
- **音效颗粒**：现在是代码生成的正弦波「叮/嗒」，够用；要更真实可换成 PCM 素材
  （注意无 PSRAM，素材要走 SPIFFS/分区，别堆进代码）。
- **上架后归档**（可选）：按 `skills/plays-archive/` 把玩法文本摘要 PR 到上游
  `FoloToy/ai-passport` 的 `docs/reference/<用户名>/<app>/`。

## 文件地图
```
main/fishing_logic.{h,c}   纯逻辑层（状态机+计分，可单测，零硬件依赖）
main/fishing_audio.{h,c}   音效（代码生成 PCM，走官方 BSP）
main/fishing.c             应用层：LVGL 画面 + 三键(回调/队列) + 电量 + NVS 最高分
main/main.c                入口：BSP 硬件初始化顺序 → fishing_app_start()
main/CMakeLists.txt        只编 fishing 文件 + 注册中文字库源文件
assets/fonts/fishing_cjk_16.c   屏上中文字库（lv_font_conv 生成，字符自动从 fishing.c 抓）
assets/fonts/README.md          字体来源 / 授权(OFL 1.1) / 重新生成步骤
assets/cover/fishing-cover.png  社区上传用封面图（已去 AI 生成水印）
tools/gen_font.py          重新生成中文字库
tools/check_cjk_coverage.py 端到端缺字校验（源码中文 vs 字库字形）
tools/clean_cover.py       给封面去生成水印
tools/verify_logic.py      逻辑即时校验（零依赖）
tests/fishing_logic_test.c C 单测（装 gcc/ESP-IDF 后跑权威版）
上架材料清单.html           上传前逐字段核对清单（浏览器打开）
.github/workflows/firmware-checks.yml   云端自动编译（push main 触发）
```
