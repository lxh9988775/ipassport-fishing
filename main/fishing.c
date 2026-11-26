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
LV_FONT_DECLARE(fishing_cjk_12);   /* v5 双行胶囊：16px 字高 20px 会叠字 */

static void use_cjk(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &fishing_cjk_16, LV_PART_MAIN | LV_STATE_DEFAULT);
}
static void use_cjk12(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &fishing_cjk_12, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ===================== 屏幕布局 ===================== */
#define SCR_W 240
#define SCR_H 320
#define HUD_H 22

/* v5 像素硬边色板（效果图 tools/build_fishing_c2_concept.py 同一套） */
#define PANEL_C  lv_color_make(16, 32, 54)
#define PANEL2_C lv_color_make(24, 46, 74)
#define WHITE_C  lv_color_make(232, 240, 248)
#define MUTED_C  lv_color_make(140, 164, 190)
#define GOLD_C   lv_color_make(238, 190, 96)
#define TEAL_C   lv_color_make(108, 190, 174)
#define RED_C    lv_color_make(198, 76, 104)

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
    uint8_t buf[768];   /* v5 schema3 = 20 + 64*11 = 724B，v4 的 512 装不下 */
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
    &fish_ziyu, &fish_heidiao, &fish_haiman,
};
static const lv_image_dsc_t *const SIL_IMG[FISH_SPECIES_COUNT] = {
    &sil_baitiao, &sil_jiyu, &sil_luofei, &sil_lianyu, &sil_bianyu, &sil_caoyu,
    &sil_liyu, &sil_qingyu, &sil_makou, &sil_huangsang, &sil_qiaozui, &sil_luyu,
    &sil_guiyu, &sil_nianyu, &sil_junyu, &sil_ganyu, &sil_xiaohuang, &sil_daiyu,
    &sil_bayu, &sil_bimu, &sil_shiban, &sil_jinqiang, &sil_qiyu, &sil_xiaosha,
    &sil_ziyu, &sil_heidiao, &sil_haiman,
};
/* 图鉴列表 34x32 缩略图：NEAREST 等比居中，禁止拉伸 */
static const lv_image_dsc_t *const THUMB_FISH_IMG[FISH_SPECIES_COUNT] = {
    &thumb_fish_baitiao, &thumb_fish_jiyu, &thumb_fish_luofei, &thumb_fish_lianyu,
    &thumb_fish_bianyu, &thumb_fish_caoyu, &thumb_fish_liyu, &thumb_fish_qingyu,
    &thumb_fish_makou, &thumb_fish_huangsang, &thumb_fish_qiaozui, &thumb_fish_luyu,
    &thumb_fish_guiyu, &thumb_fish_nianyu, &thumb_fish_junyu, &thumb_fish_ganyu,
    &thumb_fish_xiaohuang, &thumb_fish_daiyu, &thumb_fish_bayu, &thumb_fish_bimu,
    &thumb_fish_shiban, &thumb_fish_jinqiang, &thumb_fish_qiyu, &thumb_fish_xiaosha,
    &thumb_fish_ziyu, &thumb_fish_heidiao, &thumb_fish_haiman,
};
static const lv_image_dsc_t *const THUMB_SIL_IMG[FISH_SPECIES_COUNT] = {
    &thumb_sil_baitiao, &thumb_sil_jiyu, &thumb_sil_luofei, &thumb_sil_lianyu,
    &thumb_sil_bianyu, &thumb_sil_caoyu, &thumb_sil_liyu, &thumb_sil_qingyu,
    &thumb_sil_makou, &thumb_sil_huangsang, &thumb_sil_qiaozui, &thumb_sil_luyu,
    &thumb_sil_guiyu, &thumb_sil_nianyu, &thumb_sil_junyu, &thumb_sil_ganyu,
    &thumb_sil_xiaohuang, &thumb_sil_daiyu, &thumb_sil_bayu, &thumb_sil_bimu,
    &thumb_sil_shiban, &thumb_sil_jinqiang, &thumb_sil_qiyu, &thumb_sil_xiaosha,
    &thumb_sil_ziyu, &thumb_sil_heidiao, &thumb_sil_haiman,
};
static const lv_image_dsc_t *const BG_IMG[FISH_SPOT_COUNT] = {
    &bg_pond, &bg_river, &bg_sea, &bg_estuary, &bg_pier,
};
static const lv_image_dsc_t *const THUMB_BG_IMG[FISH_SPOT_COUNT] = {
    &thumb_pond, &thumb_river, &thumb_sea, &thumb_estuary, &thumb_pier,
};
/* 钓场里真正会画出来的道具：手上的竿 + 挂在钩上的饵。
 * 这两组图一直在 sprites.h 里，但 v2 的界面从来没引用过 —— 换竿换饵只有
 * 菜单文字变、画面一动不动，玩家会以为没生效。 */
/* 巨物竿图标待美术确认后补 rod_heavy；当前先用 rod_sea 占位，保证 4 支竿都有图、可编译。 */
static const lv_image_dsc_t *const ROD_IMG[FISH_ROD_COUNT]   = { &rod_hand, &rod_lure, &rod_sea, &rod_sea };
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
static lv_obj_t *g_lbl_spot    = NULL;   /* 钓场名（场景左上角双行胶囊第 1 行） */
static lv_obj_t *g_lbl_subspot = NULL;   /* 标点 + 主出鱼钩子（第 2 行） */
static lv_obj_t *g_spot_cap    = NULL;   /* 双行胶囊底 */
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
static lv_obj_t *g_lbl_gear    = NULL;

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
static lv_obj_t *g_cdx_hist    = NULL;   /* ③履历：首次第 N 日 · 首捕标点（已收录才显示） */
static lv_obj_t *g_cdx_size    = NULL;   /* 体长范围 + 偏好饵料（收录与否都显示，是"怎么钓"的提示） */
static lv_obj_t *g_cdx_desc    = NULL;
static lv_obj_t *g_cdx_lore    = NULL;   /* 性格钩子：这鱼什么脾气（新增，纯文案） */
static lv_obj_t *g_cdx_mile    = NULL;   /* 收集里程碑：下一个里程碑叫什么、还差几种 */
static lv_obj_t *g_cdx_prog    = NULL;

