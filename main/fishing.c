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

/* 钓场道具（竿 / 线 / 漂 / 饵）落点。全部是"场景内相对坐标"。
 *
 * 尺寸不是美术原稿尺寸：原稿只有 12~24px，直接摆进 240×320 像一粒沙。
 * tools/gen_sprites.py 的 PROPS 表已把竿/漂/饵按 3× NEAREST 放大并整体旋转
 * （先放大再旋转，斜边是 3px 台阶，符合像素风），这里写死的是烘焙后的结果：
 *   竿 60×(51~63)、漂 24×32、饵 32×32 —— 改尺寸要连着那张表一起改。
 *
 * ROD_W 取 60：三根竿烘焙后宽度都是 60，所以竿尖可以共用一个相对偏移，
 * 换竿时鱼线起点不用重算。 */
#define ROD_X       8
#define ROD_Y       116
#define ROD_W       60
#define ROD_TIP_DX  (ROD_W - 6)   /* 竿尖 ≈ 旋转包围盒右上角 */
#define ROD_TIP_DY  6
#define FLOAT_X     150
#define FLOAT_W     24
#define FLOAT_Y     146           /* 浮漂顶部贴在水平线上 */
#define FLOAT_CAST_Y (FLOAT_Y - 40)   /* 抛竿中：还在空中 */
#define FLOAT_BITE_Y (FLOAT_Y + 8)    /* 咬钩：下沉一下 */
#define BAIT_DY     29            /* 饵吊在漂下：漂高 32 - 3 */

/* ===================== 钓点氛围：时段 / 天气（纯视觉） =====================
 *
 * 为什么用"游戏内时钟"而不是真实时间：这台机器是纯离线玩法，没有 RTC 也没有 SNTP，
 * 固件里唯一能拿到的时钟是开机以来的单调毫秒（xTaskGetTickCount）。所以这里定义
 * 一条"钓鱼日"：1 个游戏小时 = 1 分钟真实时间，24 个游戏小时 = 1 个钓鱼日（现实
 * 24 分钟走完一昼夜）。重启后重新开始 —— 离线设备没有别的时间可依。
 *
 * 天气按"钓鱼日"序号做确定性整数散列：同一天永远是同一种天气，隔天轮换。
 *
 * 红线：这块只影响观感。咬钩时长、鱼种分布、得分倍率一律不动，
 * fishing_logic.c 一个字节都没改过。 */
#define ENV_HOUR_MS   60000u              /* 1 个游戏小时 = 1 分钟真实时间 */
#define ENV_DAY_MS    (24u * ENV_HOUR_MS) /* 1 个钓鱼日 = 24 分钟真实时间 */
#define ENV_RAIN_DROPS 14                 /* 雨丝条数（静态，不做动画） */

/* ===================== 时钟 / NVS ===================== */
static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

#define NVS_NS   "fishing"
#define NVS_KEY  "save"     /* 版本化存档 blob，见 fishing_logic.c 的说明 */
#define NVS_KEY_SND "snd"   /* 声音开关（u8：1=开 0=关）。刻意和游戏进度分开存，
                             * 这样加"设置"不用动存档结构，老存档照旧能读。 */

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

/* 声音开关的读写。用 READWRITE 打开（命名空间不存在时会自动建），
 * 读不到 key 就保持默认"开"——不能因为没存过就把用户的声音关掉。 */
static void nvs_load_sound(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t v = 1;
    if (nvs_get_u8(h, NVS_KEY_SND, &v) == ESP_OK) {
        fishing_audio_set_muted(v == 0);
    }
    nvs_close(h);
}

static void nvs_save_sound(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SND, fishing_audio_is_muted() ? 0 : 1);
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
/* 钓场里真正会画出来的道具：手上的竿 + 挂在钩上的饵。
 * 这两组图一直在 sprites.h 里，但 v2 的界面从来没引用过 —— 换竿换饵只有
 * 菜单文字变、画面一动不动，玩家会以为没生效。 */
static const lv_image_dsc_t *const ROD_IMG[FISH_ROD_COUNT]   = { &rod_hand, &rod_lure, &rod_sea };
static const lv_image_dsc_t *const BAIT_IMG[FISH_BAIT_COUNT] = { &bait_worm, &bait_dough, &bait_spinner };

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
static lv_obj_t *g_rod         = NULL;   /* 手里的鱼竿（随鱼竿选择切换） */
static lv_obj_t *g_line        = NULL;   /* 鱼线：竿尖 -> 浮漂，2 个点 */
static lv_obj_t *g_floatbob    = NULL;
static lv_obj_t *g_bait        = NULL;   /* 钩上的饵（随饵料选择切换） */
static lv_obj_t *g_fish_scene  = NULL;

/* 氛围层：时段 + 天气。g_tint 盖在背景图之上、场景面板之下（z 序靠创建顺序），
 * 所以只压背景、不会盖住竿和浮漂；雨丝同理，是"落在场景前面"的雨。 */
