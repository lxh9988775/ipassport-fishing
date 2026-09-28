/*
 * main/main.c —— 钓鱼玩法「竿影浮标」入口（替换官方 demo 入口）
 *
 * 职责：只做"硬件按正确顺序启动"，然后把控制权交给 fishing_app_start()。
 * 顺序很重要：i2c → display → lvgl → backlight → nvs → audio → battery → button → game。
 *
 * 编译前确认：本文件与 fishing_logic.c / fishing_audio.c / fishing.c 已加入
 * main/CMakeLists.txt 的 SRCS，并删除了原 demo_*.c / ui_pixel.*。
 */
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "fishing_logic.h"   /* fishing_app_start() 声明 */
#include "fishing_audio.h"

static const char *TAG = "fishing_main";

/* 按键回调（定义于 fishing.c），仅入队，由游戏任务消费 */
void fishing_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport —— 钓鱼玩法启动");

    /* I2C 必须先初始化（ES8311 音频 + CW2017 电量共用总线，幂等）*/
    bsp_i2c_init();

    /* 显示 + LVGL（失败则无法继续，打印引脚后退出）*/
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(80);

    /* NVS（最高分持久化）*/
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化返回 %s（首次启动常见，可忽略）", esp_err_to_name(nvs_err));
    }

    /* 音频 / 电量：单项失败不阻塞，UI 上电量会显示 --% */
    if (bsp_audio_init() != ESP_OK)  ESP_LOGW(TAG, "音频初始化失败");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    /* 按键：注册回调（回调只入队，不碰 LVGL）*/
    if (bsp_button_init(fishing_on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }

    /* 交给游戏应用层 */
    fishing_app_start();
}