/* v5 C+2 新页面：选钓场 / 选标点 / 图鉴列表 */
static lv_obj_t *g_spot_pick   = NULL;
static lv_obj_t *g_spot_rows[FISH_SPOT_COUNT];
static lv_obj_t *g_spot_thumbs[FISH_SPOT_COUNT];
static lv_obj_t *g_spot_names[FISH_SPOT_COUNT];
static lv_obj_t *g_spot_subs[FISH_SPOT_COUNT];
static lv_obj_t *g_spot_hooks[FISH_SPOT_COUNT];
static lv_obj_t *g_spot_new[FISH_SPOT_COUNT];
static lv_obj_t *g_spot_selbar = NULL;
static int g_spot_pick_idx = 0;   /* 选钓场页光标（含未解锁场，只查看不进入） */

static lv_obj_t *g_sub_pick    = NULL;
static lv_obj_t *g_sub_title   = NULL;
static lv_obj_t *g_sub_cards[2];
static lv_obj_t *g_sub_names[2];
static lv_obj_t *g_sub_fish[2];
static lv_obj_t *g_sub_hooks[2];
static lv_obj_t *g_sub_marks[2];

#define CODEX_LIST_ROWS 4
static lv_obj_t *g_cdx_list    = NULL;
static lv_obj_t *g_cdx_l_img[CODEX_LIST_ROWS];
static lv_obj_t *g_cdx_l_name[CODEX_LIST_ROWS];
static lv_obj_t *g_cdx_l_where[CODEX_LIST_ROWS];
static lv_obj_t *g_cdx_l_when[CODEX_LIST_ROWS];
static lv_obj_t *g_cdx_l_size[CODEX_LIST_ROWS];

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

/* 像素硬边面板：外 1px 深色 + 内亮边，直角、零内边距。
 * lv_obj_create() 默认带约 13px padding，不清零所有子元素都会右下偏 —— 这里统一处理。 */