static lv_obj_t *g_tint        = NULL;                 /* 全屏染色层 */
static lv_obj_t *g_rain[ENV_RAIN_DROPS] = { 0 };        /* 雨丝：静态短线 */
static lv_point_precise_t g_rain_pts[ENV_RAIN_DROPS][2];
static lv_obj_t *g_lbl_env     = NULL;                 /* 左上角「时段 · 天气」 */
static lv_point_precise_t g_line_pts[2];

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

/* 菜单：5 项玩法设置 + 1 项声音开关（声音只有开/关两态，单击直接切，不进编辑态） */
static lv_obj_t *g_menu        = NULL;
static lv_obj_t *g_menu_rows[6];
static lv_obj_t *g_menu_selbar = NULL;
static int g_menu_idx = 0;
static int g_menu_edit = 0;   /* 是否进入编辑某项 */

/* 图鉴 */
static lv_obj_t *g_codex       = NULL;
static lv_obj_t *g_cdx_img     = NULL;
static lv_obj_t *g_cdx_name    = NULL;
static lv_obj_t *g_cdx_meta    = NULL;   /* 已收录：最大纪录 + 钓获数；未收录：稀有度 + 栖息地 */
static lv_obj_t *g_cdx_size    = NULL;   /* 体长范围 + 偏好饵料（收录与否都显示，是"怎么钓"的提示） */
static lv_obj_t *g_cdx_desc    = NULL;
static lv_obj_t *g_cdx_lore    = NULL;   /* 性格钩子：这鱼什么脾气（新增，纯文案） */
static lv_obj_t *g_cdx_mile    = NULL;   /* 收集里程碑：下一个里程碑叫什么、还差几种 */
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

    /* ---- 氛围层（时段/天气） ----
     * 顺序就是 z 序：g_bg → g_tint → 雨丝 → …… → g_scene。
     * 所以染色层和雨丝都压在背景上、又在场景面板之下，竿/浮漂/鱼不会被染到。
     * 雨丝是静态短线（不做下落动画）：一帧一帧挪位置就是每帧 invalidate，
     * 那是这套代码里最不能碰的性能命门。 */
    g_tint = lv_obj_create(scr);
    lv_obj_set_size(g_tint, SCR_W, SCR_H);
    lv_obj_set_pos(g_tint, 0, 0);
    lv_obj_set_style_bg_color(g_tint, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(g_tint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_tint, 0, 0);
    lv_obj_set_style_radius(g_tint, 0, 0);
    lv_obj_set_style_pad_all(g_tint, 0, 0);
    lv_obj_clear_flag(g_tint, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < ENV_RAIN_DROPS; ++i) {
        int rx = 12 + (i * 17) % 228;      /* 横向铺开 */
        int ry = 28 + (i * 61) % 268;      /* 高度错开，避免排成整齐的栅栏 */
        g_rain_pts[i][0].x = rx;
        g_rain_pts[i][0].y = ry;
        g_rain_pts[i][1].x = rx - 6;       /* 斜向左下，像正在落的雨 */
        g_rain_pts[i][1].y = ry + 16;
        g_rain[i] = lv_line_create(scr);
        lv_obj_set_pos(g_rain[i], 0, 0);
        lv_obj_set_style_line_width(g_rain[i], 1, 0);
        lv_obj_set_style_line_color(g_rain[i], lv_color_make(205, 222, 245), 0);
        lv_obj_set_style_line_opa(g_rain[i], 120, 0);
        lv_line_set_points(g_rain[i], g_rain_pts[i], 2);
        lv_obj_add_flag(g_rain[i], LV_OBJ_FLAG_HIDDEN);   /* 默认不下雨 */
    }

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
    /* 显式清零内边距：lv_obj_create() 的默认样式带约 13px 内边距，
     * 子元素坐标是相对【内容区】的，不清零的话所有子元素都比源码里写的
     * 往右下偏 13px —— 实测证据：菜单选中条比预期低 12~13px、
     * 右上角「静水塘」胶囊被屏幕右缘切掉、图鉴最后一行被底部提示条压住。
     * 清零后坐标才名副其实，布局检查器（只加父级 x/y、不计父级内边距）
     * 也才和实机对得上。 */
    /* 显式清零内边距：lv_obj_create() 的默认样式带约 13px 内边距，而子元素
     * 坐标是相对【内容区】的。不清零的话所有子元素都比源码里写的往右下偏
     * 13px —— 实测证据：菜单选中条比预期低 12~13px、右上角「静水塘」胶囊被
     * 屏幕右缘切掉、图鉴最后一行被底部提示条压住。清零后坐标才名副其实，
     * 布局检查器（只加父级 x/y、不计父级内边距）也才和实机对得上。 */
    lv_obj_set_style_border_width(g_scene, 0, 0);
    lv_obj_set_style_pad_all(g_scene, 0, 0);
    lv_obj_clear_flag(g_scene, LV_OBJ_FLAG_SCROLLABLE);

    /* 环境标签：左上角「时段 · 天气」。
     * 底下一层半透明黑底 —— 天空是亮蓝的，白字直接压上去看不清。
     * 位置 (8,6) 是场景面板内坐标（面板从屏幕 y=22 起），所以在屏幕上约 y=28；
     * 那个高度圆角只切掉 x=0 一列，x=8 安全。 */
    g_lbl_env = make_label(g_scene, "", 8, 6, lv_color_white());
    lv_obj_set_style_bg_color(g_lbl_env, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_lbl_env, 120, 0);
    lv_obj_set_style_pad_all(g_lbl_env, 2, 0);

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

    /* 鱼线先建：它要压在竿和漂下面。
     * lv_line 的点是"相对自身坐标"的，所以对象固定在 (0,0)、点直接用场景坐标，
     * 省掉一层换算。线宽 1px、半透明白 —— 和像素背景叠得住，又不抢戏。 */
    g_line = lv_line_create(g_scene);
    lv_obj_set_pos(g_line, 0, 0);
    lv_obj_set_style_line_width(g_line, 1, 0);
    lv_obj_set_style_line_color(g_line, lv_color_make(244, 248, 255), 0);
    lv_obj_set_style_line_opa(g_line, 190, 0);
    lv_line_set_points(g_line, g_line_pts, 2);

    g_rod = lv_img_create(g_scene);
    lv_img_set_src(g_rod, ROD_IMG[0]);
    lv_obj_set_pos(g_rod, ROD_X, ROD_Y);

    g_floatbob = lv_img_create(g_scene);
    lv_img_set_src(g_floatbob, &prop_float);
    lv_obj_set_pos(g_floatbob, FLOAT_X, FLOAT_Y);

    g_bait = lv_img_create(g_scene);
    lv_img_set_src(g_bait, BAIT_IMG[0]);
    lv_obj_set_pos(g_bait, FLOAT_X + FLOAT_W / 2 - 16, FLOAT_Y + BAIT_DY);

    g_fish_scene = lv_img_create(g_scene);
    lv_img_set_src(g_fish_scene, FISH_IMG[0]);
    lv_obj_set_pos(g_fish_scene, 20, 176);
    lv_obj_add_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);

    /* ---------- 收线界面 ---------- */
    g_reel = lv_obj_create(scr);
    lv_obj_set_size(g_reel, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_reel, 0, HUD_H);
    lv_obj_set_style_bg_color(g_reel, lv_color_make(10, 30, 60), 0);
    lv_obj_set_style_bg_opa(g_reel, LV_OPA_COVER, 0);
    /* 显式清零内边距：lv_obj_create() 的默认样式带约 13px 内边距，
     * 子元素坐标是相对【内容区】的，不清零的话所有子元素都比源码里写的
     * 往右下偏 13px —— 实测证据：菜单选中条比预期低 12~13px、
     * 右上角「静水塘」胶囊被屏幕右缘切掉、图鉴最后一行被底部提示条压住。
     * 清零后坐标才名副其实，布局检查器（只加父级 x/y、不计父级内边距）
     * 也才和实机对得上。 */
    /* 清零内边距：面板子元素坐标相对内容区，默认内边距会让整体偏 13px */
    lv_obj_set_style_border_width(g_reel, 0, 0);
    lv_obj_set_style_pad_all(g_reel, 0, 0);
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
    /* 显式清零内边距：lv_obj_create() 的默认样式带约 13px 内边距，
     * 子元素坐标是相对【内容区】的，不清零的话所有子元素都比源码里写的
     * 往右下偏 13px —— 实测证据：菜单选中条比预期低 12~13px、
     * 右上角「静水塘」胶囊被屏幕右缘切掉、图鉴最后一行被底部提示条压住。
     * 清零后坐标才名副其实，布局检查器（只加父级 x/y、不计父级内边距）
     * 也才和实机对得上。 */
    /* 清零内边距：面板子元素坐标相对内容区，默认内边距会让整体偏 13px */
    lv_obj_set_style_border_width(g_result, 0, 0);
    lv_obj_set_style_pad_all(g_result, 0, 0);
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
    /* 显式清零内边距：lv_obj_create() 的默认样式带约 13px 内边距，
     * 子元素坐标是相对【内容区】的，不清零的话所有子元素都比源码里写的
     * 往右下偏 13px —— 实测证据：菜单选中条比预期低 12~13px、
     * 右上角「静水塘」胶囊被屏幕右缘切掉、图鉴最后一行被底部提示条压住。
     * 清零后坐标才名副其实，布局检查器（只加父级 x/y、不计父级内边距）
     * 也才和实机对得上。 */
    /* 清零内边距：面板子元素坐标相对内容区，默认内边距会让整体偏 13px */
    lv_obj_set_style_border_width(g_menu, 0, 0);
    lv_obj_set_style_pad_all(g_menu, 0, 0);
    lv_obj_clear_flag(g_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);

    g_menu_selbar = lv_obj_create(g_menu);
    lv_obj_set_size(g_menu_selbar, SCR_W - 12, 26);
    lv_obj_set_pos(g_menu_selbar, 6, 70);
    lv_obj_set_style_bg_color(g_menu_selbar, lv_color_make(153, 0, 58), 0); /* 高亮红 */
    lv_obj_set_style_bg_opa(g_menu_selbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_menu_selbar, 0, 0);
    lv_obj_set_style_radius(g_menu_selbar, 4, 0);
    lv_obj_clear_flag(g_menu_selbar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 6; ++i) {
        g_menu_rows[i] = make_label(g_menu, "", 12, 74 + i * 30, lv_color_white());
    }

    /* ---------- 图鉴 ---------- */
    g_codex = lv_obj_create(scr);
    lv_obj_set_size(g_codex, SCR_W, SCR_H - HUD_H - 24);
    lv_obj_set_pos(g_codex, 0, HUD_H);
    lv_obj_set_style_bg_color(g_codex, lv_color_make(6, 18, 36), 0);
    lv_obj_set_style_bg_opa(g_codex, LV_OPA_COVER, 0);
    /* 显式清零内边距：lv_obj_create() 的默认样式带约 13px 内边距，
     * 子元素坐标是相对【内容区】的，不清零的话所有子元素都比源码里写的
     * 往右下偏 13px —— 实测证据：菜单选中条比预期低 12~13px、
     * 右上角「静水塘」胶囊被屏幕右缘切掉、图鉴最后一行被底部提示条压住。
     * 清零后坐标才名副其实，布局检查器（只加父级 x/y、不计父级内边距）
     * 也才和实机对得上。 */
    /* 清零内边距：面板子元素坐标相对内容区，默认内边距会让整体偏 13px */
    lv_obj_set_style_border_width(g_codex, 0, 0);
    lv_obj_set_style_pad_all(g_codex, 0, 0);
    lv_obj_clear_flag(g_codex, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_codex, LV_OBJ_FLAG_HIDDEN);

    g_cdx_img  = lv_img_create(g_codex);
    lv_obj_set_pos(g_cdx_img, 92, 14);
    g_cdx_name = make_label(g_codex, "", 8, 54, lv_color_white());
    /* 稀有度/完美标记上移到名称行，meta 只留体长+钓数：
     * 原来「传说 · 最大 888.8 厘米 · 已钓 8888 条 · 完美✓」要 345px，屏只有 240px。
     * DOT 模式兜底 —— 万一以后文案又变长，末尾显示省略号，绝不出屏、绝不折行。 */
    g_cdx_meta = make_label(g_codex, "", 8, 78, lv_color_make(255, 208, 90));  /* 金色：突出"最大纪录" */
    lv_obj_set_width(g_cdx_meta, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_meta, LV_LABEL_LONG_DOT);
    /* 体型范围 + 偏好饵料：这是"这条鱼能长多大、爱吃什么"的攻略信息，
     * 收录与否都显示 —— 没钓到之前它正好告诉玩家该去哪、用什么饵。 */
    g_cdx_size = make_label(g_codex, "", 8, 102, lv_color_make(150, 215, 255));
    lv_obj_set_width(g_cdx_size, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_size, LV_LABEL_LONG_DOT);
    g_cdx_desc = make_label(g_codex, "", 8, 126, lv_color_white());
    lv_obj_set_width(g_cdx_desc, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_cdx_desc, 4, 0);
    /* 性格钩子（lore）：这鱼什么脾气、有没有什么故事。
     * desc 是客观说明（长什么样），lore 是性格与钩子，两者分开写，玩家
     * 看完 desc 判断"能不能钓"，看完 lore 决定"想不想钓"。
     * 位置压在 desc 之下、里程碑之上；最坏 2 行（lore 最长 22 字 × 16px = 352px
     * ÷ 224px 行宽 = 2 行），行高 20 + 行距 4 = 44px，126+44+6=176 起排不压 desc。 */
    g_cdx_lore = make_label(g_codex, "", 8, 176, lv_color_make(255, 236, 205));
    lv_obj_set_width(g_cdx_lore, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_lore, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_cdx_lore, 4, 0);
    /* 里程碑：纯提示，不奖励任何数值。只说"下一个里程碑叫什么、还差几种"。 */
    g_cdx_mile = make_label(g_codex, "", 8, 224, lv_color_make(150, 230, 175));
    lv_obj_set_width(g_cdx_mile, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_mile, LV_LABEL_LONG_DOT);
    g_cdx_prog = make_label(g_codex, "", 8, 248, lv_color_make(190, 210, 240));
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
static int s_rod_img    = -98;
static int s_bait_img   = -98;
static int s_tackle     = -99;   /* 上一次摆好的浮漂 y */

