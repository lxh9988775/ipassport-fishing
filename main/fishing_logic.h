/*
 * fishing_logic.h - 钓鱼小游戏「竿影浮标」纯逻辑层（平台无关）
 *
 * 这一层不依赖 LVGL / BSP / ESP-IDF，纯粹是状态机 + 计分算法。
 * 目的：
 *   1. 可以在 Windows / Linux 上用 gcc 直接单测（见 tests/fishing_logic_test.c）
 *   2. 也可以在 Python 里做参考实现对拍（见 tools/verify_logic.py）
 *   3. 真机 fishing.c 只负责"把按键/时间喂进来、把画面画出去"
 *
 * 设计要点（贴合 AI Passport 硬件边界）：
 *   - 无 PSRAM / 8MB Flash 友好：逻辑层零动态分配、零浮点依赖（用整数定点）
 *   - 确定性 RNG：种子可注入，方便单测可复现
 */

#ifndef FISHING_LOGIC_H
#define FISHING_LOGIC_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 饵料（影响鱼群构成 / 咬钩节奏） ============ */
typedef enum {
    BAIT_WORM  = 0,  /* 蚯蚓：通用，小鱼多，咬钩快而稳 */
    BAIT_BREAD = 1,  /* 面团：中鱼概率高，节奏适中 */
    BAIT_LURE  = 2,  /* 亮片：大鱼概率高，但咬钩慢 */
    BAIT_COUNT
} bait_t;

/* ============ 钓点（影响分值倍率 / 难度） ============ */
typedef enum {
    SPOT_POND  = 0,  /* 静水塘：鱼小但多，分低，新手友好 */
    SPOT_RIVER = 1,  /* 急流河：中大鱼，分中 */
    SPOT_SEA   = 2,  /* 深海：大鱼多，分高但难 */
    SPOT_COUNT
} spot_t;

/* ============ 游戏状态 ============ */
typedef enum {
    STATE_IDLE  = 0, /* 待机：可抛竿 / 进菜单 */
    STATE_WAITING,   /* 抛竿后等待咬钩 */
    STATE_BITE,      /* 咬钩窗口：限时提竿 */
    STATE_CATCH,     /* 钓上鱼（结算展示） */
    STATE_MISS,      /* 跑鱼 / 超时 */
    STATE_MENU       /* 菜单：选饵料 / 钓点 */
} game_state_t;

/* ============ tick 产生的事件 ============ */
typedef enum {
    EVT_NONE        = 0,
    EVT_BITE_START,     /* 进入咬钩窗口 */
    EVT_BITE_TIMEOUT,   /* 窗口结束仍未提竿 -> 跑鱼 */
    EVT_CAST_READY      /* 回到可抛竿状态 */
} fishing_event_t;

/* ============ 钓上鱼的结果 ============ */
typedef struct {
    int fish_size;    /* 鱼大小档 1..3（1小 2中 3大）*/
    int base_score;   /* 鱼基础分（未乘钓点倍率）*/
    int score;        /* 实际得分（已乘钓点倍率，四舍五入）*/
} catch_result_t;

/* ============ 对外状态快照 ============ */
typedef struct {
    game_state_t state;
    int score;          /* 本局累计分 */
    int high_score;     /* 历史最高分（由应用层 NVS 回填）*/
    int time_in_state_ms;/* 当前状态已持续毫秒 */
    int bite_window_ms;   /* 当前咬钩窗口总时长（STATE_BITE 时有意义）*/
    bait_t bait;
    spot_t spot;
} fishing_status_t;

/* ---------- 生命周期 ---------- */
void fishing_init(uint32_t seed);
void fishing_set_bait(bait_t b);
void fishing_set_spot(spot_t s);
void fishing_enter_menu(void);
void fishing_exit_menu(void);

/* 抛竿：从 IDLE/MENU 进入 WAITING，返回预计等待时长(ms)（仅用于 UI 提示）*/
int fishing_cast(void);

/* 推进时间（now_ms 为单调毫秒时钟），返回事件 */
fishing_event_t fishing_tick(int now_ms);

/*
 * 提竿：
 *   仅在 STATE_BITE 且处于窗口内调用才成功，返回非空指针（结果缓存在内部）；
 *   其余情况返回 NULL（无效操作，鱼不会上钩也不会跑）。
 */
const catch_result_t* fishing_strike(void);

/* 状态查询 */
void fishing_get_status(fishing_status_t* out);

/* 最高分（持久化由应用层 NVS 负责，这里只缓存）*/
int  fishing_get_high_score(void);
void fishing_set_high_score(int hs);

/* 应用入口：在 BSP（显示/LVGL/按键/音频/电量/NVS）已初始化后由 main.c 调用 */
void fishing_app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FISHING_LOGIC_H */
