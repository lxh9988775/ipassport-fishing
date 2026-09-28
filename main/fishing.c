/*
 * fishing.c - 钓鱼小游戏「竿影浮标」应用层（AI Passport / ESP32-C3, LVGL v9）
 *
 * 职责边界（遵循官方 BSP/应用分层）：
 *   - 游戏"规则与计分"全部在 fishing_logic.c（纯逻辑，可单测）
 *   - 本文件只负责：LVGL 画面、三键交互、音效触发、电量显示、NVS 最高分
 *
 * 已对照官方 BSP 头文件（components/bsp/include/）校准：
 *   bsp_display.h : bsp_display_init / bsp_display_backlight / bsp_lvgl_init /
 *                   bsp_lvgl_lock(timeout_ms)->bool / bsp_lvgl_unlock
 *   bsp_button.h  : bsp_button_init(cb,user) + 回调事件 BSP_BTN_CLICK / BSP_BTN_LONG
 *   bsp_battery.h : bsp_battery_soc(void)->int (0..100, 失败 -1)
 *   bsp_audio.h   : bsp_audio_set_format / bsp_audio_set_volume / bsp_audio_write
 *   LVGL v9       : lv_screen_active()（不是 v8 的 lv_scr_act）
 *
 * 入口见 main.c：main.c 负责 BSP 硬件初始化，本文件 fishing_app_start()
 * 在 BSP 就绪后被调用。
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp_display.h"   /* 显示初始化 + LVGL 接入（bsp_lvgl_init/lock/unlock） */
#include "bsp_button.h"    /* 三键回调 */
#include "bsp_battery.h"   /* 电量 */
#include "bsp_audio.h"     /* 音频播放 */
#include "fishing_logic.h"
#include "fishing_audio.h"

/* ===================== 单调时钟（毫秒） ===================== */
/* 用 FreeRTOS 节拍计数换算毫秒（sdkconfig 里 FREERTOS_HZ=1000，portTICK_PERIOD_MS=1），
 * 不依赖 LVGL 版本里的 tick API 名称，也不需要额外组件。 */
static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ===================== NVS（最高分断电不丢） ===================== */
#define NVS_NS "fishing"
#define NVS_KEY_HIGH "high"