/* 浮漂 / 鱼饵 / 鱼线是一套跟着 bob_y 走的道具，一次摆好。
 * 只在 bob_y 真的变了才动 LVGL —— 主循环 20ms 一帧，无条件摆会让整条线
 * 每帧失效重绘（收线页那份"已画值门闸"就是为这个加的）。 */
static void place_tackle(int bob_y) {
    if (bob_y == s_tackle) return;
    s_tackle = bob_y;
    lv_obj_set_pos(g_floatbob, FLOAT_X, bob_y);
    lv_obj_set_pos(g_bait, FLOAT_X + FLOAT_W / 2 - 16, bob_y + BAIT_DY);
    g_line_pts[0].x = ROD_X + ROD_TIP_DX;      /* 竿尖 */
    g_line_pts[0].y = ROD_Y + ROD_TIP_DY;
    g_line_pts[1].x = FLOAT_X + FLOAT_W / 2;   /* 漂顶 */
    g_line_pts[1].y = bob_y;
    lv_line_set_points(g_line, g_line_pts, 2);
}

/* ===================== 钓点氛围：时段 / 天气 =====================
 * 全部是纯计算 + 纯视觉，不碰 fishing_logic.c 的任何平衡数值。 */

typedef enum {
    ENV_MORNING = 0,   /* 清晨 5..9 点 */
    ENV_NOON,          /* 正午 10..16 点 */
    ENV_DUSK,          /* 黄昏 17..19 点 */
    ENV_NIGHT,         /* 深夜 20..4 点 */
    ENV_PERIOD_COUNT
} env_period_t;

