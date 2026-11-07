idf_component_register(
    SRCS "ember_main.c"
         "ember.c"
         "ember_color.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash lvgl freertos
)

# 《余烬》中文子集字体：由 lv_font_conv 从思源黑体生成（tools 下用 lv_font_conv 重生）。
# 只打包屏上会用到的字，≈137 KB。改文案后重跑 lv_font_conv 重新生成。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ember_cjk_16.c"
)

# 静态夜森林背景（tools/gen_ember_bg.py 生成的 ARGB8888 常量图，放 Flash 不占 LV_MEM）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/ember/ember_bg.c"
)
