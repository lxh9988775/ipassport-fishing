idf_component_register(
    SRCS "main.c"
         "fishing_audio.c"
         "pet_logic.c"
         "pet.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash lvgl freertos
)

# 中文子集字体：由 lv_font_conv 生成（源字体思源黑体 CN / SIL OFL 1.1）。
# 只打包屏上会用到的字，≈90 KB。改文案后跑 `python tools/gen_font.py` 重新生成。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/fishing_cjk_16.c"
)

# 像素 sprite 资产（由 tools/gen_sprites.py / gen_house_bg.py / gen_costumes.py /
# gen_game_sprites.py 自动生成，勿手工编辑）。宠物固件只打包兔子精灵 + 房间背景 +
# 换装装扮 + 小游戏道具；钓鱼的鱼/道具/场景已按用户要求移出构建。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/sprites/pet_sprites.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/sprites/pet_room.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/sprites/pet_costumes.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/sprites/pet_game_sprites.c"
)
