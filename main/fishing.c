/*
 * fishing.c - 钓鱼 v2「竿影浮标」应用层（AI Passport / ESP32-C3, LVGL v9）
 *
 * 职责边界（遵循官方 BSP/应用分层）：
 *   - 游戏"规则与计分"全部在 fishing_logic.c（纯逻辑，可单测）
 *   - 本文件只负责：LVGL 画面、三键交互、音效触发、电量显示、NVS 存档
 *
 * v2 交互（核心玩法：星露谷式收线）：
 *   IDLE    OK 单击=抛竿 / OK 长按=开菜单
 *   BITE    OK 单击=提竿（错过窗口就跑鱼）
 *   REELING 按住 OK=捕捉区上抬，松开=下落 —— 把鱼稳在区内涨进度
 *   CODEX   上下翻鱼种 / OK 单击=看详情 / OK 长按=返回
 *
 * 按键说明：BSP 不提供"松开"事件，这里用 PRESS(按下) 开始长按、CLICK(按下并抬起)
 * 结束长按，既拿到按下瞬间的低延迟，也能可靠知道手已松开。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_pins.h"     /* BSP_BTN_MV_TABLE：收线时直接读电压判"手指还在不在" */
#include "bsp_battery.h"
#include "bsp_audio.h"
#include "fishing_logic.h"
#include "fishing_audio.h"
#include "../assets/sprites/sprites.h"

/* ===================== 中文字体 ===================== */
/* 屏上文案用中文。LVGL 自带字体不含本游戏用字（实测「钓 鱼 饵 蚯 蚓 竿」等缺字），
 * 故用 lv_font_conv 生成的专用子集字体 assets/fonts/fishing_cjk_16.c。
 * 改文案后同步流程：改文案 -> 跑 tools/gen_font.py -> 跑 tools/check_cjk_coverage.py。 */
LV_FONT_DECLARE(fishing_cjk_16);

static void use_cjk(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &fishing_cjk_16, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ===================== 屏幕布局 ===================== */
#define SCR_W 240
#define SCR_H 320
#define HUD_H 22

/* 收线轨道像素区域 */
#define TRK_X 196
#define TRK_W 26
#define TRK_Y 46
#define TRK_H 224
/* 进度条像素区域 */
#define PRG_X 20
#define PRG_W 16
#define PRG_Y 46
#define PRG_H 224

/* ===================== 时钟 / NVS ===================== */
static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

#define NVS_NS   "fishing"
#define NVS_KEY  "save"     /* 版本化存档 blob，见 fishing_logic.c 的说明 */

/* 读存档：失败就当新号，不崩 */
static void nvs_load_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0) {
        uint8_t *buf = (uint8_t *)malloc(len);
        if (buf) {
            if (nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK) {
                fishing_save_apply(buf, (int)len);   /* 内含版本校验与迁移 */
            }
            free(buf);
        }
    }
    nvs_close(h);
}

static void nvs_save_now(void) {
    uint8_t buf[512];
    int n = fishing_save_serialize(buf, (int)sizeof(buf));
    if (n <= 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, buf, (size_t)n);
    nvs_commit(h);
    nvs_close(h);
}

/* ===================== sprite 查表 ===================== */
static const lv_image_dsc_t *const FISH_IMG[FISH_SPECIES_COUNT] = {
    &fish_baitiao, &fish_jiyu, &fish_luofei, &fish_lianyu, &fish_bianyu, &fish_caoyu,
    &fish_liyu, &fish_qingyu, &fish_makou, &fish_huangsang, &fish_qiaozui, &fish_luyu,
    &fish_guiyu, &fish_nianyu, &fish_junyu, &fish_ganyu, &fish_xiaohuang, &fish_daiyu,
    &fish_bayu, &fish_bimu, &fish_shiban, &fish_jinqiang, &fish_qiyu, &fish_xiaosha,
};
static const lv_image_dsc_t *const SIL_IMG[FISH_SPECIES_COUNT] = {
    &sil_baitiao, &sil_jiyu, &sil_luofei, &sil_lianyu, &sil_bianyu, &sil_caoyu,
    &sil_liyu, &sil_qingyu, &sil_makou, &sil_huangsang, &sil_qiaozui, &sil_luyu,
    &sil_guiyu, &sil_nianyu, &sil_junyu, &sil_ganyu, &sil_xiaohuang, &sil_daiyu,
    &sil_bayu, &sil_bimu, &sil_shiban, &sil_jinqiang, &sil_qiyu, &sil_xiaosha,
};
static const lv_image_dsc_t *const BG_IMG[FISH_SPOT_COUNT] = { &bg_pond, &bg_river, &bg_sea };

/* ===================== UI 对象 ===================== */
/*
 * 屏幕布局有两条硬约束，改坐标前务必先读：
 *   1) 屏只有 240px 宽，四角还有 30px 圆角遮罩（BSP_LVGL_SCREEN_RADIUS），
 *      被遮区域一律涂黑 —— 顶部 y=4 那一行实际只剩 x∈[16,223] 可见。
 *   2) 中文 16px 一个字，四个中文标签横排就是 128px 起，塞不下。
 * 所以顶栏只放三项："得分 / 最高 / 电池图形+百分比"，钓点名改挂场景面板右上角。
 * 改完必须跑 tools/check_ui_layout.py（出屏/被圆角切/互相压住 都会报出来）。
 */
