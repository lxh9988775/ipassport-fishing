idf_component_register(
    SRCS "main.c"
         "mage_state.c"
         "mage_data.c"
         "mage.c"
         "mage_draw.c"
    INCLUDE_DIRS "." ".." "../assets"
    REQUIRES bsp nvs_flash lvgl freertos
)

# 中文子集字体：由 lv_font_conv 生成（源字体思源黑体 CN / SIL OFL 1.1）。
# 只打包屏上会用到的字，改文案后跑 `python tools/gen_font_mage.py` 重新生成。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/mage_cjk_16.c"
)

# 贴图：与 HTML 原版同一批 PNG，由 `python tools/gen_mage_sprites.py` 生成 RGB565 数组。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/mage_sprites.c"
)

# 【重要】串口截屏模块 fap_screenshot.c 已从构建中移除。
# 原因：它的 150KB 全屏静态缓冲 + 8KB 任务栈把 ESP32-C3 的可用 RAM
# 从 147KB 压到 84KB，导致 LVGL 渲染 240x320 时分配失败，
# 表现为固定地址 MEPC=0x420069f2 的 Load access fault 崩溃重启循环。
# 抓真机截图改走 simulator / 电脑端渲染，不再在设备上做全屏快照。