static int nvs_load_high(void) {
    nvs_handle_t h;
    int val = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY_HIGH, &val);
        nvs_close(h);
    }
    return val;
}
static void nvs_save_high(int v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_HIGH, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ===================== UI 对象 ===================== */
static lv_obj_t *g_screen    = NULL;
static lv_obj_t *g_sky       = NULL;
static lv_obj_t *g_water     = NULL;
static lv_obj_t *g_floatline = NULL;  /* 鱼线 */
static lv_obj_t *g_floatbob  = NULL;  /* 浮标 */
static lv_obj_t *g_fish      = NULL;  /* 水下鱼影 */
static lv_obj_t *g_lbl_score   = NULL;
static lv_obj_t *g_lbl_high    = NULL;
static lv_obj_t *g_lbl_status  = NULL;
static lv_obj_t *g_lbl_batt    = NULL;
static lv_obj_t *g_lbl_menu    = NULL; /* 菜单/选择提示 */

/* ===================== 按键事件队列 ===================== */
/* 官方按键回调运行在共享 esp_timer 任务，只能入队或做有界操作，
 * 不能碰 LVGL / 重活。故回调只入队，由游戏任务(game_task)消费。 */
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;
static QueueHandle_t s_btn_q = NULL;

/* 菜单选中项：0=编辑饵料 1=编辑钓点 */
static int g_menu_sel = 0;

/* 电量（节流读取，避免每帧都刷 I2C） */
static int g_batt_soc = -1;
static int g_last_batt_ms = 0;

/* ===================== UI 构造 ===================== */
static void build_ui(void) {
    g_screen = lv_screen_active();
    lv_obj_set_style_bg_color(g_screen, lv_color_make(135, 206, 235), 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);

    /* 天空（上半） */
    g_sky = lv_obj_create(g_screen);
    lv_obj_set_size(g_sky, 240, 160);
    lv_obj_set_pos(g_sky, 0, 0);
    lv_obj_set_style_bg_color(g_sky, lv_color_make(135, 206, 235), 0);
    lv_obj_set_style_bg_opa(g_sky, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_sky, LV_OBJ_FLAG_SCROLLABLE);

    /* 水面（下半） */
    g_water = lv_obj_create(g_screen);
    lv_obj_set_size(g_water, 240, 160);
    lv_obj_set_pos(g_water, 0, 160);
    lv_obj_set_style_bg_color(g_water, lv_color_make(20, 60, 120), 0);
    lv_obj_set_style_bg_opa(g_water, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_water, LV_OBJ_FLAG_SCROLLABLE);

    /* 鱼线（一条竖线，从顶部到浮标） */
    static lv_point_t line_pts[] = {{120, 10}, {120, 175}};
    g_floatline = lv_line_create(g_screen);
    lv_line_set_points(g_floatline, line_pts, 2);
    lv_obj_set_style_line_width(g_floatline, 2, 0);
    lv_obj_set_style_line_color(g_floatline, lv_color_make(255, 255, 255), 0);

    /* 浮标（小圆） */
    g_floatbob = lv_obj_create(g_screen);
    lv_obj_set_size(g_floatbob, 14, 14);
    lv_obj_set_pos(g_floatbob, 113, 168);
    lv_obj_set_style_bg_color(g_floatbob, lv_color_make(255, 80, 80), 0);
    lv_obj_set_style_bg_opa(g_floatbob, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_floatbob, LV_OBJ_FLAG_SCROLLABLE);

    /* 水下鱼影（默认隐藏） */
    g_fish = lv_obj_create(g_screen);
    lv_obj_set_size(g_fish, 40, 16);
    lv_obj_set_pos(g_fish, 40, 210);
    lv_obj_set_style_bg_color(g_fish, lv_color_make(200, 200, 120), 0);
    lv_obj_set_style_bg_opa(g_fish, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_fish, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_fish, LV_OBJ_FLAG_HIDDEN);

    /* HUD 文本 */
    g_lbl_score = lv_label_create(g_screen);
    lv_label_set_text(g_lbl_score, "SCORE 0");
    lv_obj_set_pos(g_lbl_score, 4, 4);

    g_lbl_high = lv_label_create(g_screen);
    lv_label_set_text(g_lbl_high, "BEST 0");
    lv_obj_set_pos(g_lbl_high, 4, 20);

    g_lbl_batt = lv_label_create(g_screen);
    lv_label_set_text(g_lbl_batt, "BAT --%");
    lv_obj_set_pos(g_lbl_batt, 160, 4);

    g_lbl_status = lv_label_create(g_screen);
    lv_label_set_text(g_lbl_status, "OK=Cast  LongOK=Menu");
    lv_obj_set_pos(g_lbl_status, 4, 300);

    g_lbl_menu = lv_label_create(g_screen);
    lv_obj_add_flag(g_lbl_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(g_lbl_menu, 16, 120);
}

/* ===================== UI 刷新 ===================== */
static void refresh_ui(void) {
    fishing_status_t st;
    fishing_get_status(&st);

    char buf[48];
    snprintf(buf, sizeof(buf), "SCORE %d", st.score);
    lv_label_set_text(g_lbl_score, buf);
    snprintf(buf, sizeof(buf), "BEST %d", st.high_score);
    lv_label_set_text(g_lbl_high, buf);

    /* 电量 */
    if (g_batt_soc >= 0) {
        snprintf(buf, sizeof(buf), "BAT %d%%", g_batt_soc);
        lv_label_set_text(g_lbl_batt, buf);
    }

    /* 浮标动画：咬钩时下沉 + 鱼影出现 */
    if (st.state == STATE_BITE) {
        lv_obj_set_pos(g_floatbob, 113, 188);
        lv_obj_clear_flag(g_fish, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(g_floatbob, 113, 168);
        lv_obj_add_flag(g_fish, LV_OBJ_FLAG_HIDDEN);
    }

    /* 状态文本（LVGL 默认字体无中文字形，屏上统一用英文，避免方块） */
    const char *hint = "OK=Cast  LongOK=Menu";
    switch (st.state) {
        case STATE_IDLE:    hint = "OK=Cast  LongOK=Menu"; break;
        case STATE_WAITING: hint = "Waiting for bite..."; break;
        case STATE_BITE:    hint = "BITE! Press OK!"; break;
        case STATE_CATCH:   hint = "Caught! +score"; break;
        case STATE_MISS:    hint = "Missed! try again"; break;
        case STATE_MENU:    hint = "Menu: UP/DN tune, OK switch, LongOK ok"; break;
        default: break;
    }
    lv_label_set_text(g_lbl_status, hint);

    /* 菜单内容 */
    if (st.state == STATE_MENU) {
        const char *baits[] = {"WORM", "BREAD", "LURE"};
        const char *spots[] = {"POND", "RIVER", "SEA"};
        snprintf(buf, sizeof(buf),
                 "BAIT[%s] SPOT[%s]\nEdit: %s\nUP/DN tune LongOK ok",
                 baits[st.bait], spots[st.spot],
                 g_menu_sel == 0 ? "BAIT" : "SPOT");
        lv_label_set_text(g_lbl_menu, buf);
        lv_obj_clear_flag(g_lbl_menu, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== 按键动作处理 ===================== */
static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    fishing_status_t st;
    fishing_get_status(&st);

    /* 长按 OK：菜单进/出（全局统一） */
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (st.state == STATE_IDLE) {
            fishing_enter_menu();
            fishing_audio_play(SFX_CLICK);
        } else if (st.state == STATE_MENU) {
            fishing_exit_menu();
            fishing_audio_play(SFX_CLICK);
        }
        return;
    }

    /* 菜单内：只处理单击（PRESS 忽略，避免与 LONG 冲突） */
    if (st.state == STATE_MENU) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_OK) {
            g_menu_sel = (g_menu_sel + 1) % 2;   /* 在 饵料/钓点 间切换要编辑的项 */
            fishing_audio_play(SFX_CLICK);
        } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            if (g_menu_sel == 0)
                fishing_set_bait((bait_t)((st.bait + (btn == BSP_BTN_DOWN ? 1 : BAIT_COUNT - 1)) % BAIT_COUNT));
            else
                fishing_set_spot((spot_t)((st.spot + (btn == BSP_BTN_DOWN ? 1 : SPOT_COUNT - 1)) % SPOT_COUNT));
            fishing_audio_play(SFX_CLICK);
        }
        return;
    }

    /* 非菜单：用 PRESS（按下瞬间）即时响应，适合游戏 */
    if (ev == BSP_BTN_PRESS && btn == BSP_BTN_OK) {
        if (st.state == STATE_IDLE) {
            fishing_cast();
            fishing_audio_play(SFX_CLICK);
        } else if (st.state == STATE_BITE) {
            const catch_result_t *r = fishing_strike();
            if (r) {
                fishing_audio_play(SFX_CATCH);
                if (fishing_get_high_score() > nvs_load_high())
                    nvs_save_high(fishing_get_high_score());
            }
        }
    }
}

/* ===================== 游戏主任务（独立任务，LVGL 加锁） ===================== */
static void game_task(void *arg) {
    (void)arg;
    for (;;) {
        /* 1) 消费按键队列 */
        btn_ev_t e;
        while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) {
            handle_btn(e.btn, e.ev);
        }

        /* 2) 推进逻辑层（单调毫秒时钟） */
        int now = (int)now_ms();
        fishing_event_t evt = fishing_tick(now);
        if (evt == EVT_BITE_START)       fishing_audio_play(SFX_BITE);
        else if (evt == EVT_BITE_TIMEOUT) fishing_audio_play(SFX_MISS);

        /* 3) 电量节流（每 500ms 读一次 I2C） */
        if (now - g_last_batt_ms > 500) {
            g_batt_soc = bsp_battery_soc();
            g_last_batt_ms = now;
        }

        /* 4) 刷新画面（LVGL 必须加锁） */
        if (bsp_lvgl_lock(100)) {
            refresh_ui();
            bsp_lvgl_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ===================== 按键回调（esp_timer 任务，仅入队） ===================== */
void fishing_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_btn_q) return;
    const btn_ev_t e = { btn, ev };
    xQueueSend(s_btn_q, &e, 0);
}

/* ===================== 应用入口（main.c 在 BSP 就绪后调用） ===================== */
void fishing_app_start(void) {
    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));

    fishing_init(now_ms());
    fishing_set_high_score(nvs_load_high());
    fishing_audio_init();   /* 设置采样格式 + 音量（bsp_audio_init 已在 main 完成） */

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();
        bsp_lvgl_unlock();
    }

    xTaskCreate(game_task, "fishing", 4096, NULL, 5, NULL);
}
