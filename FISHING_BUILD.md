# 钓鱼玩法「竿影浮标」— 构建与上传手册

本目录是一个**完整可编译的 ESP-IDF 项目**：在官方 FoloToy/ai-passport 基础上，
把 `main/` 换成了钓鱼小游戏（逻辑层 + 应用层 + 音效），其余 BSP / 分区 / 构建配置
沿用官方。目标板：**AI Passport（ESP32-C3 / 8MB Flash / 240×320 屏）**。

游戏操作（三键，无触屏）：
- **OK 短按**：待机时抛竿；咬钩窗口内提竿（关键！）
- **OK 长按**：进入 / 退出「饵料 / 钓点」菜单
- **上 / 下**：菜单内切换要编辑的项（OK 在「饵料↔钓点」间切换），再上下调具体值
- 饵料：WORM / BREAD / LURE；钓点：POND / RIVER / SEA（深海分高但咬钩窗口更短）
- 最高分存 NVS，断电不丢

---

## 0. 本地先验证逻辑（不需任何环境）
```bash
cd AIpassport
python tools/verify_logic.py      # 7 项逻辑测试，全过即可放心
```

## 1. 安装 ESP-IDF 5.5.3（仅首次）
官方要求 **ESP-IDF 5.5.3**（见 `sdkconfig.defaults`）。Windows 用 Git Bash：
```bash
# 任选一路下载（国内用清华/中科大镜像更快）
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32c3
# 每次新开终端先激活：
. $HOME/esp/esp-idf/export.sh      # 实际路径以你安装位置为准
```

## 2. 构建 + 合并整镜像（上传必须用这个）
```bash
cd AIpassport
idf.py build                                  # 编译 app + bootloader + 分区表
idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin
# ↑ 这是从 0x0 开始的完整镜像，包含 bootloader+分区表+应用，≤8MB
```
> ⚠️ 别用 `idf.py build` 单独产出的 `build/FoloToy-AI-Passport.bin`（app-only，缺 bootloader/分区表），
> 社区校验会拒。上传只认 **`build/FoloToy-AI-Passport-full.bin`**。

## 3.（推荐）跑官方校验
```bash
./tools/validate.sh --firmware    # 需要 idf.py + cc + python3，Git Bash 里跑
```
它通过后会把 `build/FoloToy-AI-Passport-full.bin` 放到 `build/`，并校验分区偏移 / 不重叠 / ≤8MB。

## 4. 真机 / 模拟器先自测（可选，但建议）
- **模拟器**（不用真机）：https://openswiftuiproject.github.io/FoloToy-Passport-Simulator
  加载导出的 `full.bin` 即可看画面 + 按键 + 声音。
- **真机烧录**：`idf.py flash` 或
  `esptool.py write_flash 0x0 build/FoloToy-AI-Passport-full.bin`（Chrome/Edge 网页 USB 也行）。

## 5. 把代码推到公开 Git 仓库（上传的前置条件）
社区要求源码是一个**公开可达的 HTTPS Git 页面**（GitHub / Gitee / GitLab / Codeberg）。
```bash
cd AIpassport
git init && git add -A && git commit -m "feat: fishing game for AI Passport"
# 在 GitHub 建一个公开仓库，然后：
git remote add origin https://github.com/<你的用户名>/ai-passport-fishing.git
git push -u origin main
```
记下这个仓库的 HTTPS 页面地址（下一步要填）。

## 6. 上传到社区（官网发布技能）
按官方 `docs/development/release/publish-to-community.md`，上传靠官网的**发布技能**，
不是手动拖文件。流程（在你的 AI 助手 / 官网里）：
1. 安装发布技能：`https://ai-passport.folotoy.cn/skills/folotoy-ai-passport-publisher.zip`
2. 技能会引导你准备：
   - **固件**：`build/FoloToy-AI-Passport-full.bin`（已校验）
   - **封面图**：一张代表性 JPEG/PNG/WebP（≤10MiB）
   - **双语标题 + 简介**（中英文）
   - **Git 源码地址**：第 5 步的公开仓库 HTTPS 页
3. 在官网注册 / 登录**授权**，预览每个字段并确认后再上传。
   （助手不碰你的密码；凭证由你在官网自己处理。）

---

## 已知待办（不影响上传，按需做）
- **中文显示**：当前屏上文字是英文，因为 LVGL 默认字体无 CJK 字形。要中文显示，
  按 `docs/engineering/lvgl-chinese-fonts.zh_CN.md` 接入 CJK 字库（可只子集化用到的字），
  然后改 `main/fishing.c` 里的显示字符串即可。
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
main/CMakeLists.txt        只编 fishing 文件
tools/verify_logic.py      逻辑即时校验（零依赖）
tests/fishing_logic_test.c C 单测（装 gcc/ESP-IDF 后跑权威版）
```
