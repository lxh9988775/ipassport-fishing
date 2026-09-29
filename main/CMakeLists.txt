idf_component_register(
    SRCS "main.c"
         "fishing_logic.c"
         "fishing_audio.c"
         "fishing.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash lvgl freertos
)

# 中文子集字体：由 lv_font_conv 生成（源字体思源黑体 CN / SIL OFL 1.1）。
# 只打包屏上会用到的字，≈90 KB。改文案后跑 `python tools/gen_font.py` 重新生成。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/fishing_cjk_16.c"
)

# 像素 sprite 资产（由 tools/gen_sprites.py 自动生成，勿手工编辑）。
# 鱼(48x32 ARGB8888) + 未捕剪影 + 饵/竿/道具/图标 + 场景背景(RGB565)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/sprites/fish_sprites.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/sprites/prop_sprites.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/sprites/bg_sprites.c"
)