typedef enum {
    ENV_CLEAR = 0,     /* 晴 */
    ENV_CLOUDY,        /* 阴 */
    ENV_RAIN,          /* 雨 */
    ENV_WEATHER_COUNT
} env_weather_t;

static int env_hour(uint32_t now) { return (int)((now / ENV_HOUR_MS) % 24u); }
static int env_day(uint32_t now)  { return (int)(now / ENV_DAY_MS); }

static env_period_t env_period_of(int hour) {
    if (hour >= 5 && hour < 10) return ENV_MORNING;
    if (hour >= 10 && hour < 17) return ENV_NOON;
    if (hour >= 17 && hour < 20) return ENV_DUSK;
    return ENV_NIGHT;
}

/* 天气：按"钓鱼日"序号做整数散列（不是随机数，没有状态，也不会每帧跳）。
 * 同一天永远是同一种天气，隔天轮换；晴 60% / 阴 25% / 雨 15%。 */
static env_weather_t env_weather_of(int day) {
    uint32_t h = (uint32_t)day * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    unsigned r = (unsigned)(h % 20u);
    if (r < 12u) return ENV_CLEAR;
    if (r < 17u) return ENV_CLOUDY;
    return ENV_RAIN;
}

static const char *env_period_name(env_period_t p) {
    switch (p) {
        case ENV_MORNING: return "清晨";
        case ENV_NOON:    return "正午";
        case ENV_DUSK:    return "黄昏";
        default:          return "深夜";
    }
}

