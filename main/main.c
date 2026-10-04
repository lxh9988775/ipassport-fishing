/*
 * main/main.c —— AI Passport 应用启动器（钓鱼 / 电子宠物）
 *
 * 职责：只做"硬件按正确顺序启动"，然后显示应用选择页，把控制权交给选中的应用。
 * 顺序很重要：i2c → display → lvgl → backlight → nvs → audio → battery → button → launcher。
 *
 * 为什么要 launcher：设备上现在有两个完整应用（钓鱼 v2、电子宠物），三键机身
 * 没有全局返回键，所以切换应用 = 重启后再选（对玩具场景足够，也最稳）。
 *
 * 按键回调只入队/置标志，不碰 LVGL —— 切换动作在 launcher_task 里做，
 * 避免在 esp_timer 回调上下文里重建按键资源。
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

#include "fishing_logic.h"   /* fishing_app_start() 声明 */
#include "fishing_audio.h"
#include "pet_logic.h"       /* pet_app_start() 声明 */

static const char *TAG = "app_launcher";

/* 按键回调（分别定义于 fishing.c / pet.c），仅入队，由各自游戏任务消费 */
void fishing_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);
void pet_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

/* ===================== 启动器 UI ===================== */
#define LAUNCH_OPT_COUNT 2

static lv_obj_t *g_root     = NULL;
static lv_obj_t *g_opt[LAUNCH_OPT_COUNT] = {0};
static lv_obj_t *g_selbar   = NULL;

static volatile int s_sel    = 0;    /* 0=钓鱼 1=电子宠物 */
static volatile int s_launch = -1;   /* >=0 = 请求启动该应用 */

/* 中文字体：与两个应用共用同一子集字库（tools/gen_font.py 自动生成） */
LV_FONT_DECLARE(fishing_cjk_16);

static void use_cjk(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &fishing_cjk_16, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, int x, int y, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    use_cjk(l);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static void launcher_build_ui(void) {
    g_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_root, 240, 320);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_make(20, 16, 20), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(g_root, "AI Passport", 0, 0, lv_color_make(230, 220, 225));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);

    static const char *NAMES[LAUNCH_OPT_COUNT] = { "钓鱼", "电子宠物" };
    for (int i = 0; i < LAUNCH_OPT_COUNT; ++i) {
        g_opt[i] = make_label(g_root, NAMES[i], 0, 0, lv_color_white());
        lv_obj_set_width(g_opt[i], 140);
        lv_obj_set_style_text_align(g_opt[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(g_opt[i], LV_ALIGN_TOP_MID, 0, 130 + i * 56);
    }

    /* 选中胶囊（沉底，不盖文字） */
    g_selbar = lv_obj_create(g_root);
    lv_obj_set_size(g_selbar, 156, 32);
    lv_obj_set_pos(g_selbar, 42, 124);
    lv_obj_set_style_bg_color(g_selbar, lv_color_make(153, 0, 58), 0);   /* 锐胜红 */
    lv_obj_set_style_bg_opa(g_selbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_selbar, 0, 0);
    lv_obj_set_style_radius(g_selbar, 16, 0);
    lv_obj_set_style_pad_all(g_selbar, 0, 0);
    lv_obj_clear_flag(g_selbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_to_index(g_selbar, 0);

    lv_obj_t *hint = make_label(g_root, "上下选 · OK 进", 0, 0, lv_color_make(150, 140, 145));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

static void launcher_refresh_sel(void) {
    if (!g_selbar) return;
    lv_obj_set_pos(g_selbar, 42, 124 + s_sel * 56);
}

/* ===================== 启动器任务 ===================== */
static void launcher_task(void *arg) {
    (void)arg;
    int last_sel = -1;
    for (;;) {
        if (s_launch >= 0) {
            const int which = s_launch;
            /* 1) 拆掉启动器界面 */
            if (bsp_lvgl_lock(1000)) {
                if (g_root) { lv_obj_del(g_root); g_root = NULL; }
                bsp_lvgl_unlock();
            }
            /* 2) 把按键回调切给选中的应用 */
            bsp_button_init(which == 0 ? fishing_on_key : pet_on_key, NULL);
            /* 3) 启动应用（各自接管屏幕与任务） */
            if (which == 0) fishing_app_start();
            else            pet_app_start();
            vTaskDelete(NULL);   /* launcher 使命结束 */
            return;
        }
        if (s_sel != last_sel) {
            last_sel = s_sel;
            if (bsp_lvgl_lock(100)) {
                launcher_refresh_sel();
                bsp_lvgl_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ===================== 按键回调（仅置标志） ===================== */
static void launcher_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP)   s_sel = (s_sel + LAUNCH_OPT_COUNT - 1) % LAUNCH_OPT_COUNT;
    else if (btn == BSP_BTN_DOWN) s_sel = (s_sel + 1) % LAUNCH_OPT_COUNT;
    else if (btn == BSP_BTN_OK)   s_launch = s_sel;
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport —— 应用启动器");

    /* I2C 必须先初始化（ES8311 音频 + CW2017 电量共用总线，幂等）*/
    bsp_i2c_init();

    /* 显示 + LVGL（失败则无法继续，打印引脚后退出）*/
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(80);

    /* NVS（两个应用各自持久化）*/
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化返回 %s（首次启动常见，可忽略）", esp_err_to_name(nvs_err));
    }

    /* 音频 / 电量：单项失败不阻塞，UI 上电量会显示 --% */
    if (bsp_audio_init() != ESP_OK)  ESP_LOGW(TAG, "音频初始化失败");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计初始化失败");

    /* 启动器界面 + 按键 */
    if (bsp_lvgl_lock(1000)) {
        launcher_build_ui();
        launcher_refresh_sel();
        bsp_lvgl_unlock();
    }
    if (bsp_button_init(launcher_on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }
    xTaskCreate(launcher_task, "launcher", 3072, NULL, 5, NULL);
}
