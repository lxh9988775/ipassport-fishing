idf_component_register(
    SRCS "main.c"
         "fishing_logic.c"
         "fishing_audio.c"
         "fishing.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash lvgl freertos
)