static lv_obj_t *g_bg          = NULL;   /* 背景（按钓点切换） */
static lv_obj_t *g_lbl_score   = NULL;
static lv_obj_t *g_lbl_high    = NULL;
static lv_obj_t *g_lbl_batt    = NULL;   /* 电量百分比数字（单位由电池图形表达） */
static lv_obj_t *g_batt_body   = NULL;   /* 电池外壳 */
static lv_obj_t *g_batt_fill   = NULL;   /* 电池内部电量条 */
static lv_obj_t *g_batt_nub    = NULL;   /* 电池正极凸点 */
static lv_obj_t *g_lbl_spot    = NULL;   /* 钓点名（挂在场景面板右上角的胶囊里） */
static lv_obj_t *g_spot_cap    = NULL;   /* 钓点名胶囊底 */
static lv_obj_t *g_lbl_hint    = NULL;   /* 底部提示 */

/* 钓鱼场景 */
static lv_obj_t *g_scene       = NULL;
static lv_obj_t *g_floatbob    = NULL;
static lv_obj_t *g_fish_scene  = NULL;

/* 收线界面 */
static lv_obj_t *g_reel        = NULL;
static lv_obj_t *g_track       = NULL;
static lv_obj_t *g_zone        = NULL;
static lv_obj_t *g_zone_fill   = NULL;
static lv_obj_t *g_fishmark    = NULL;
static lv_obj_t *g_prg_fill    = NULL;
static lv_obj_t *g_lbl_reel    = NULL;

/* 上鱼结算 */
static lv_obj_t *g_result      = NULL;
static lv_obj_t *g_res_img     = NULL;
static lv_obj_t *g_res_name    = NULL;
static lv_obj_t *g_res_info    = NULL;

/* 菜单 */
static lv_obj_t *g_menu        = NULL;
static lv_obj_t *g_menu_rows[5];
static lv_obj_t *g_menu_selbar = NULL;
static int g_menu_idx = 0;
static int g_menu_edit = 0;   /* 是否进入编辑某项 */

/* 图鉴 */
static lv_obj_t *g_codex       = NULL;
static lv_obj_t *g_cdx_img     = NULL;
static lv_obj_t *g_cdx_name    = NULL;
static lv_obj_t *g_cdx_meta    = NULL;
static lv_obj_t *g_cdx_desc    = NULL;
static lv_obj_t *g_cdx_prog    = NULL;

/* ===================== 按键队列 ===================== */
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } btn_ev_t;
static QueueHandle_t s_btn_q = NULL;

static int g_batt_soc = -1;
static int g_last_batt_ms = 0;
static int g_last_save_ms = 0;

/* ===================== 工具 ===================== */
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, int x, int y, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    use_cjk(l);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/* ===================== 只在"真的变了"才写给 LVGL =====================
 * 主循环 20ms/帧，原来各 refresh_* 是无条件写的，代价很大：
 *   - lv_label_set_text() 每次都会重新排字形（16px 中文子集，最贵的一项）；
 *   - lv_image_set_src() 没有"src 相同就返回"的短路（LVGL 9.5 的
 *     lv_image_set_src 首尾各一次无条件 invalidate），240x320 背景图
 *     每帧就是 153,600 字节的重绘 —— 场景页因此一直在整屏重画。
 * 用控件现值比较而不是影子变量：g_lbl_score 会被场景/收线/结算三处写，
 * 影子变量容易失同步，跟 LVGL 自己的值比一定不会漏画。 */
static bool set_text_cached(lv_obj_t *lbl, const char *txt) {
    if (!lbl || !txt) return false;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, txt) == 0) return false;   /* 没变：一个 LVGL API 都不调 */
    lv_label_set_text(lbl, txt);
    return true;
}

/* 图片按【业务键】缓存（钓点/鱼种/是否已收录），不依赖 lv_img_get_src()。
 * 每个 key 配一个 static int 影子（-99 = 还没画过）。 */
static bool set_img_by_key(lv_obj_t *img, int *cache, int key, const lv_image_dsc_t *dsc) {
    if (!img || !dsc || !cache || *cache == key) return false;
    *cache = key;
    lv_img_set_src(img, dsc);
    return true;
}