static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t fill) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_color(p, fill, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, lv_color_make(120, 156, 190), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
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

/* 标点主出鱼提示：从 gold_a 里取前两条鱼名，拼「泥滩 · 鲈鱼/鲻鱼」。
 * 鱼种数、标点鱼掩码都从逻辑层接口取，不在 UI 写死 24/27/10。 */
static void subspot_hint(int ss, char *buf, int cap) {
    buf[0] = '\0';
    const subspot_cfg_t *info = fishing_subspot_info(ss);
    if (!info || cap <= 0) return;
    const char *names[2] = { NULL, NULL };
    int n = 0;
    for (int i = 0; i < FISH_SPECIES_COUNT && n < 2; ++i) {
        if ((info->gold_a & (1u << (unsigned)i)) == 0) continue;
        const fish_species_t *f = fishing_species_info(i);
        if (f) names[n++] = f->name;
    }
    if (n == 0) snprintf(buf, (size_t)cap, "普通水域");
    else if (n == 1) snprintf(buf, (size_t)cap, "%s", names[0]);
    else snprintf(buf, (size_t)cap, "%s/%s", names[0], names[1]);
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

    /* v5 主界面双行胶囊：绝对 (10,34)~(150,72)，相对场景面板 (10,12)。
     * 第 1 行钓场，第 2 行「标点 · 主出鱼」，分隔线 y=68（相对 46）。
     * 环境标签下移到 y=74，避免压住胶囊 —— v4 的鱼讯一条都不能丢。 */
    g_spot_cap = make_panel(g_scene, 10, 12, 140, 38, lv_color_make(16, 32, 54));
    g_lbl_spot = make_label(g_spot_cap, "静水塘", 8, 3, lv_color_white());
    g_lbl_subspot = make_label(g_spot_cap, "芦苇荡 · 白条/鲫鱼", 8, 19,
                               lv_color_make(108, 190, 174));
    use_cjk12(g_lbl_spot);
    use_cjk12(g_lbl_subspot);
    lv_obj_set_width(g_lbl_subspot, 128);
    lv_label_set_long_mode(g_lbl_subspot, LV_LABEL_LONG_DOT);
    {
        lv_obj_t *sep = lv_obj_create(g_spot_cap);
        lv_obj_set_size(sep, 124, 1);
        lv_obj_set_pos(sep, 8, 34);
        lv_obj_set_style_bg_color(sep, lv_color_make(46, 72, 100), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_pad_all(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 环境标签：胶囊下方「时段 · 天气 / 当日鱼讯」两行，宽 140 不与右侧道具冲突。 */
    g_lbl_env = make_label(g_scene, "", 10, 52, lv_color_white());
    lv_obj_set_width(g_lbl_env, 140);
    lv_label_set_long_mode(g_lbl_env, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_lbl_env, 2, 0);
    lv_obj_set_style_bg_color(g_lbl_env, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_lbl_env, 110, 0);
    lv_obj_set_style_pad_all(g_lbl_env, 3, 0);

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
    /* 竿速 / 鱼的节奏单独一行，不挤进 g_lbl_reel（三行会顶到右边轨道） */
    g_lbl_gear = make_label(g_reel, "慢竿 巡游", 60, 160, MUTED_C);

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

    /* 选中条：跟着菜单行走（菜单行坐标见下，逐条展开给布局检查器还原）。 */
    g_menu_selbar = lv_obj_create(g_menu);
    lv_obj_set_size(g_menu_selbar, SCR_W - 12, 30);
    lv_obj_set_pos(g_menu_selbar, 6, 10);
    lv_obj_set_style_bg_color(g_menu_selbar, lv_color_make(153, 0, 58), 0); /* 高亮红 */
    lv_obj_set_style_bg_opa(g_menu_selbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_menu_selbar, 0, 0);
    lv_obj_set_style_radius(g_menu_selbar, 4, 0);
    lv_obj_clear_flag(g_menu_selbar, LV_OBJ_FLAG_SCROLLABLE);

    /* 6 行铺满整块面板（原来 74+i*30 全挤在上半部，下半屏一大片留白）。
     * 起始 y=14、行距 42：第 1 行 y=14 … 第 6 行 y=224（行高 20），
     * 面板高 274，末行底 244，底部留 30px 不压提示条。
     * 逐条展开而非 for(i)，布局检查器才能解析出真实坐标、检出不出了屏。 */
    lv_obj_t *menu_row0 = make_label(g_menu, "开始钓鱼", 14, 17, lv_color_white());
    lv_obj_t *menu_row1 = make_label(g_menu, "鱼竿   手竿", 14, 59, lv_color_white());
    lv_obj_t *menu_row2 = make_label(g_menu, "饵料   蚯蚓", 14, 101, lv_color_white());
    lv_obj_t *menu_row3 = make_label(g_menu, "钓点   静水塘", 14, 143, lv_color_white());
    lv_obj_t *menu_row4 = make_label(g_menu, "图鉴   0/27", 14, 185, lv_color_white());
    lv_obj_t *menu_row5 = make_label(g_menu, "声音   开", 14, 227, lv_color_white());
    g_menu_rows[0] = menu_row0; g_menu_rows[1] = menu_row1;
    g_menu_rows[2] = menu_row2; g_menu_rows[3] = menu_row3;
    g_menu_rows[4] = menu_row4; g_menu_rows[5] = menu_row5;

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

    /* v4 图鉴详情页重排：新增一行「钓鱼履历」，同时压缩行距，保证末行不溢出。
     * 面板高 = 320-22-24 = 274；末行 prog @242 + 行高 20 = 262 < 274，留 12px 安全边距。
     * desc/lore 行距从 4 压到 2：最坏 2 行各占 20+2+20=42px，正好前后相接不叠字。 */
    g_cdx_img  = lv_img_create(g_codex);
    lv_obj_set_pos(g_cdx_img, 92, 6);
    g_cdx_name = make_label(g_codex, "", 8, 44, lv_color_white());
    /* 稀有度/完美标记上移到名称行，meta 只留体长+钓数：
     * 原来「传说 · 最大 888.8 厘米 · 已钓 8888 条 · 完美✓」要 345px，屏只有 240px。
     * DOT 模式兜底 —— 万一以后文案又变长，末尾显示省略号，绝不出屏、绝不折行。 */
    g_cdx_meta = make_label(g_codex, "", 8, 66, lv_color_make(255, 208, 90));  /* 金色：突出"最大纪录" */
    lv_obj_set_width(g_cdx_meta, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_meta, LV_LABEL_LONG_DOT);
    /* ③钓鱼履历：已收录才显示"首次第 N 日 · 首捕标点"；金色沿用最大纪录行。 */
    g_cdx_hist = make_label(g_codex, "", 8, 88, lv_color_make(255, 208, 90));
    lv_obj_set_width(g_cdx_hist, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_hist, LV_LABEL_LONG_DOT);
    /* 体型范围 + 偏好饵料：这是"这条鱼能长多大、爱吃什么"的攻略信息，
     * 收录与否都显示 —— 没钓到之前它正好告诉玩家该去哪、用什么饵。 */
    g_cdx_size = make_label(g_codex, "", 8, 110, lv_color_make(150, 215, 255));
    lv_obj_set_width(g_cdx_size, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_size, LV_LABEL_LONG_DOT);
    g_cdx_desc = make_label(g_codex, "", 8, 132, lv_color_white());
    lv_obj_set_width(g_cdx_desc, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_cdx_desc, 2, 0);
    /* 性格钩子（lore）：这鱼什么脾气、有没有什么故事。
     * 最坏 2 行 20+2+20=42px，132+42=174 起排不压 176 的 lore。 */
    g_cdx_lore = make_label(g_codex, "", 8, 176, lv_color_make(255, 236, 205));
    lv_obj_set_width(g_cdx_lore, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_lore, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(g_cdx_lore, 2, 0);
    /* 里程碑：纯提示，不奖励任何数值。只说"下一个里程碑叫什么、还差几种"。 */
    g_cdx_mile = make_label(g_codex, "", 8, 220, lv_color_make(150, 230, 175));
    lv_obj_set_width(g_cdx_mile, SCR_W - 16);
    lv_label_set_long_mode(g_cdx_mile, LV_LABEL_LONG_DOT);
    g_cdx_prog = make_label(g_codex, "", 8, 242, lv_color_make(190, 210, 240));

    /* ================= v5 C+2：选钓场（5 行） ================= */
    g_spot_pick = make_panel(scr, 0, HUD_H, SCR_W, SCR_H - HUD_H,
                             lv_color_make(8, 16, 30));
    lv_obj_set_style_pad_all(g_spot_pick, 0, 0);
    lv_obj_add_flag(g_spot_pick, LV_OBJ_FLAG_HIDDEN);
    make_label(g_spot_pick, "选钓场", 14, 10, lv_color_white());
    make_label(g_spot_pick, "天色为首，去哪下竿", 14, 30, lv_color_make(140, 164, 190));
    /* 五行逐条展开（不用循环）：布局检查器按源码常量还原坐标，
     * 数组循环里的 i 求不出值，会漏检选钓场页的出屏/压盖。 */
    {
        lv_obj_t *spot_row0 = make_panel(g_spot_pick, 10, 46, 220, 42, PANEL_C);
        g_spot_rows[0] = spot_row0;
        g_spot_thumbs[0] = lv_img_create(spot_row0); lv_img_set_src(g_spot_thumbs[0], THUMB_BG_IMG[0]); lv_obj_set_pos(g_spot_thumbs[0], 6, 5);
        g_spot_names[0] = make_label(spot_row0, "", 48, 4, WHITE_C);
        g_spot_subs[0]  = make_label(spot_row0, "2 处标点", 48, 20, TEAL_C);
        g_spot_hooks[0] = make_label(spot_row0, "", 48, 31, MUTED_C);
        g_spot_new[0]   = make_label(spot_row0, "", 192, 20, GOLD_C);
        lv_obj_t *spot_row1 = make_panel(g_spot_pick, 10, 93, 220, 42, PANEL_C);
        g_spot_rows[1] = spot_row1;
        g_spot_thumbs[1] = lv_img_create(spot_row1); lv_img_set_src(g_spot_thumbs[1], THUMB_BG_IMG[1]); lv_obj_set_pos(g_spot_thumbs[1], 6, 5);
        g_spot_names[1] = make_label(spot_row1, "", 48, 4, WHITE_C);
        g_spot_subs[1]  = make_label(spot_row1, "2 处标点", 48, 20, TEAL_C);
        g_spot_hooks[1] = make_label(spot_row1, "", 48, 31, MUTED_C);
        g_spot_new[1]   = make_label(spot_row1, "", 192, 20, GOLD_C);
        lv_obj_t *spot_row2 = make_panel(g_spot_pick, 10, 140, 220, 42, PANEL_C);
        g_spot_rows[2] = spot_row2;
        g_spot_thumbs[2] = lv_img_create(spot_row2); lv_img_set_src(g_spot_thumbs[2], THUMB_BG_IMG[2]); lv_obj_set_pos(g_spot_thumbs[2], 6, 5);
        g_spot_names[2] = make_label(spot_row2, "", 48, 4, WHITE_C);
        g_spot_subs[2]  = make_label(spot_row2, "2 处标点", 48, 20, TEAL_C);
        g_spot_hooks[2] = make_label(spot_row2, "", 48, 31, MUTED_C);
        g_spot_new[2]   = make_label(spot_row2, "", 192, 20, GOLD_C);
        lv_obj_t *spot_row3 = make_panel(g_spot_pick, 10, 187, 220, 42, PANEL_C);
        g_spot_rows[3] = spot_row3;
        g_spot_thumbs[3] = lv_img_create(spot_row3); lv_img_set_src(g_spot_thumbs[3], THUMB_BG_IMG[3]); lv_obj_set_pos(g_spot_thumbs[3], 6, 5);
        g_spot_names[3] = make_label(spot_row3, "", 48, 4, WHITE_C);
        g_spot_subs[3]  = make_label(spot_row3, "2 处标点", 48, 20, TEAL_C);
        g_spot_hooks[3] = make_label(spot_row3, "", 48, 31, MUTED_C);
        g_spot_new[3]   = make_label(spot_row3, "", 192, 20, GOLD_C);
        lv_obj_t *spot_row4 = make_panel(g_spot_pick, 10, 234, 220, 42, PANEL_C);
        g_spot_rows[4] = spot_row4;
        g_spot_thumbs[4] = lv_img_create(spot_row4); lv_img_set_src(g_spot_thumbs[4], THUMB_BG_IMG[4]); lv_obj_set_pos(g_spot_thumbs[4], 6, 5);
        g_spot_names[4] = make_label(spot_row4, "", 48, 4, WHITE_C);
        g_spot_subs[4]  = make_label(spot_row4, "2 处标点", 48, 20, TEAL_C);
        g_spot_hooks[4] = make_label(spot_row4, "", 48, 31, MUTED_C);
        g_spot_new[4]   = make_label(spot_row4, "", 192, 20, GOLD_C);
    }
    g_spot_selbar = lv_obj_create(g_spot_pick);
    lv_obj_set_size(g_spot_selbar, 3, 42);
    lv_obj_set_pos(g_spot_selbar, 10, 46);
    lv_obj_set_style_bg_color(g_spot_selbar, lv_color_make(198, 76, 104), 0);
    lv_obj_set_style_bg_opa(g_spot_selbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_spot_selbar, 0, 0);
    lv_obj_set_style_pad_all(g_spot_selbar, 0, 0);
    lv_obj_clear_flag(g_spot_selbar, LV_OBJ_FLAG_SCROLLABLE);

    /* ================= v5 C+2：选标点（2 张大卡 + 提示卡） ================= */
    g_sub_pick = make_panel(scr, 0, HUD_H, SCR_W, SCR_H - HUD_H,
                            lv_color_make(6, 14, 28));
    lv_obj_set_style_pad_all(g_sub_pick, 0, 0);
    lv_obj_add_flag(g_sub_pick, LV_OBJ_FLAG_HIDDEN);
    {
        /* 预压暗背景图：RGB565 不透明，绝不用运行时半透明全屏层 */
        lv_obj_t *dark = lv_img_create(g_sub_pick);
        lv_img_set_src(dark, &bg_estuary_dark);
        lv_obj_set_pos(dark, 0, 0);   /* 先建图片沉底，后建文字自然盖在上面 */
    }
    g_sub_title = make_label(g_sub_pick, "选标点", 14, 10, lv_color_white());
    make_label(g_sub_pick, "同一片水，不同位置不一样的鱼", 14, 30,
               lv_color_make(108, 190, 174));
    {
        lv_obj_t *sub_card0 = make_panel(g_sub_pick, 14, 50, 212, 74, PANEL_C);
        g_sub_cards[0] = sub_card0;
        g_sub_names[0] = make_label(sub_card0, "", 12, 8, WHITE_C);
        g_sub_fish[0]  = make_label(sub_card0, "", 12, 30, GOLD_C);
        g_sub_hooks[0] = make_label(sub_card0, "", 12, 50, MUTED_C);
        g_sub_marks[0] = make_label(sub_card0, "", 172, 8, RED_C);
        lv_obj_t *sub_card1 = make_panel(g_sub_pick, 14, 134, 212, 74, PANEL_C);
        g_sub_cards[1] = sub_card1;
        g_sub_names[1] = make_label(sub_card1, "", 12, 8, WHITE_C);
        g_sub_fish[1]  = make_label(sub_card1, "", 12, 30, GOLD_C);
        g_sub_hooks[1] = make_label(sub_card1, "", 12, 50, MUTED_C);
        g_sub_marks[1] = make_label(sub_card1, "", 172, 8, RED_C);
    }
    {
        lv_obj_t *note = make_panel(g_sub_pick, 14, 224, 212, 40,
                                    lv_color_make(16, 32, 54));
        make_label(note, "标点只是提高概率，不锁死", 8, 4, lv_color_make(108, 190, 174));
        make_label(note, "换个位置，稀客也会来，只是少", 8, 22, lv_color_make(140, 164, 190));
    }

    /* ================= v5 C+2：图鉴列表（4 行） ================= */
    g_cdx_list = make_panel(scr, 0, HUD_H, SCR_W, SCR_H - HUD_H,
                            lv_color_make(8, 16, 30));
    lv_obj_set_style_pad_all(g_cdx_list, 0, 0);
    lv_obj_add_flag(g_cdx_list, LV_OBJ_FLAG_HIDDEN);
    make_label(g_cdx_list, "图鉴 · 钓获", 14, 10, lv_color_white());
    make_label(g_cdx_list, "每一条都记着是在哪钓的", 14, 30, lv_color_make(140, 164, 190));
    {
        lv_obj_t *cdx_row0 = make_panel(g_cdx_list, 12, 46, 216, 52, PANEL_C);
        g_cdx_l_img[0] = lv_img_create(cdx_row0); lv_img_set_src(g_cdx_l_img[0], &thumb_fish_baitiao); lv_obj_set_pos(g_cdx_l_img[0], 6, 8);
        g_cdx_l_name[0]  = make_label(cdx_row0, "", 48, 4, WHITE_C);
        g_cdx_l_where[0] = make_label(cdx_row0, "", 48, 21, GOLD_C);
        g_cdx_l_when[0]  = make_label(cdx_row0, "", 48, 37, MUTED_C);
        g_cdx_l_size[0]  = make_label(cdx_row0, "", 184, 21, TEAL_C);
        lv_obj_t *cdx_row1 = make_panel(g_cdx_list, 12, 104, 216, 52, PANEL_C);
        g_cdx_l_img[1] = lv_img_create(cdx_row1); lv_img_set_src(g_cdx_l_img[1], &thumb_fish_baitiao); lv_obj_set_pos(g_cdx_l_img[1], 6, 8);
        g_cdx_l_name[1]  = make_label(cdx_row1, "", 48, 4, WHITE_C);
        g_cdx_l_where[1] = make_label(cdx_row1, "", 48, 21, GOLD_C);
        g_cdx_l_when[1]  = make_label(cdx_row1, "", 48, 37, MUTED_C);
        g_cdx_l_size[1]  = make_label(cdx_row1, "", 184, 21, TEAL_C);
        lv_obj_t *cdx_row2 = make_panel(g_cdx_list, 12, 162, 216, 52, PANEL_C);
        g_cdx_l_img[2] = lv_img_create(cdx_row2); lv_img_set_src(g_cdx_l_img[2], &thumb_fish_baitiao); lv_obj_set_pos(g_cdx_l_img[2], 6, 8);
        g_cdx_l_name[2]  = make_label(cdx_row2, "", 48, 4, WHITE_C);
        g_cdx_l_where[2] = make_label(cdx_row2, "", 48, 21, GOLD_C);
        g_cdx_l_when[2]  = make_label(cdx_row2, "", 48, 37, MUTED_C);
        g_cdx_l_size[2]  = make_label(cdx_row2, "", 184, 21, TEAL_C);
        lv_obj_t *cdx_row3 = make_panel(g_cdx_list, 12, 220, 216, 52, PANEL_C);
        g_cdx_l_img[3] = lv_img_create(cdx_row3); lv_img_set_src(g_cdx_l_img[3], &thumb_fish_baitiao); lv_obj_set_pos(g_cdx_l_img[3], 6, 8);
        g_cdx_l_name[3]  = make_label(cdx_row3, "", 48, 4, WHITE_C);
        g_cdx_l_where[3] = make_label(cdx_row3, "", 48, 21, GOLD_C);
        g_cdx_l_when[3]  = make_label(cdx_row3, "", 48, 37, MUTED_C);
        g_cdx_l_size[3]  = make_label(cdx_row3, "", 184, 21, TEAL_C);
    }
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
    lv_obj_add_flag(g_spot_pick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_sub_pick,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_cdx_list,  LV_OBJ_FLAG_HIDDEN);
    if (which == 0) lv_obj_clear_flag(g_scene,     LV_OBJ_FLAG_HIDDEN);
    if (which == 1) lv_obj_clear_flag(g_reel,      LV_OBJ_FLAG_HIDDEN);
    if (which == 2) lv_obj_clear_flag(g_result,    LV_OBJ_FLAG_HIDDEN);
    if (which == 3) lv_obj_clear_flag(g_menu,      LV_OBJ_FLAG_HIDDEN);
    if (which == 4) lv_obj_clear_flag(g_codex,     LV_OBJ_FLAG_HIDDEN);
    if (which == 5) lv_obj_clear_flag(g_spot_pick, LV_OBJ_FLAG_HIDDEN);
    if (which == 6) lv_obj_clear_flag(g_sub_pick,  LV_OBJ_FLAG_HIDDEN);
    if (which == 7) lv_obj_clear_flag(g_cdx_list,  LV_OBJ_FLAG_HIDDEN);

    const char *hint = NULL;
    switch (which) {
        case 1: hint = "↓快竿追冲刺 ↑慢竿"; break;   /* 收线：明确冲刺要切快竿 */
        case 4: hint = "上下翻·OK详情·长按返回";  break;   /* 图鉴详情 */
        case 5: hint = "上下选钓场 · OK进标点"; break;
        case 6: hint = "左右选标点 · OK下竿"; break;
        case 7: hint = "上下翻·OK详情·长按返回"; break;
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

/* v4 每日鱼讯：给玩家一个"今天下竿"的由头，不逼完成、不惩罚、不加菜单、不碰存档。
 * 按钓鱼日序号做整数散列选一句 —— 同一天恒定，跨天轮换，无随机状态、不会每帧跳。 */
static const char *const FISH_NEWS[] = {
    "塘里白条成群，今天好开口",
    "水下有大家伙翻花，线放长些",
    "风往东吹，鱼靠西岸",
    "夜猫子们要出来了",
    "水清鱼也精，别急着提竿",
    "细雨落水面，鱼口正旺",
    "日头毒，鱼都躲深水了",
    "岸边虫多，上层鱼在等着",
    "闷了一夜，天亮准有动静",
    "水色微浑，正是钓底的好时候",
};
#define FISH_NEWS_COUNT ((int)(sizeof(FISH_NEWS) / sizeof(FISH_NEWS[0])))

static const char *fish_news_of(int day) {
    uint32_t h = (uint32_t)day * 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return FISH_NEWS[(unsigned)h % FISH_NEWS_COUNT];
}

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
    /* v4：把时段/天气/钓鱼日喂给逻辑层。时段天气影响抽鱼概率，钓鱼日用于
     * ③钓鱼履历记首捕日。每帧都调是安全的——logic 只存三个整数，不碰任何 LVGL 对象。 */
    fishing_set_env((int)p, (int)w);
    fishing_set_day(env_day(now));
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
    /* 两行：第 1 行时段·天气，第 2 行当日鱼讯。鱼讯按 env_day 选句，同一钓鱼日恒定。
     * 文本整体交给 set_text_cached 门闸：同一天内 buf 完全一致 → 一个 LVGL 调用都不做；
     * 时段切换（每小时）或跨天时 buf 才变 → 各重绘一次，符合预期，不逐帧重绘。 */
    char buf[80];
    snprintf(buf, sizeof(buf), "%s · %s\n%s", env_period_name(p), env_weather_name(w),
             fish_news_of(env_day(now)));
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

    /* 双行胶囊：钓场 + 标点/主出鱼。两者都走文本缓存，标点没变时零 LVGL 调用。 */
    set_text_cached(g_lbl_spot, fishing_spot_name(st->spot));
    {
        char hint[64];
        char cap[128];
        subspot_hint(fishing_subspot(), hint, (int)sizeof(hint));
        if (fishing_subspot() >= 0)
            snprintf(cap, sizeof(cap), "%s · %s", fishing_subspot_name(fishing_subspot()), hint);
        else
            snprintf(cap, sizeof(cap), "%s", hint);
        set_text_cached(g_lbl_subspot, cap);
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
static int s_mark_color = -1;
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

    /* 鱼的节奏只在翻转时改色：黄=巡游，红=蓄力预警，橙=冲刺。
     * 每帧无条件 set_style 会把整块轨道重绘，是卡死铁律里的头号杀手。 */
    int color_key = st->reel_phase;
    if (color_key > 3) color_key = 0;
    if (color_key != s_mark_color) {
        s_mark_color = color_key;
        lv_color_t c = (color_key == 1) ? lv_color_make(255, 96, 96)
                     : (color_key == 2) ? lv_color_make(255, 168, 56)
                     : (color_key == 3) ? lv_color_make(150, 170, 190)
                     :                    lv_color_make(255, 200, 70);
        lv_obj_set_style_bg_color(g_fishmark, c, 0);
    }

    const fish_species_t *f = fishing_species_info(st->cur_species);
    static const char *const PHASE_LINE[2][4] = {
        {"慢竿 巡游", "慢竿 蓄力", "慢竿 冲刺", "慢竿 歇息"},
        {"快竿 巡游", "快竿 蓄力", "快竿 冲刺", "快竿 歇息"},
    };
    int phase = st->reel_phase;
    if (phase < 0 || phase > 3) phase = 0;
    snprintf(buf, sizeof(buf), "收线 %d%%\n%s", st->reel_progress / 10,
             f ? f->name : "");
    set_text_cached(g_lbl_reel, buf);
    set_text_cached(g_lbl_gear, PHASE_LINE[st->reel_gear ? 1 : 0][phase]);
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
    lv_obj_set_pos(g_menu_selbar, 6, 10 + g_menu_idx * 42);   /* 铺满布局：选中条跟着行走 */

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
         * 和玩家说的"同类最大鱼的尺寸"直接对上）。累计钓获条数在底部进度行已有，
         * 这一行腾出的位置给最大重量，v4 的履历信息不因加标点而丢失。 */
        {
            int bl = fishing_codex_best_len(idx);
            int wg = fishing_codex_best_wgt(idx);
            if (wg > 0) snprintf(buf, sizeof(buf), "最大 %d.%d 厘米 · %d.%d千克",
                                 bl / 10, bl % 10, wg / 1000, (wg % 1000) / 100);
            else        snprintf(buf, sizeof(buf), "最大 %d.%d 厘米 · 重量未记录",
                                 bl / 10, bl % 10);
            set_text_cached(g_cdx_meta, buf);
        }
        /* ③履历：首次钓鱼日 + 首捕标点。0 值（老存档迁移/未设置）显示措辞，
         * 绝不让"第 0 天"这种破绽出现在屏上。标点只显示玩家自己钓到时的位置，
         * 未收录行不剧透 —— 那一行只给主栖息地。 */
        {
            int fd = fishing_codex_first_day(idx);
            int ss = fishing_codex_subspot(idx);
            char day_buf[24];   /* "第%d日"最坏 17 字节，24 留足 */
            if (fd > 0) snprintf(day_buf, sizeof(day_buf), "第%d日", fd);
            else        snprintf(day_buf, sizeof(day_buf), "很久以前");
            if (ss >= 0) snprintf(buf, sizeof(buf), "首次 %s · %s",
                                  day_buf, fishing_subspot_name(ss));
            else         snprintf(buf, sizeof(buf), "首次 %s · 标点未知", day_buf);
            set_text_cached(g_cdx_hist, buf);
        }
        set_text_cached(g_cdx_desc, f->desc);
    } else {
        set_text_cached(g_cdx_hist, "");   /* 未收录：履历行留空，不剧透 */
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

/* ===================== v5 C+2：三个新页面刷新 ===================== */
static int subspot_count_of(int spot) {
    int n = 0;
    for (int i = 0; i < FISH_SUBSPOT_COUNT; ++i) {
        const subspot_cfg_t *ss = fishing_subspot_info(i);
        if (ss && ss->spot == (unsigned char)spot) n++;
    }
    return n;
}

static const char *period_short(unsigned mask) {
    if (mask & FISH_PERF_MORNING) return "晨";
    if (mask & FISH_PERF_NOON)    return "午";
    if (mask & FISH_PERF_DUSK)    return "昏";
    if (mask & FISH_PERF_NIGHT)   return "夜";
    return "全天";
}

static void refresh_spot_pick(void) {
    static const char *const HOOKS[FISH_SPOT_COUNT] = {
        "晨晴 · 白条成群", "昏雨 · 鳜鱼藏石缝", "夜晴 · 金枪旗鱼",
        "黄昏 · 鲈鱼洄游", "夜晴 · 灯光诱鱼",
    };
    static int drawn_sel = -1;
    static int drawn_lock_mask = -1;   /* 解锁状态影子：只在变化时动文字颜色，避免每帧 invalidate */
    char buf[64];
    int lock_mask = 0;
    for (int i = 0; i < FISH_SPOT_COUNT; ++i) {
        const bool unlocked = fishing_spot_unlocked((spot_t)i);
        if (!unlocked) lock_mask |= (1 << i);
        snprintf(buf, sizeof(buf), "%d 处标点", subspot_count_of(i));
        set_text_cached(g_spot_subs[i], buf);
        set_text_cached(g_spot_names[i], fishing_spot_name((spot_t)i));
        if (unlocked) {
            /* 已解锁：保留原来的鱼情提示与 NEW 标 */
            set_text_cached(g_spot_hooks[i], HOOKS[i]);
            set_text_cached(g_spot_new[i], (i >= 3 && i < FISH_SPOT_COUNT) ? "NEW" : "");
        } else {
            /* 未解锁：把鱼情换成明确门槛，玩家一眼看懂"不是坏了，是还没开" */
            int left = fishing_spot_unlock_left((spot_t)i);
            snprintf(buf, sizeof(buf), "未解锁 · 还差 %d 条", left);
            set_text_cached(g_spot_hooks[i], buf);
            set_text_cached(g_spot_new[i], "锁");
        }
    }
    /* 解锁状态翻转才改色：未解锁整行文字压暗、缩略图调暗，已解锁恢复。 */
    if (drawn_lock_mask != lock_mask) {
        drawn_lock_mask = lock_mask;
        for (int i = 0; i < FISH_SPOT_COUNT; ++i) {
            bool locked = (lock_mask & (1 << i)) != 0;
            lv_obj_set_style_text_color(g_spot_names[i],
                locked ? lv_color_make(120, 132, 148) : WHITE_C, 0);
            lv_obj_set_style_text_color(g_spot_subs[i],
                locked ? lv_color_make(96, 108, 124) : TEAL_C, 0);
            lv_obj_set_style_text_color(g_spot_hooks[i],
                locked ? lv_color_make(214, 158, 92) : MUTED_C, 0); /* 门槛用暖色更醒目 */
            lv_obj_set_style_img_opa(g_spot_thumbs[i], locked ? 120 : 255, 0);
        }
    }
    /* 选中态只在光标真的移动时碰样式（周期性重设样式=整页重绘） */
    if (drawn_sel != g_spot_pick_idx) {
        if (g_spot_pick_idx < 0) g_spot_pick_idx = 0;
        if (g_spot_pick_idx >= FISH_SPOT_COUNT) g_spot_pick_idx = FISH_SPOT_COUNT - 1;
        for (int i = 0; i < FISH_SPOT_COUNT; ++i)
            lv_obj_set_style_bg_color(g_spot_rows[i],
                (i == g_spot_pick_idx) ? lv_color_make(24, 46, 74) : lv_color_make(16, 32, 54), 0);
        lv_obj_set_pos(g_spot_selbar, 10, 46 + g_spot_pick_idx * 47);
        drawn_sel = g_spot_pick_idx;
    }
}

static void refresh_sub_pick(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s · 选标点", fishing_spot_name(st.spot));
    set_text_cached(g_sub_title, buf);

    int ids[2] = { -1, -1 };
    int n = 0;
    for (int i = 0; i < FISH_SUBSPOT_COUNT && n < 2; ++i) {
        const subspot_cfg_t *ss = fishing_subspot_info(i);
        if (ss && ss->spot == (unsigned char)st.spot) ids[n++] = i;
    }
    static int drawn_sel = -999;
    const int cur = fishing_subspot();
    for (int i = 0; i < 2; ++i) {
        if (ids[i] < 0) {
            set_text_cached(g_sub_names[i], "");
            set_text_cached(g_sub_fish[i], "");
            set_text_cached(g_sub_hooks[i], "");
            set_text_cached(g_sub_marks[i], "");
            continue;
        }
        const subspot_cfg_t *ss = fishing_subspot_info(ids[i]);
        char fish_hint[64];
        subspot_hint(ids[i], fish_hint, (int)sizeof(fish_hint));
        set_text_cached(g_sub_names[i], ss->name);
        snprintf(buf, sizeof(buf), "出鱼：%s", fish_hint);
        set_text_cached(g_sub_fish[i], buf);
        set_text_cached(g_sub_hooks[i], ss->hook);
        set_text_cached(g_sub_marks[i], (ids[i] == cur) ? "选中" : "");
    }
    if (drawn_sel != cur) {
        for (int i = 0; i < 2; ++i) {
            if (ids[i] < 0) continue;
            lv_obj_set_style_border_color(g_sub_cards[i],
                (ids[i] == cur) ? lv_color_make(198, 76, 104)
                                : lv_color_make(120, 156, 190), 0);
        }
        drawn_sel = cur;
    }
}

static void refresh_codex_list(void) {
    static int img_key[CODEX_LIST_ROWS] = { -9999, -9999, -9999, -9999 };
    const int top = fishing_codex_cursor();
    fishing_status_t s2;
    fishing_get_status(&s2);
    char buf[128];
    for (int r = 0; r < CODEX_LIST_ROWS; ++r) {
        const int idx = (top + r) % FISH_SPECIES_COUNT;
        const fish_species_t *f = fishing_species_info(idx);
        if (!f) continue;
        const bool seen = fishing_codex_is_seen(idx);
        const int ss = seen ? fishing_codex_subspot(idx) : -1;
        const int key = seen ? idx : -(idx + 1);
        if (key != img_key[r]) {
            img_key[r] = key;
            lv_img_set_src(g_cdx_l_img[r], seen ? THUMB_FISH_IMG[idx] : THUMB_SIL_IMG[idx]);
        }
        snprintf(buf, sizeof(buf), "%d. %s", idx + 1, seen ? f->name : "???");
        set_text_cached(g_cdx_l_name[r], buf);
        if (seen && ss >= 0)
            snprintf(buf, sizeof(buf), "%s·%s",
                     fishing_spot_name((spot_t)f->spot), fishing_subspot_name(ss));
        else
            snprintf(buf, sizeof(buf), "%s", fishing_spot_name((spot_t)f->spot));
        set_text_cached(g_cdx_l_where[r], buf);
        {
            const char *bait = first_bait_name(f->bait_mask);
            if (bait) snprintf(buf, sizeof(buf), "%s · %s", period_short(f->period_pref), bait);
            else      snprintf(buf, sizeof(buf), "%s", period_short(f->period_pref));
        }
        set_text_cached(g_cdx_l_when[r], buf);
        if (seen) {
            const int bl = fishing_codex_best_len(idx);
            snprintf(buf, sizeof(buf), "%d.%dcm", bl / 10, bl % 10);
        } else {
            snprintf(buf, sizeof(buf), "%d-%dcm", f->len_min_mm / 10, f->len_max_mm / 10);
        }
        set_text_cached(g_cdx_l_size[r], buf);
    }
    /* 底部计数行动态生成：数量一律取常量，效果图里的「27/12」不许抄进源码 */
    snprintf(buf, sizeof(buf), "已收录 %d 种 · 标点 %d 处", s2.codex_total, FISH_SUBSPOT_COUNT);
    set_text_cached(g_lbl_hint, buf);
}

static void refresh_ui(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    hud_batt_refresh();                    /* 顶栏电量：所有页面都刷新 */
    switch (st.state) {
        case STATE_MENU:                       show_only(3); refresh_menu(&st);        break;
        case STATE_CODEX:
        case STATE_CODEX_INFO:                 show_only(4); refresh_codex(&st);       break;
        case STATE_SPOT_SELECT:                show_only(5); refresh_spot_pick();      break;
        case STATE_SUBSPOT:                    show_only(6); refresh_sub_pick();       break;
        case STATE_CODEX_LIST:                 show_only(7); refresh_codex_list();     break;
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
        /* 上下键 = 竿速档位：慢档好控小绿区，快档追冲刺鱼。
         * 只在单击时切，不影响 OK 的电平收线。 */
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            if (ev == BSP_BTN_CLICK) fishing_set_reel_gear(btn == BSP_BTN_DOWN ? 1 : 0);
            return;
        }
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

    /* ---- v5 选钓场 ---- */
    if (st.state == STATE_SPOT_SELECT) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)        g_spot_pick_idx = (g_spot_pick_idx - 1 + FISH_SPOT_COUNT) % FISH_SPOT_COUNT;
        else if (btn == BSP_BTN_DOWN) g_spot_pick_idx = (g_spot_pick_idx + 1) % FISH_SPOT_COUNT;
        else if (btn == BSP_BTN_OK) {
            /* 未解锁场只让看，不让进；提示条常驻"再钓 N 条解锁 X"，不是静默无反应 */
            if (fishing_spot_unlocked((spot_t)g_spot_pick_idx)) {
                fishing_select_spot(g_spot_pick_idx);
                fishing_open_subspot_select();
            }
        }
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- v5 选标点 ---- */
    if (st.state == STATE_SUBSPOT) {
        if (ev != BSP_BTN_CLICK) return;
        int ids[2] = { -1, -1 }, n = 0;
        for (int i = 0; i < FISH_SUBSPOT_COUNT && n < 2; ++i) {
            const subspot_cfg_t *ss = fishing_subspot_info(i);
            if (ss && ss->spot == (unsigned char)st.spot) ids[n++] = i;
        }
        if (n <= 0) return;
        const int cur = fishing_subspot();
        int at = 0;
        for (int i = 0; i < n; ++i) if (ids[i] == cur) at = i;
        if (btn == BSP_BTN_UP)        fishing_set_subspot(ids[(at - 1 + n) % n]);
        else if (btn == BSP_BTN_DOWN) fishing_set_subspot(ids[(at + 1) % n]);
        else if (btn == BSP_BTN_OK) {
            fishing_confirm_subspot();
            g_menu_idx = 3;   /* 回主菜单「钓点」行，下次抛竿生效 */
        }
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- v5 图鉴列表 ---- */
    if (st.state == STATE_CODEX_LIST) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP)        fishing_codex_list_move(-1);
        else if (btn == BSP_BTN_DOWN) fishing_codex_list_move(1);
        else if (btn == BSP_BTN_OK)   fishing_codex_list_open_info();
        fishing_audio_play(SFX_CLICK);
        return;
    }

    /* ---- 图鉴详情（旧入口，保留兼容） ---- */
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
            } else if (g_menu_idx == 3) {
                /* v5：钓点行不再就地改值，进入选钓场页（再进标点页） */
                g_spot_pick_idx = (int)st.spot;
                fishing_open_spot_select();
            } else if (g_menu_idx == 4) {
                fishing_open_codex_list();
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
            fishing_codex_close();
            fishing_audio_play(SFX_CLICK);
            break;
        case STATE_CODEX_INFO:
            fishing_codex_back_to_list();   /* 详情 -> 列表，再长按才回菜单 */
            fishing_audio_play(SFX_CLICK);
            break;
        case STATE_CODEX_LIST:
            fishing_codex_close();
            fishing_audio_play(SFX_CLICK);
            break;
        case STATE_SPOT_SELECT:
            fishing_cancel_spot_select();
            fishing_audio_play(SFX_CLICK);
            break;
        case STATE_SUBSPOT:
            fishing_back_to_spot_select();
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
