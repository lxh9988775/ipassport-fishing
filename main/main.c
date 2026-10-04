/*
 * main/main.c —— AI Passport 电子宠物「口袋兔」单独固件
 *
 * 职责：只做"硬件按正确顺序启动"，然后直接进入电子宠物应用。
 * 顺序很重要：i2c → display → lvgl → backlight → nvs → audio → battery → button → pet。
 *
 * 注：按用户要求，本固件不再包含钓鱼玩法与双应用启动器（2026-10-04）。
 * 开机即宠物，无应用选择页。
 */
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pet_logic.h"       /* pet_app_start() 声明 */

static const char *TAG = "app_pet";

/* 按键回调（定义于 pet.c），仅入队，由宠物任务消费 */
void pet_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport —— 口袋兔电子宠物");

    /* I2C 必须先初始化（ES8311 音频 + CW2017 电量共用总线，幂等）*/
    bsp_i2c_init();

    /* 显示 + LVGL（失败则无法继续，打印引脚后退出）*/
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(80);

    /* NVS（宠物存档持久化）*/
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化返回 %s（首次启动常见，可忽略）", esp_err_to_name(nvs_err));
    }

    /* 音频 / 电量：单项失败不阻塞，UI 上电量会显示 --% */
    if (bsp_audio_init() != ESP_OK)  ESP_LOGW(TAG, "音频初始化失败");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    /* 按键直接交给宠物应用 */
    if (bsp_button_init(pet_on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }

    /* 直接进入电子宠物（pet_app_start 内部自建任务）*/
    pet_app_start();
}