/* ===================== UI 构造 ===================== */
static void build_ui(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 背景图（像素场景，随钓点切换） */
    g_bg = lv_img_create(scr);
    lv_img_set_src(g_bg, BG_IMG[0]);
    lv_obj_set_pos(g_bg, 0, 0);

    /* ---- 顶栏 HUD ----
     * 屏只有 240px 宽、左右还被圆角各吃掉约 16px，所以刻意只放三项：
     *   左「得分」 · 中「最高」 · 右「电池图形 + 百分比」
     * 电量不再写「电量」二字 —— 电池图形本身就是单位，省下的宽度留给分数。
     * 三项最坏（得分/最高各四位、电量 100）合计约 181px，落在可见区 x∈[17,223] 内。 */
    lv_obj_t *hud = lv_obj_create(scr);
    lv_obj_set_size(hud, SCR_W, HUD_H + 2);   /* 文字底边在 y=24，底板盖到 24 */
    lv_obj_set_pos(hud, 0, 0);
    lv_obj_set_style_bg_color(hud, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hud, 150, 0);
    lv_obj_set_style_border_width(hud, 0, 0);
    lv_obj_set_style_radius(hud, 0, 0);
    lv_obj_set_style_pad_all(hud, 0, 0);
    lv_obj_clear_flag(hud, LV_OBJ_FLAG_SCROLLABLE);
    (void)hud;

    g_lbl_score = make_label(scr, "得分 0", 17, 4, lv_color_white());
    g_lbl_high  = make_label(scr, "最高 0", 96, 4, lv_color_white());

    /* 电池：壳 + 内部电量条 + 正极凸点。
     * 不用 LV_SYMBOL_BATTERY_* —— 本应用把文字字体换成了 fishing_cjk_16（.fallback
     * 为 NULL），内置符号字形取不到，画出来是空的。 */
    g_batt_body = lv_obj_create(scr);
    lv_obj_set_size(g_batt_body, 12, 8);
    lv_obj_set_pos(g_batt_body, 176, 9);
    lv_obj_set_style_bg_opa(g_batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(g_batt_body, lv_color_white(), 0);
    lv_obj_set_style_border_width(g_batt_body, 1, 0);
    lv_obj_set_style_radius(g_batt_body, 1, 0);
    lv_obj_set_style_pad_all(g_batt_body, 0, 0);
    lv_obj_clear_flag(g_batt_body, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_fill = lv_obj_create(g_batt_body);
    lv_obj_set_size(g_batt_fill, 10, 6);
    lv_obj_set_pos(g_batt_fill, 0, 0);        /* 相对内容区，正好贴在边框内侧 */
    lv_obj_set_style_bg_color(g_batt_fill, lv_color_make(90, 220, 120), 0);
    lv_obj_set_style_bg_opa(g_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_fill, 0, 0);
    lv_obj_set_style_radius(g_batt_fill, 0, 0);
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    g_batt_nub = lv_obj_create(scr);
    lv_obj_set_size(g_batt_nub, 2, 4);
    lv_obj_set_pos(g_batt_nub, 188, 11);
    lv_obj_set_style_bg_color(g_batt_nub, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_batt_nub, 0, 0);
    lv_obj_set_style_radius(g_batt_nub, 0, 0);
    lv_obj_clear_flag(g_batt_nub, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_batt = make_label(scr, "--", 193, 4, lv_color_white());

    /* 底部提示：文案长短不一，交给 BOTTOM_MID 自动居中，别手算 x 再撞上圆角 */
    g_lbl_hint = make_label(scr, "", 0, 0, lv_color_white());
    lv_obj_set_style_bg_color(g_lbl_hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_lbl_hint, 170, 0);
    lv_obj_set_style_pad_all(g_lbl_hint, 1, 0);
    lv_obj_align(g_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -5);

    /* ---------- 钓鱼场景 ---------- */
    g_scene = lv_obj_create(scr);
    lv_obj_set_size(g_scene, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_scene, 0, HUD_H);
    lv_obj_set_style_bg_opa(g_scene, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_scene, 0, 0);
    lv_obj_clear_flag(g_scene, LV_OBJ_FLAG_SCROLLABLE);

    /* 钓点名：挂在场景面板右上角的小胶囊里。
     * 放在 g_scene 内部 → 切到菜单/图鉴时随场景一起隐藏，show_only() 不用改。
     * 绝对位置 (172,28)~(229,47)，该高度上右侧圆角只吃到 x=237，不会切到文字。 */
    g_spot_cap = lv_obj_create(g_scene);
    lv_obj_set_size(g_spot_cap, 58, 20);
    lv_obj_set_pos(g_spot_cap, 172, 6);
    lv_obj_set_style_bg_color(g_spot_cap, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_spot_cap, 140, 0);
    lv_obj_set_style_border_width(g_spot_cap, 0, 0);
    lv_obj_set_style_radius(g_spot_cap, 10, 0);
    lv_obj_set_style_pad_all(g_spot_cap, 0, 0);
    lv_obj_clear_flag(g_spot_cap, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_spot = make_label(g_spot_cap, "静水塘", 0, 0, lv_color_white());
    lv_obj_center(g_lbl_spot);

    g_floatbob = lv_img_create(g_scene);
    lv_img_set_src(g_floatbob, &prop_float);
    lv_obj_set_pos(g_floatbob, 110, 96);

    g_fish_scene = lv_img_create(g_scene);
    lv_img_set_src(g_fish_scene, FISH_IMG[0]);
    lv_obj_set_pos(g_fish_scene, 20, 170);
    lv_obj_add_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);

    /* ---------- 收线界面 ---------- */
    g_reel = lv_obj_create(scr);
    lv_obj_set_size(g_reel, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_reel, 0, HUD_H);
    lv_obj_set_style_bg_color(g_reel, lv_color_make(10, 30, 60), 0);
    lv_obj_set_style_bg_opa(g_reel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_reel, 0, 0);
    lv_obj_clear_flag(g_reel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_reel, LV_OBJ_FLAG_HIDDEN);

    /* 进度条（左） */
    lv_obj_t *prg_bg = lv_obj_create(g_reel);
    lv_obj_set_size(prg_bg, PRG_W, PRG_H);
    lv_obj_set_pos(prg_bg, PRG_X, PRG_Y);
    lv_obj_set_style_bg_color(prg_bg, lv_color_make(40, 40, 60), 0);
    lv_obj_set_style_bg_opa(prg_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(prg_bg, 0, 0);
    lv_obj_set_style_radius(prg_bg, 0, 0);
    lv_obj_clear_flag(prg_bg, LV_OBJ_FLAG_SCROLLABLE);
    g_prg_fill = lv_obj_create(g_reel);
    lv_obj_set_size(g_prg_fill, PRG_W, 0);
    lv_obj_set_pos(g_prg_fill, PRG_X, PRG_Y + PRG_H);
    lv_obj_set_style_bg_color(g_prg_fill, lv_color_make(80, 220, 120), 0);
    lv_obj_set_style_bg_opa(g_prg_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_prg_fill, 0, 0);
    lv_obj_set_style_radius(g_prg_fill, 0, 0);
    lv_obj_clear_flag(g_prg_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* 收线轨道（右） */
    g_track = lv_obj_create(g_reel);
    lv_obj_set_size(g_track, TRK_W, TRK_H);
    lv_obj_set_pos(g_track, TRK_X, TRK_Y);
    lv_obj_set_style_bg_color(g_track, lv_color_make(30, 55, 90), 0);
    lv_obj_set_style_bg_opa(g_track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_track, lv_color_make(180, 210, 255), 0);
    lv_obj_set_style_border_width(g_track, 2, 0);
    lv_obj_set_style_radius(g_track, 3, 0);
    lv_obj_clear_flag(g_track, LV_OBJ_FLAG_SCROLLABLE);

    g_zone = lv_obj_create(g_reel);
    lv_obj_set_size(g_zone, TRK_W - 4, 40);
    lv_obj_set_pos(g_zone, TRK_X + 2, TRK_Y + 2);
    lv_obj_set_style_bg_color(g_zone, lv_color_make(90, 230, 140), 0);
    lv_obj_set_style_bg_opa(g_zone, LV_OPA_50, 0);
    lv_obj_set_style_border_width(g_zone, 0, 0);
    lv_obj_set_style_radius(g_zone, 0, 0);
    lv_obj_clear_flag(g_zone, LV_OBJ_FLAG_SCROLLABLE);

    g_fishmark = lv_obj_create(g_reel);
    lv_obj_set_size(g_fishmark, TRK_W - 6, 10);
    lv_obj_set_pos(g_fishmark, TRK_X + 3, TRK_Y + 100);
    lv_obj_set_style_bg_color(g_fishmark, lv_color_make(255, 200, 70), 0);
    lv_obj_set_style_bg_opa(g_fishmark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_fishmark, 0, 0);
    lv_obj_clear_flag(g_fishmark, LV_OBJ_FLAG_SCROLLABLE);

    g_lbl_reel = make_label(g_reel, "收线中", 40, TRK_Y, lv_color_white());

    /* ---------- 结算 ---------- */
    g_result = lv_obj_create(scr);
    lv_obj_set_size(g_result, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_result, 0, HUD_H);
    lv_obj_set_style_bg_color(g_result, lv_color_make(8, 24, 48), 0);
    lv_obj_set_style_bg_opa(g_result, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_result, 0, 0);
    lv_obj_clear_flag(g_result, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_result, LV_OBJ_FLAG_HIDDEN);

    g_res_img = lv_img_create(g_result);
    lv_obj_set_pos(g_res_img, 84, 40);
    g_res_name = make_label(g_result, "", 8, 84, lv_color_white());
    g_res_info = make_label(g_result, "", 8, 106, lv_color_white());
    lv_obj_set_width(g_res_info, SCR_W - 16);
    lv_obj_set_style_text_line_space(g_res_info, 4, 0);
    lv_label_set_long_mode(g_res_info, LV_LABEL_LONG_WRAP);

    /* ---------- 菜单 ---------- */
    g_menu = lv_obj_create(scr);
    lv_obj_set_size(g_menu, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_menu, 0, HUD_H);
    lv_obj_set_style_bg_color(g_menu, lv_color_make(6, 18, 36), 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_menu, 0, 0);
    lv_obj_clear_flag(g_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);

    g_menu_selbar = lv_obj_create(g_menu);
    lv_obj_set_size(g_menu_selbar, SCR_W - 12, 26);
    lv_obj_set_pos(g_menu_selbar, 6, 70);
    lv_obj_set_style_bg_color(g_menu_selbar, lv_color_make(153, 0, 58), 0); /* 锐胜红 */
    lv_obj_set_style_bg_opa(g_menu_selbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_menu_selbar, 0, 0);
    lv_obj_set_style_radius(g_menu_selbar, 4, 0);
    lv_obj_clear_flag(g_menu_selbar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; ++i) {
        g_menu_rows[i] = make_label(g_menu, "", 12, 74 + i * 30, lv_color_white());
    }

    /* ---------- 图鉴 ---------- */
    g_codex = lv_obj_create(scr);
    lv_obj_set_size(g_codex, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_codex, 0, HUD_H);
    lv_obj_set_style_bg_color(g_codex, lv_color_make(6, 18, 36), 0);
    lv_obj_set_style_bg_opa(g_codex, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_codex, 0, 0);
    lv_obj_clear_flag(g_codex, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_codex, LV_OBJ_FLAG_HIDDEN);

    g_cdx_img  = lv_img_create(g_codex);
    lv_obj_set_pos(g_cdx_img, 92, 14);
    g_cdx_name = make_label(g_codex, "", 8, 56, lv_color_white());
    /* 稀有度/完美标记上移到名称行，meta 只留体长+钓数：
     * 原来「传说 · 最大 888.8 厘米 · 已钓 8888 条 · 完美✓」要 345px，屏只有 240px。
     * DOT 模式兜底 —— 万一以后文案又变长，末尾显示省略号，绝不出屏、绝不折行。 */
    g_cdx_meta = make_label(g_codex, "", 8, 78, lv_color_make(190, 210, 240));
    lv_obj_set_width(g_cdx_meta, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_meta, LV_LABEL_LONG_DOT);
    g_cdx_desc = make_label(g_codex, "", 8, 104, lv_color_white());
    lv_obj_set_width(g_cdx_desc, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_cdx_desc, 4, 0);
    g_cdx_prog = make_label(g_codex, "", 8, 250, lv_color_make(190, 210, 240));
}

/* 面板互斥显示。
 * 加门闸 + 把面板级【固定】提示放在这里：这两句跟帧数据无关，切面板时写一次就够，
 * 原先塞在每帧的 refresh_reel / refresh_codex 里，等于每帧重排一次中文字形。 */
static int s_cur_panel = -1;   /* 当前显示的面板编号；-1 = 还没显示过 */

static void show_only(int which) {
    if (s_cur_panel == which) return;   /* 面板没变：连 5 次 flag 操作都省掉 */
    s_cur_panel = which;

    lv_obj_add_flag(g_scene,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_reel,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_result, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_menu,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_codex,  LV_OBJ_FLAG_HIDDEN);
    if (which == 0) lv_obj_clear_flag(g_scene,  LV_OBJ_FLAG_HIDDEN);
    if (which == 1) lv_obj_clear_flag(g_reel,   LV_OBJ_FLAG_HIDDEN);
    if (which == 2) lv_obj_clear_flag(g_result, LV_OBJ_FLAG_HIDDEN);
    if (which == 3) lv_obj_clear_flag(g_menu,   LV_OBJ_FLAG_HIDDEN);
    if (which == 4) lv_obj_clear_flag(g_codex,  LV_OBJ_FLAG_HIDDEN);

    const char *hint = NULL;
    switch (which) {
        case 1: hint = "按住 OK 抬竿 · 松开落下"; break;   /* 收线：原 refresh_reel 里每帧重设 */
        case 4: hint = "上下翻·OK详情·长按返回";  break;   /* 图鉴：原 refresh_codex 里每帧重设 */
        default: break;   /* 场景/结算/菜单的提示随状态或选中项变化，仍留在各自 refresh（已带缓存） */
    }
    if (hint) set_text_cached(g_lbl_hint, hint);
}

/* ===================== 各面板刷新 ===================== */

/* 电量：图形 + 数字。
 * 刻意不写在 refresh_scene 里 —— 它是常驻顶栏的一部分，收线/菜单/图鉴页也得刷新，
 * 否则在那些页面里电量会冻在上一次的值。
 * 门闸：SoC 本来就只有 500ms 才更新一次，值没变时一个 LVGL API 都别调
 * （原来每帧无条件写 2 次样式 + 1 次中文标签重排）。 */
static int s_batt_drawn = -2;      /* 上一次真正画到屏上的 SoC；-2 = 从来没画过 */

static void hud_batt_refresh(void) {
    if (s_batt_drawn == g_batt_soc) return;
    s_batt_drawn = g_batt_soc;

    if (g_batt_soc < 0) {                      /* 电量计不应答就优雅降级 */
        lv_label_set_text(g_lbl_batt, "--");
        lv_obj_add_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(g_batt_body, lv_color_make(130, 130, 130), 0);
        return;
    }
    lv_obj_clear_flag(g_batt_fill, LV_OBJ_FLAG_HIDDEN);
    /* 低电量连外壳一起变红 —— 只填 2px 红条在 8px 高的电池上根本看不出来 */
    lv_obj_set_style_border_color(g_batt_body,
                                  g_batt_soc < 20 ? lv_color_make(230, 60, 50)
                                                  : lv_color_white(), 0);
    lv_obj_set_width(g_batt_fill, 1 + g_batt_soc * 9 / 100);   /* 1..10 px */
    lv_obj_set_style_bg_color(g_batt_fill,
                              g_batt_soc < 20 ? lv_color_make(230, 60, 50)
                                              : lv_color_make(90, 220, 120), 0);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", g_batt_soc);
    lv_label_set_text(g_lbl_batt, buf);
}

/* 已画到控件上的"业务键"，用来跳过重复写入（-98 是取不到的哨兵值，避免撞上真实键 0/1） */
static int s_bg_spot    = -98;
static int s_fish_scene = -98;

static void refresh_scene(const fishing_status_t *st) {
    /* 背景图只在钓点真的换了才重设 —— 这是原来的头号开销源：
     * lv_image_set_src 没有"同源短路"，每帧都会 invalidate 整张 240x320（153,600 字节）。 */
    set_img_by_key(g_bg, &s_bg_spot, (int)st->spot, BG_IMG[st->spot]);

    /* 钓点名只在文案变化时写；宽度变了才需要重新居中（「深海」比三字名窄 16px） */
    if (set_text_cached(g_lbl_spot, fishing_spot_name(st->spot))) {
        lv_obj_center(g_lbl_spot);
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "得分 %d", st->score);
    set_text_cached(g_lbl_score, buf);
    snprintf(buf, sizeof(buf), "最高 %d", st->high_score);
    set_text_cached(g_lbl_high, buf);

    lv_obj_add_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);
    int bob_y = 96;
    const char *hint = "";
    switch (st->state) {
        case STATE_IDLE:    hint = "OK 抛竿 · 长按菜单"; break;
        case STATE_CASTING: hint = "抛竿中…"; bob_y = 70; break;
        case STATE_WAITING: hint = "等鱼上钩…"; bob_y = 96; break;
        case STATE_BITE:
            hint = "咬钩了！按 OK 提竿";
            bob_y = 116;
            lv_obj_clear_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);
            if (st->cur_species >= 0) {
                set_img_by_key(g_fish_scene, &s_fish_scene, st->cur_species, SIL_IMG[st->cur_species]);
            }
            break;
        default: hint = ""; break;
    }
    lv_obj_set_pos(g_floatbob, 110, bob_y);   /* 值没变时 LVGL 自己会短路 */
    set_text_cached(g_lbl_hint, hint);
}

/* 收线页控件的"已画值"门闸：像素坐标没变就不调 set_size/set_pos，
 * 少一次失效区就少一次重绘（收线页每帧有 9 个分散的失效区）。 */
static int s_zone_h = -1;
static int s_zone_y = -1;
static int s_mark_y = -1;
static int s_prg_h  = -1;

static void refresh_reel(const fishing_status_t *st) {
    char buf[160];
    snprintf(buf, sizeof(buf), "得分 %d", st->score);
    set_text_cached(g_lbl_score, buf);

    /* 捕捉区：把 0..1000 定点换算成像素 */
    int zh = st->reel_bar_h * TRK_H / 1000;
    int top = TRK_Y + st->reel_bar_pos * TRK_H / 1000;
    if (zh < 8) zh = 8;
    if (zh != s_zone_h || top != s_zone_y) {
        s_zone_h = zh;
        s_zone_y = top;
        lv_obj_set_size(g_zone, TRK_W - 4, zh);
        lv_obj_set_pos(g_zone, TRK_X + 2, top);
    }

    int fy = TRK_Y + st->reel_fish_pos * TRK_H / 1000;
    if (fy != s_mark_y) {
        s_mark_y = fy;
        lv_obj_set_pos(g_fishmark, TRK_X + 3, fy - 5);
    }

    int ph = st->reel_progress * PRG_H / 1000;
    if (ph != s_prg_h) {          /* 高度与 y 一一对应，一起更新 */
        s_prg_h = ph;
        lv_obj_set_size(g_prg_fill, PRG_W, ph);
        lv_obj_set_pos(g_prg_fill, PRG_X, PRG_Y + PRG_H - ph);
    }

    const fish_species_t *f = fishing_species_info(st->cur_species);
    snprintf(buf, sizeof(buf), "收线 %d%%\n%s", st->reel_progress / 10,
             f ? f->name : "");
    set_text_cached(g_lbl_reel, buf);
    /* 「按住 OK 抬竿 · 松开落下」是面板固定文案，已移到 show_only(1) 里只在切面板时写一次 */
}

/* 结算页要停 2.2 秒（约 110 帧），全部走缓存收益最明显。
 * key：>=0 = 鱼种，-1 = 跑鱼的默认剪影。 */
static int s_res_img = -99;

static void refresh_result(const fishing_status_t *st) {
    const catch_result_t *res = fishing_last_catch();
    char buf[192];
    if (st->state == STATE_CATCH && res && res->species >= 0) {
        const fish_species_t *f = fishing_species_info(res->species);
        set_img_by_key(g_res_img, &s_res_img, res->species, FISH_IMG[res->species]);
        snprintf(buf, sizeof(buf), "%s  %s", f->name, fishing_rarity_name((rarity_t)f->rarity));
        set_text_cached(g_res_name, buf);
        snprintf(buf, sizeof(buf), "%d.%d 厘米 · %d 克\n本次 +%d 分%s",
                 res->len_mm / 10, res->len_mm % 10, res->wgt_g, res->score,
                 res->perfect ? "\n完美钓获！" : "");
        set_text_cached(g_res_info, buf);
        set_text_cached(g_lbl_hint, res->first_catch ? "新收录！加入图鉴" : "已放入图鉴");
    } else {
        set_img_by_key(g_res_img, &s_res_img, -1, SIL_IMG[0]);
        set_text_cached(g_res_name, "跑鱼了…");
        set_text_cached(g_res_info, "再试一次，注意提前跟竿");
        set_text_cached(g_lbl_hint, "别灰心");
    }
    snprintf(buf, sizeof(buf), "得分 %d", st->score);
    set_text_cached(g_lbl_score, buf);
}

static void refresh_menu(const fishing_status_t *st) {
    char buf[96];
    for (int i = 0; i < 5; ++i) {
        const char *txt = "";
        switch (i) {
            case 0: txt = "开始钓鱼"; break;
            case 1: snprintf(buf, sizeof(buf), "鱼竿   %s", fishing_rod_name(st->rod)); txt = buf; break;
            case 2: snprintf(buf, sizeof(buf), "饵料   %s", fishing_bait_name(st->bait)); txt = buf; break;
            case 3: {
                if (fishing_spot_unlocked(st->spot))
                    snprintf(buf, sizeof(buf), "钓点   %s", fishing_spot_name(st->spot));
                else
                    snprintf(buf, sizeof(buf), "钓点   %s(未解锁)", fishing_spot_name(st->spot));
                txt = buf; break;
            }
            case 4: snprintf(buf, sizeof(buf), "图鉴   %d/%d", st->codex_total, FISH_SPECIES_COUNT); txt = buf; break;
            default: break;
        }
        set_text_cached(g_menu_rows[i], txt);
    }
    lv_obj_set_pos(g_menu_selbar, 6, 70 + g_menu_idx * 30);   /* 值没变时 LVGL 自己会短路 */
    set_text_cached(g_lbl_hint, g_menu_edit ? "上下改值·OK确认" : "上下选·OK进入·长按返回");
}

/* key：>=0 = 已收录（用鱼种序号），否则 = -(idx+1) 表示未收录剪影 */
static int s_cdx_img = -9999;

static void refresh_codex(const fishing_status_t *st) {
    (void)st;
    int idx = fishing_codex_cursor();
    const fish_species_t *f = fishing_species_info(idx);
    bool seen = fishing_codex_is_seen(idx);
    char buf[192];

    set_img_by_key(g_cdx_img, &s_cdx_img, seen ? idx : -(idx + 1),
                   seen ? FISH_IMG[idx] : SIL_IMG[idx]);
    if (seen) {
        snprintf(buf, sizeof(buf), "%d. %s · %s%s", idx + 1, f->name,
                 fishing_rarity_name((rarity_t)f->rarity),
                 fishing_codex_is_perfect(idx) ? " ✓" : "");
        set_text_cached(g_cdx_name, buf);
        snprintf(buf, sizeof(buf), "最长 %d.%d 厘米 · 钓 %d 条",
                 fishing_codex_best_len(idx) / 10, fishing_codex_best_len(idx) % 10,
                 fishing_codex_count(idx));
        set_text_cached(g_cdx_meta, buf);
        set_text_cached(g_cdx_desc, f->desc);
    } else {
        snprintf(buf, sizeof(buf), "%d. ???", idx + 1);
        set_text_cached(g_cdx_name, buf);
        snprintf(buf, sizeof(buf), "%s · 栖息于%s", fishing_rarity_name((rarity_t)f->rarity),
                 fishing_spot_name((spot_t)f->spot));
        set_text_cached(g_cdx_meta, buf);
        set_text_cached(g_cdx_desc, "还没有见过它，钓上来就能解锁");
    }
    fishing_status_t s2;
    fishing_get_status(&s2);
    snprintf(buf, sizeof(buf), "收录 %d/%d   累计钓获 %d", s2.codex_total, FISH_SPECIES_COUNT, s2.total_catch);
    set_text_cached(g_cdx_prog, buf);
    /* 「上下翻·OK详情·长按返回」是面板固定文案，已移到 show_only(4) 里只在切面板时写一次 */
}

static void refresh_ui(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    hud_batt_refresh();                    /* 顶栏电量：所有页面都刷新 */
    switch (st.state) {
        case STATE_MENU:                       show_only(3); refresh_menu(&st);        break;
        case STATE_CODEX:
        case STATE_CODEX_INFO:                 show_only(4); refresh_codex(&st);       break;
        case STATE_REELING:                    show_only(1); refresh_reel(&st);        break;
        case STATE_CATCH:
        case STATE_ESCAPE:                     show_only(2); refresh_result(&st);      break;
        default:                               show_only(0); refresh_scene(&st);       break;
    }
}

/* ===================== 按键处理 ===================== */
static void menu_change_value(int dir) {
    fishing_status_t st;
    fishing_get_status(&st);
    if (g_menu_idx == 1) {
        int v = ((int)st.rod + (dir > 0 ? 1 : FISH_ROD_COUNT - 1)) % FISH_ROD_COUNT;
        fishing_set_rod((rod_t)v);
    } else if (g_menu_idx == 2) {
        int v = ((int)st.bait + (dir > 0 ? 1 : FISH_BAIT_COUNT - 1)) % FISH_BAIT_COUNT;
        fishing_set_bait((bait_t)v);
    } else if (g_menu_idx == 3) {
        /* 只能在已解锁的钓点之间循环，避免玩家选到进不去的关卡 */
        for (int k = 1; k <= FISH_SPOT_COUNT; ++k) {
            int v = ((int)st.spot + (dir > 0 ? k : -k + FISH_SPOT_COUNT * 2)) % FISH_SPOT_COUNT;
            if (fishing_spot_unlocked((spot_t)v)) { fishing_set_spot((spot_t)v); break; }
        }
    }
}

/* ===================== 收线：直接读按键电压判"按住" =====================
 *
 * 为什么不靠按键事件：BSP 只注册了 PRESS_DOWN / SINGLE_CLICK / DOUBLE_CLICK /
 * LONG_PRESS_START（components/bsp/src/bsp_button.c:118-124），**没有松手事件**。
 * button 组件在按住满 1500ms 后松手走 PRESS_LONG_PRESS_UP_CHECK 分支，只发
 * PRESS_UP / LONG_PRESS_UP / PRESS_END —— 应用层一个都收不到，于是捕捉区贴顶下不来；
 * 就算正常触发，SINGLE_CLICK 也要松手后再等 short_press_time(180ms) 才有。
 * 收线恰好就是要按住一两秒的玩法，所以这里改成每帧读一次 ADC 电压问"手指在不在"。
 *
 * 电压表复用 bsp_pins.h，与 button_level()（bsp_button.c:40-65）同源，
 * 换了分压电阻只要改 bsp_pins.h 一处，不会出现第二套窗口。
 * 命名不带 g_/s_ 前缀：tools/check_c_sanity.py 只对 g_* / s_* 做声明校验，
 * 这条带 const 的数组声明不匹配它的正则（与 bsp_button.c:14 保持同一写法）。 */
static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

#define REEL_ADC_FAIL_MAX 10      /* 连续读失败这么多帧（200ms）就退回事件路径 */

static bool g_adc_live        = false;  /* 本局收线里 ADC 是否已确认过一次"按下" */
static int  g_adc_fail        = 0;      /* 连续读失败帧数 */
static bool g_reel_seen_press = false;  /* 本局收线是否见过一次 PRESS */

/* 0 = 松开，1 = 按住，-1 = 读数不可用 */
static int ok_key_state(void) {
    const int mv = bsp_button_read_mv();
    if (mv < 0) return -1;
    return (mv >= BTN_MV[BSP_BTN_OK][0] && mv < BTN_MV[BSP_BTN_OK][1]) ? 1 : 0;
}

/* 每帧（20ms）喂一次 OK 电平，只在 STATE_REELING 调用。
 *
 * g_adc_live 这道闸门解决三件事：
 *   1) 模拟器（FoloToy Passport Simulator 在 ADC 层注入电压，OK=595mV/松开=3300mV）
 *      物理按住要满 300ms 才把 ADC 置成 595，头 300ms 仍是 3300；
 *      没有这道闸门，进收线时会被误判成"松手"让捕捉区先掉一下。
 *   2) 真机第一帧就能读到按下，之后全程走电平，不吃任何事件延迟。
 *   3) 读不出来时自动退回原来的事件路径，且不会被锁死在降级模式。 */
static void reel_poll_hold(void) {
    const int s = ok_key_state();
    if (s < 0) {
        /* 读失败这一帧【不喂】：把"不知道"当成"松手"，一次读取抖动就会误落一次。 */
        if (++g_adc_fail >= REEL_ADC_FAIL_MAX) g_adc_live = false;
        return;
    }
    g_adc_fail = 0;
    if (s == 1) {
        g_adc_live = true;
        fishing_reel_hold_sample(true);
        return;
    }
    if (g_adc_live) fishing_reel_hold_sample(false);   /* 只有确认过按下，才允许判松手 */
}

static void handle_btn(bsp_btn_t btn, bsp_btn_ev_t ev) {
    fishing_status_t st;
    fishing_get_status(&st);

    /* ---- 收线中：按住 / 松开 ----
     * 松手判定已改由 game_task 每帧轮询 ADC（reel_poll_hold → fishing_reel_hold_sample），
     * 这里只保留两件事：让按下再快一点，以及 ADC 读不出来时的兜底。 */
    if (st.state == STATE_REELING) {
        if (btn != BSP_BTN_OK) return;
        if (ev == BSP_BTN_PRESS) {
            /* 按下立即生效（比等下一帧轮询快 <=20ms）。
             * 万一这是进收线前残留的旧事件，轮询会在两帧内用真实电压纠正回来。 */
            g_reel_seen_press = true;
            if (!g_adc_live) fishing_reel_hold_sample(true);
            return;
        }
        /* 只有 ADC 不可用、且本次收线确实见过按下时，才用事件判松手。
         * g_reel_seen_press 挡掉"从菜单/图鉴进收线时队列里残留的 CLICK"——
         * 那些事件对应的按下根本不在这次收线里。 */
        if (!g_adc_live && g_reel_seen_press &&
            (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE)) {
            fishing_reel_hold_sample(false);
        }
        return;
    }

    /* ---- 图鉴 ---- */
    if (st.state == STATE_CODEX || st.state == STATE_CODEX_INFO) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) { fishing_codex_close(); return; }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)        fishing_codex_move(-1);
        else if (btn == BSP_BTN_DOWN) fishing_codex_move(1);
        else if (btn == BSP_BTN_OK)   fishing_codex_toggle_info();
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- 菜单 ---- */
    if (st.state == STATE_MENU) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            fishing_exit_menu(); g_menu_edit = 0; fishing_audio_play(SFX_CLICK);
            return;
        }
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)   { if (g_menu_edit) menu_change_value(-1); else g_menu_idx = (g_menu_idx + 4) % 5; }
        else if (btn == BSP_BTN_DOWN) { if (g_menu_edit) menu_change_value(1); else g_menu_idx = (g_menu_idx + 1) % 5; }
        else if (btn == BSP_BTN_OK) {
            if (g_menu_idx == 0) {
                fishing_exit_menu();
                fishing_cast();
            } else if (g_menu_idx == 4) {
                fishing_codex_open();
            } else {
                g_menu_edit = !g_menu_edit;
            }
        }
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- 场景：OK 抛竿 / 提竿，长按开菜单 ---- */
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK && st.state == STATE_IDLE) {
        fishing_enter_menu(); fishing_audio_play(SFX_CLICK); return;
    }
    if (ev == BSP_BTN_PRESS && btn == BSP_BTN_OK) {
        if (st.state == STATE_IDLE) { fishing_cast(); fishing_audio_play(SFX_CLICK); }
        else if (st.state == STATE_BITE) {
            fishing_strike();
            fishing_audio_play(SFX_CLICK);
        }
    }
}