static const char *env_weather_name(env_weather_t w) {
    switch (w) {
        case ENV_CLEAR:  return "晴";
        case ENV_CLOUDY: return "阴";
        default:         return "雨";
    }
}

/* 时段底色 + 天气覆盖，合成一层盖在背景上的半透明色块。
 * 纯整数运算（不用浮点），LVGL 的 opacity 取 0..255。 */
static void env_visual(env_period_t p, env_weather_t w, lv_color_t *out, int *opa) {
    int r, g, b, o;
    switch (p) {
        case ENV_MORNING: r = 255; g = 178; b =  96; o =  44; break;  /* 暖橙 */
        case ENV_NOON:    r = 255; g = 250; b = 225; o =   0; break;  /* 晴天不染色 */
        case ENV_DUSK:    r = 255; g = 124; b =  62; o =  72; break;  /* 橙红 */
        default:          r =  24; g =  44; b = 108; o = 120; break;  /* 冷蓝夜色 */
    }
    if (w == ENV_CLOUDY) {
        r = (r * 45 + 130 * 55) / 100;      /* 往灰蓝靠 */
        g = (g * 45 + 138 * 55) / 100;
        b = (b * 45 + 148 * 55) / 100;
        if (o < 66) o = 66;
    } else if (w == ENV_RAIN) {
        r = (r * 35 +  92 * 65) / 100;      /* 更冷更暗 */
        g = (g * 35 + 110 * 65) / 100;
        b = (b * 35 + 150 * 65) / 100;
        if (o < 96) o = 96;
    }
    *out = lv_color_make(r, g, b);
    *opa = o;
}

/* 已画值门闸：时段/天气没变就一个 LVGL 调用都不做。
 * 主循环 20ms 一帧，无条件重设样式 = 每帧让整屏失效重绘。 */
static int s_env_key = -1;

