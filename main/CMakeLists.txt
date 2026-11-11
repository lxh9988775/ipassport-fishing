idf_component_register(
    SRCS "ember_main.c"
         "ember.c"
         "ember_color.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash lvgl freertos esp_timer
)

# lv_font_conv 生成的字体用 #ifdef LV_LVGL_H_INCLUDE_SIMPLE 选择 #include "lvgl.h"，
# 本工程 lvgl 头在 include 根目录，故定义该宏（字体重生后也不报错）。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# 《余烬》中文子集字体：由 lv_font_conv 从思源黑体生成（tools 下用 lv_font_conv 重生）。
# 只打包屏上会用到的字，≈137 KB。改文案后重跑 lv_font_conv 重新生成。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ember_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ember_digit_34.c"
)

# 《余烬》真实母版美术资源（tools/gen_ember_assets.py 生成，全 Flash 常量图）：
#   静态夜森林 RGB565（永不 recolor）+ 6 档火色精灵 ARGB8888 + 6 张全图余烬卡 RGB565。
# 旧的 ember_bg.c（程序化夜景 ARGB8888 且字节序写反）已停用，不再编译。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/ember/ember_assets.c"
)