/* ===================== 主任务 ===================== */

/* 主循环节奏。收线是这个应用里唯一对"手到屏幕上"敏感的界面：
 * 按键电平靠主循环采样，循环慢一倍、感知延迟就慢一倍，
 * 所以收线期间把周期压到 10ms（100Hz），其他状态没必要快、保持 20ms 省电。
 * 画面单独节流到 20ms：物理可以跑 100Hz，但 240x320 局部重绘翻倍后仍会
 * 拖长循环周期，反过来把采样频率吃回去 —— 手感和流畅度要分开调。 */
#define LOOP_IDLE_MS     20
#define LOOP_REELING_MS  10
#define UI_REFRESH_MS    20

static void game_task(void *arg) {
    (void)arg;
    int last_ui_ms = 0;
    int last_state = -1;
    for (;;) {
        btn_ev_t e;
        while (xQueueReceive(s_btn_q, &e, 0) == pdTRUE) handle_btn(e.btn, e.ev);

        /* 收线期间：先喂本帧的 OK 电平，再推进物理 —— 顺序反了这一帧用的就是上一帧的按键状态。
         * 离开收线时复位，下次进收线从干净状态开始（否则会带着上一次的降级标记）。 */
        fishing_status_t cur;
        fishing_get_status(&cur);
        const bool reeling = (cur.state == STATE_REELING);
        if (reeling) {
            reel_poll_hold();
        } else {
            g_adc_live        = false;
            g_adc_fail        = 0;
            g_reel_seen_press = false;
        }

        int now = (int)now_ms();
        fishing_event_t evt = fishing_tick(now);
        if (evt == EVT_BITE_START)        fishing_audio_play(SFX_BITE);
        else if (evt == EVT_BITE_TIMEOUT) fishing_audio_play(SFX_MISS);
        else if (evt == EVT_CATCH)        { fishing_audio_play(SFX_CATCH); nvs_save_now(); }
        else if (evt == EVT_ESCAPE)       fishing_audio_play(SFX_MISS);

        /* 电量节流 */
        if (now - g_last_batt_ms > 500) { g_batt_soc = bsp_battery_soc(); g_last_batt_ms = now; }
        /* 每 30 秒兜底存一次（钓到鱼时也会存），避免频繁擦写 flash */
        if (now - g_last_save_ms > 30000) { nvs_save_now(); g_last_save_ms = now; }

        /* 状态切换立刻出画面（否则切页要看 20ms 的节流脸色）；同一画面内才节流。 */
        if (cur.state != last_state || now - last_ui_ms >= UI_REFRESH_MS) {
            last_ui_ms = now;
            last_state = cur.state;
            if (bsp_lvgl_lock(100)) { refresh_ui(); bsp_lvgl_unlock(); }
        }

        vTaskDelay(pdMS_TO_TICKS(reeling ? LOOP_REELING_MS : LOOP_IDLE_MS));
    }
}

void fishing_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_btn_q) return;
    const btn_ev_t e = { btn, ev };
    xQueueSend(s_btn_q, &e, 0);
}

/* ===================== 入口 ===================== */
void fishing_app_start(void) {
    s_btn_q = xQueueCreate(16, sizeof(btn_ev_t));

    fishing_init(now_ms());
    nvs_load_save();                 /* 内含版本校验/迁移，失败就当新号 */
    fishing_audio_init();

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();
        bsp_lvgl_unlock();
    }
    xTaskCreate(game_task, "fishing", 6144, NULL, 5, NULL);
}