static void refresh_env(void) {
    const uint32_t now = (uint32_t)now_ms();
    const env_period_t  p = env_period_of(env_hour(now));
    const env_weather_t w = env_weather_of(env_day(now));
    const int key = (int)p * ENV_WEATHER_COUNT + (int)w;

    if (key != s_env_key) {
        s_env_key = key;
        lv_color_t c;
        int opa;
        env_visual(p, w, &c, &opa);
        lv_obj_set_style_bg_color(g_tint, c, 0);
        lv_obj_set_style_bg_opa(g_tint, opa, 0);
        for (int i = 0; i < ENV_RAIN_DROPS; ++i) {
            if (w == ENV_RAIN) lv_obj_clear_flag(g_rain[i], LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag(g_rain[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%s · %s", env_period_name(p), env_weather_name(w));
    set_text_cached(g_lbl_env, buf);
}

static void refresh_scene(const fishing_status_t *st) {
    /* 背景图只在钓点真的换了才重设 —— 这是原来的头号开销源：
     * lv_image_set_src 没有"同源短路"，每帧都会 invalidate 整张 240x320（153,600 字节）。 */
    set_img_by_key(g_bg, &s_bg_spot, (int)st->spot, BG_IMG[st->spot]);

    /* 氛围（时段/天气）：自己带已画值门闸，时段没变就一个 LVGL 调用都不做。 */
    refresh_env();

    /* 换竿 / 换饵要看得见：菜单里选完，钓场上的竿和钩上的饵当场就换。
     * 这两组 sprite 早就编进了固件，但 v2 的界面从没引用过 ——
     * 所以"选完鱼竿/鱼饵画面没变化"不是错觉，是真没画。 */
    set_img_by_key(g_rod,  &s_rod_img,  (int)st->rod,  ROD_IMG[st->rod]);
    set_img_by_key(g_bait, &s_bait_img, (int)st->bait, BAIT_IMG[st->bait]);

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
    int bob_y = FLOAT_Y;
    const char *hint = "";
    switch (st->state) {
        case STATE_IDLE:    hint = "OK 抛竿 · 长按菜单"; break;
        case STATE_CASTING: hint = "抛竿中…"; bob_y = FLOAT_CAST_Y; break;
        case STATE_WAITING: hint = "等鱼上钩…"; bob_y = FLOAT_Y; break;
        case STATE_BITE:
            hint = "咬钩了！按 OK 提竿";
            bob_y = FLOAT_BITE_Y;
            lv_obj_clear_flag(g_fish_scene, LV_OBJ_FLAG_HIDDEN);
            if (st->cur_species >= 0) {
                set_img_by_key(g_fish_scene, &s_fish_scene, st->cur_species, SIL_IMG[st->cur_species]);
            }
            break;
        default: hint = ""; break;
    }
    place_tackle(bob_y);
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
    for (int i = 0; i < 6; ++i) {
        const char *txt = "";
        switch (i) {
            case 0: txt = "开始钓鱼"; break;
            case 1: snprintf(buf, sizeof(buf), "鱼竿   %s", fishing_rod_name(st->rod)); txt = buf; break;
            case 2: snprintf(buf, sizeof(buf), "饵料   %s", fishing_bait_name(st->bait)); txt = buf; break;
            case 3: snprintf(buf, sizeof(buf), "钓点   %s", fishing_spot_name(st->spot)); txt = buf; break;
            case 4: snprintf(buf, sizeof(buf), "图鉴   %d/%d", st->codex_total, FISH_SPECIES_COUNT); txt = buf; break;
            case 5: snprintf(buf, sizeof(buf), "声音   %s", fishing_audio_is_muted() ? "关" : "开"); txt = buf; break;
            default: break;
        }
        set_text_cached(g_menu_rows[i], txt);
    }
    lv_obj_set_pos(g_menu_selbar, 6, 70 + g_menu_idx * 30);   /* 值没变时 LVGL 自己会短路 */

    /* 钓点这一行：没解锁的钓点选不进去（fishing_set_spot 会拒绝），
     * 但之前这版只做了"静默跳过"，玩家按住上下键画面纹丝不动，看上去就是坏了
     * （社区反馈的原话是"不能更换钓点"）。光标停在这一行时，把还差几条直接写在
     * 提示条上 —— 选中/进入编辑两种状态都给，因为"按了没反应"正是发生在编辑态。 */
    char hb[64];
    const char *hint;
    int locked = fishing_next_locked_spot();
    if (g_menu_idx == 3 && locked >= 0) {
        snprintf(hb, sizeof(hb), "再钓 %d 条解锁 %s",
                 fishing_spot_unlock_left((spot_t)locked), fishing_spot_name((spot_t)locked));
        hint = hb;
    } else if (g_menu_edit) {
        hint = "上下改值·OK确认";
    } else {
        hint = "上下选·OK进入·长按返回";
    }
    set_text_cached(g_lbl_hint, hint);
}

/* key：>=0 = 已收录（用鱼种序号），否则 = -(idx+1) 表示未收录剪影 */
static int s_cdx_img = -9999;

/* 取偏好的第一种饵料名，mask 位低位优先；mask 为 0（不挑饵）返回 NULL。
 * 只列一种：三个饵全列成"蚯蚓/面团/亮片"会把这一行撑出 240px 屏。 */
static const char *first_bait_name(uint8_t mask) {
    if (mask & 0x01u) return fishing_bait_name(BAIT_WORM);
    if (mask & 0x02u) return fishing_bait_name(BAIT_DOUGH);
    if (mask & 0x04u) return fishing_bait_name(BAIT_LURE);
    return NULL;
}

/* ===================== 收集里程碑（纯提示，不奖励任何数值） =====================
 * 图鉴要能给出长期目标感：每集齐一批就点亮一个名号。
 * 纯文案 —— 不进逻辑层、不写存档、不影响计分和掉落，只是把"下一个名号叫什么、
 * 还差几种"讲清楚。阈值最后一个用 FISH_SPECIES_COUNT 派生，将来加鱼不用改这里。 */
typedef struct {
    int need;              /* 集齐多少种算达成 */
    const char *name;      /* 里程碑名 */
} codex_mile_t;

static const codex_mile_t CODEX_MILES[] = {
    {  4, "初识水性" },
    {  8, "渐入佳境" },
    { 12, "半塘好手" },
    { 16, "远近闻名" },
    { 20, "一方霸主" },
    { FISH_SPECIES_COUNT, "竿影大成" },
};
#define CODEX_MILE_COUNT ((int)(sizeof(CODEX_MILES) / sizeof(CODEX_MILES[0])))

/* 下一个还没达成的里程碑（返回它的达成数，名字写到 *name）。
 * 全都达成了就返回最后一条，调用方靠"已集齐"分支区分。 */
static int codex_next_mile(int collected, const char **name) {
    for (int i = 0; i < CODEX_MILE_COUNT; ++i) {
        if (collected < CODEX_MILES[i].need) {
            *name = CODEX_MILES[i].name;
            return CODEX_MILES[i].need;
        }
    }
    *name = CODEX_MILES[CODEX_MILE_COUNT - 1].name;
    return CODEX_MILES[CODEX_MILE_COUNT - 1].need;
}

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
        /* 最大纪录放在图鉴第二行最显眼处（原来是"最长 …"，措辞改成"最大"，
         * 和玩家说的"同类最大鱼的尺寸"直接对上），并带上累计钓获条数。 */
        snprintf(buf, sizeof(buf), "最大 %d.%d 厘米 · 已钓 %d 条",
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
    /* 性格钩子：收录与否都显示 —— 没见过时它正好是"为什么要去钓它"的由头。 */
    set_text_cached(g_cdx_lore, f->lore);
    /* 体型范围 + 偏好饵料：收录与否都显示 —— 没钓到时它正好是"去哪钓、用什么饵"的提示。 */
    {
        const int lo = f->len_min_mm / 10;
        const int hi = f->len_max_mm / 10;
        const char *bait = first_bait_name(f->bait_mask);
        if (bait) snprintf(buf, sizeof(buf), "体型 %d-%d 厘米 · 爱%s", lo, hi, bait);
        else      snprintf(buf, sizeof(buf), "体型 %d-%d 厘米", lo, hi);
        set_text_cached(g_cdx_size, buf);
    }
    fishing_status_t s2;
    fishing_get_status(&s2);
    /* 里程碑：下一个名号 + 集齐它需要多少种 */
    {
        const char *mname = NULL;
        int need = codex_next_mile(s2.codex_total, &mname);
        if (s2.codex_total >= FISH_SPECIES_COUNT) {
            snprintf(buf, sizeof(buf), "已集齐 %d 种 · %s", FISH_SPECIES_COUNT, mname);
        } else {
            snprintf(buf, sizeof(buf), "集齐 %d 种 · %s", need, mname);
        }
        set_text_cached(g_cdx_mile, buf);
    }
    /* 收集进度 + 百分比 + 累计钓获条数（百分比用整数算，避免浮点） */
    snprintf(buf, sizeof(buf), "收录 %d/%d（%d%%）  钓获 %d",
             s2.codex_total, FISH_SPECIES_COUNT,
             s2.codex_total * 100 / FISH_SPECIES_COUNT,
             s2.total_catch);
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
    /* 三项都走逻辑层的 fishing_cycle_*：策略（尤其是"只在已解锁钓点之间跳"）
     * 在那边有单测钉着，UI 只负责把按下的方向递过去。
     * 钓点换不动时这里不弹提示 —— 光标停在第 3 行时 refresh_menu() 已经把
     * "再钓 N 条解锁 X" 常驻在提示条上，按下的一刻就能对上，不用另设一闪而过的话。 */
    if (g_menu_idx == 1)      (void)fishing_cycle_rod(dir);
    else if (g_menu_idx == 2) (void)fishing_cycle_bait(dir);
    else if (g_menu_idx == 3) (void)fishing_cycle_spot(dir);
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

    /* ---- 先吞掉"长按补发的那个单击" ----
     * iot_button 只在按满 long_press_time(1.5s) 时才走长按分支；我们的长按阈值
     * 是 0.7s，玩家按 0.7~1.5 秒松手时组件会再补发一个 SINGLE_CLICK。长按刚把
     * 菜单开出来，这个迟到的单击就会被当成「OK 确认」落在第 0 项「开始钓鱼」上
     * —— 菜单一闪即关还顺手抛竿（模拟器实测复现过）。
     * 必须放在所有状态分支之前，否则菜单/图鉴会先把它吃掉。 */
    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK && fishing_hold_take_stale_click()) {
        return;
    }

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
        /* 「长按返回」不在这里做：BSP 的 LONG 事件要按满 1500ms，改由
         * ok_poll_long()/handle_long_press() 用按键电平判定（700ms）。 */
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)        fishing_codex_move(-1);
        else if (btn == BSP_BTN_DOWN) fishing_codex_move(1);
        else if (btn == BSP_BTN_OK)   fishing_codex_toggle_info();
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- 菜单 ---- */
    if (st.state == STATE_MENU) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)   { if (g_menu_edit) menu_change_value(-1); else g_menu_idx = (g_menu_idx + 5) % 6; }
        else if (btn == BSP_BTN_DOWN) { if (g_menu_edit) menu_change_value(1); else g_menu_idx = (g_menu_idx + 1) % 6; }
        else if (btn == BSP_BTN_OK) {
            if (g_menu_idx == 0) {
                fishing_exit_menu();
                fishing_cast();
            } else if (g_menu_idx == 4) {
                fishing_codex_open();
            } else if (g_menu_idx == 5) {
                /* 声音：只有开/关两态，单击直接切、立刻存 —— 不进"编辑态"，
                 * 也就没有"进编辑再按上下"这一层多余操作。切换后紧跟的那声
                 * CLICK 就是反馈：刚打开会响、刚关掉自然没声。 */
                fishing_audio_set_muted(!fishing_audio_is_muted());
                nvs_save_sound();
                g_menu_edit = 0;
            } else {
                g_menu_edit = !g_menu_edit;
            }
        }
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- 场景：OK 抛竿 / 提竿（长按开菜单走 handle_long_press） ---- */
    if (ev == BSP_BTN_PRESS && btn == BSP_BTN_OK) {
        if (st.state == STATE_IDLE) { fishing_cast(); fishing_audio_play(SFX_CLICK); }
        else if (st.state == STATE_BITE) {
            fishing_strike();
            fishing_audio_play(SFX_CLICK);
        }
    }
}

/* ===================== 长按 OK：电平驱动（收线之外的所有页面） =====================
 *
 * 为什么另起一套而不是继续等 BSP 的 BSP_BTN_LONG 事件：
 *   按下 OK 的那一瞬间 BSP_BTN_PRESS 先到，IDLE 下 fishing_cast() 已经把状态推成
 *   CASTING；1.5 秒后 LONG_PRESS_START 才来，调用点的 st.state == STATE_IDLE
 *   守卫必然为假 → 长按只会抛竿，菜单永远打不开（社区审核据此打回）。
 *   就算把守卫放开，1.5 秒的等待也远不如自己定阈值跟手。
 *
 * 现在每帧读一次 OK 电平喂给 fishing_hold_sample()（阈值 700ms），到点后按页面分发。
 * 读数和收线共用同一次 bsp_button_read_mv()，不存在第二套按键窗口。
 * BSP 那一侧的 BSP_BTN_LONG 仍然会发，但应用层已经一律忽略（各分支只认
 * CLICK / PRESS），所以不会出现"菜单刚开出来又被 1.5 秒的长按关掉"。 */
static void handle_long_press(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    switch (st.state) {
        /* 钓场三态都开菜单：长按是"从按下开始算"的，按下那一刻顺手抛的竿
         * 到判定时已经跑进 CASTING/WAITING 了，不放行这三态就等于又打不开。 */
        case STATE_IDLE:
        case STATE_CASTING:
        case STATE_WAITING:
        case STATE_BITE:
            fishing_enter_menu();
            g_menu_edit = 0;          /* 避免把上次的编辑态带进新开的菜单 */
            fishing_audio_play(SFX_CLICK);
            break;
        case STATE_MENU:
            fishing_exit_menu();
            g_menu_edit = 0;
            fishing_audio_play(SFX_CLICK);
            break;
        case STATE_CODEX:
        case STATE_CODEX_INFO:
            fishing_codex_close();
            fishing_audio_play(SFX_CLICK);
            break;
        default:
            break;                     /* 收线 / 结算：长按另有含义或不参与 */
    }
}

/* 非收线页面的每帧长按采样。读失败时【不喂】：把"读不出来"当成"松手"会把
 * 已经按住的时间清零，手指明明按着却永远到不了 700ms。 */
static void ok_poll_long(int now) {
    const int s = ok_key_state();
    if (s < 0) return;
    if (fishing_hold_sample(s == 1, now)) handle_long_press();
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

        /* 时间戳先取：输入采样和后面的 tick 必须用同一个 now，
         * 否则长按阈值会被两次采样之间的耗时额外拉长。 */
        int now = (int)now_ms();

        /* 输入采样：每帧读一次 OK 电平 —— 收线的"按住/松开"和所有页面的
         * "长按"共用这次读数（两者互斥，收线期间长按一律不参与）。
         * 顺序必须在 fishing_tick() 之前：反了这一帧用的就是上一帧的按键状态。 */
        fishing_status_t cur;
        fishing_get_status(&cur);
        const bool reeling = (cur.state == STATE_REELING);
        if (reeling) {
            reel_poll_hold();
            /* 收线时按住 OK 是玩法本身，别把"拉鱼那两秒"攒成一次长按。 */
            fishing_hold_reset();
        } else {
            g_adc_live        = false;
            g_adc_fail        = 0;
            g_reel_seen_press = false;
            ok_poll_long(now);
        }

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
    nvs_load_sound();                /* 声音开关（独立 NVS key，默认开） */

    if (bsp_lvgl_lock(1000)) {
        build_ui();
        refresh_ui();
        bsp_lvgl_unlock();
    }
    xTaskCreate(game_task, "fishing", 6144, NULL, 5, NULL);
}
