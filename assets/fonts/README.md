# assets/fonts — 屏上中文字体

## 目录内容
| 文件 | 说明 | 是否进版本库 |
| --- | --- | --- |
| `fishing_cjk_16.c` | 屏上中文的**子集字库**，编译进固件（≈88 KB） | ✅ 提交 |
| `SourceHanSansCN-Normal.otf` | 生成用的源字体（8.4 MB），脚本按此路径查找 | ❌ 忽略（见下） |

## 为什么不用 LVGL 自带字体
- `Montserrat 14/20`：只有拉丁字母，中文全是方块。
- LVGL 内置的 `lv_font_source_han_sans_sc_16_cjk`：**看着像能用，其实只是个 1187 字形的子集**。
  实测它缺失本游戏的核心用字：`钓 鱼 饵 蚯 蚓 咬 抛 竿 塘 钩 选 择 按 编 辑` → 屏上会显示方框。
  所以**没有采用内置方案**，改为自己生成精确子集。

## 源字体来源与授权
- 名称：**Source Han Sans CN Normal**（思源黑体 CN Normal）
- 版权：Adobe / Google，**SIL Open Font License 1.1**
- 授权要点：可自由使用、修改、嵌入、再分发（含商用）；不得单独售卖字体本身；
  再分发时须保留 OFL 声明。生成出的 `fishing_cjk_16.c` 属于"嵌入使用"，合规。
- 下载地址（CDN 镜像，避免访问 GitHub 受限）：
  ```
  https://cdn.jsdelivr.net/gh/adobe-fonts/source-han-sans@release/SubsetOTF/CN/SourceHanSansCN-Normal.otf
  ```
- SHA-256：
  ```
  9fc9ad5b64f086a2b342087865623923c216e346f879142a84df37c7ffd323c5
  ```

## 重新生成字库

```bash
# 1) 准备源字体（已存在则跳过）
curl -L -o assets/fonts/SourceHanSansCN-Normal.otf \
  https://cdn.jsdelivr.net/gh/adobe-fonts/source-han-sans@release/SubsetOTF/CN/SourceHanSansCN-Normal.otf

# 2) 安装转换工具（一次性；装在托管 node workspace，不用 -g）
cd "$NODE_WORKSPACE" && npm install lv_font_conv

# 3) 生成 + 校验
python tools/gen_font.py            # 生成 assets/fonts/fishing_cjk_16.c
python tools/check_cjk_coverage.py  # 端到端校验：源码里的中文，字库是否全覆盖
```

## 关键约定
- **字符集自动从 `main/fishing.c` 抓取**（所有字符串字面量里的非 ASCII 字符 + 可打印 ASCII 全量），
  不需要手工维护清单；改完屏上文案重跑 `gen_font.py` 即可。
- 生成的符号名固定为 `lv_font_fishing_cjk_16`，在 `main/fishing.c` 里用
  `LV_FONT_DECLARE(fishing_cjk_16)` 引用。
- **不要**在别处再 `#include` 这个 `.c`，也不要把同一个字体编两次
  （`main/CMakeLists.txt` 里已通过 `target_sources` 注册一次）。
- 字号 = 16 px、bpp = 4、未压缩（官方建议首次接入先不压缩）。
- 工具版本：`lv_font_conv 1.5.3`（换版本请重跑校验脚本，确认字形覆盖与度量无变化）。
