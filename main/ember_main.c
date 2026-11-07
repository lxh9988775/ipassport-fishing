/* ember_main.c — 《余烬》固件入口
 *
 * 职责：只做“硬件按正确顺序启动”，然后进入《余烬》应用。
 * 顺序（与口袋兔 main.c 一致，顺序很重要）：
 *   i2c → display → lvgl → backlight → nvs → audio → battery → button → ember。
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

#include "ember_color.h"   /* 仅拿类型，实际逻辑在 ember.c */

static const char *TAG = "app_ember";

/* 按键回调（定义于 ember.c），仅入队，由游戏任务消费 */
void ember_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);
void ember_app_start(void);

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport —— 余烬 (Embers)");

    /* I2C 必须先初始化（ES8311 音频 + CW2017 电量共用总线，幂等）*/
    bsp_i2c_init();

    /* 显示 + LVGL（失败则无法继续，打印后退出）*/
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(80);

    /* NVS（余烬存档持久化）*/
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化返回 %s（首次启动常见，可忽略）", esp_err_to_name(nvs_err));
    }

    /* 音频 / 电量：单项失败不阻塞 */
    if (bsp_audio_init() != ESP_OK)  ESP_LOGW(TAG, "音频初始化失败");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    /* 按键交给《余烬》应用 */
    if (bsp_button_init(ember_on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }

    /* 进入《余烬》 */
    ember_app_start();
}
