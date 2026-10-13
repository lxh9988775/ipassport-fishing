idf_component_register(
    SRCS "main.c"
         "mage_state.c"
         "mage_data.c"
         "mage.c"
         "mage_draw.c"
         "fap_screenshot.c"
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
