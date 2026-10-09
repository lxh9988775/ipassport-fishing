/*
 * fishing_logic.h - 钓鱼 v2「竿影浮标」纯逻辑层（平台无关）
 *
 * 这一层不依赖 LVGL / BSP / ESP-IDF，只有状态机 + 数据表 + 定点算法。
 * 目的：
 *   1. Windows / Linux 上可用 gcc 直接单测（tests/fishing_logic_test.c）
 *   2. 也可用 Python 参考实现对拍（tools/verify_logic.py，本机无 gcc 时的兜底）
 *   3. 真机 fishing.c 只负责"喂按键/时间进逻辑层，把中间量画出去"
 *
 * 约束（贴合 AI Passport 硬件）：无 PSRAM、无 FPU 依赖 —— 零动态分配、零浮点，
 * 收线小游戏全部用 0..1000 的整数定点表达。
 *
 * v2 相对 v1 的变化：
 *   - 新增 STATE_REELING 收线小游戏（星露谷式：按住 OK 抬捕捉区，把鱼稳在区里涨进度）
 *   - 新增 24 鱼种数据表（真实体长/体重/稀有度/偏好饵料/简介/挣扎参数）
 *   - 新增 3 鱼竿（影响捕捉区高度、起落速度、得分系数）
 *   - 新增关卡解锁 + 图鉴捕获记录
 *   - 新增版本化存档（magic + schema_ver），为 v3 追加内容预留（见 DESIGN.md 第 9 节）
 */

#ifndef FISHING_LOGIC_H
#define FISHING_LOGIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 版本 / 容量 / 能力位（升级预留） ===================== */
#define FISHING_SCHEMA_VER   1      /* 存档结构版本：结构变动才 +1，触发迁移 */
#define FISHING_CONTENT_VER  1      /* 内容版本：改数值/文案 +1，不触发迁移 */
#define FISHING_SAVE_MAGIC   0x46534832u  /* 'FSH2' */
#define FISHING_CODEX_CAP    64     /* 图鉴上限：现用 24，v3 起只追加不搬家 */

#define FEAT_SINGLE  0x0001u        /* 单机钓鱼 */
#define FEAT_CODEX   0x0002u        /* 鱼图鉴 */
#define FEAT_LEVELS  0x0004u        /* 关卡解锁 */
#define FEAT_RODS    0x0008u        /* 鱼竿选择 */
#define FEAT_BLE     0x0010u        /* 蓝牙联机（Phase 4 预留） */
#define FEAT_DAILY   0x0020u        /* 每日挑战（预留） */

/* 当前固件实际支持的能力位：IDE 阶段据此决定是否展示对应入口 */
#define FISHING_SUPPORTED_FEATURES (FEAT_SINGLE | FEAT_CODEX | FEAT_LEVELS | FEAT_RODS)

/* 内容体量（遍历一律用 *_COUNT，不要写死数字，方便后续追加） */
#define FISH_SPECIES_COUNT 24
#define FISH_BAIT_COUNT    3
#define FISH_ROD_COUNT     3
#define FISH_SPOT_COUNT    3
#define FISH_RARITY_COUNT  4

/* ===================== 枚举 ===================== */
typedef enum {
    BAIT_WORM  = 0,  /* 蚯蚓：小鱼多，咬钩快，新手友好 */
    BAIT_DOUGH = 1,  /* 面团：中鱼均衡，节奏适中 */
    BAIT_LURE  = 2,  /* 亮片：大鱼概率高，但咬钩慢 */
    BAIT_COUNT = FISH_BAIT_COUNT
} bait_t;

typedef enum {
    ROD_HAND = 0,    /* 手竿：捕捉区大、起落慢，最稳 */
    ROD_LURE = 1,    /* 路亚竿：起落快，适合乱窜的鱼 */
    ROD_SEA  = 2,    /* 海竿：容错高、得分高，但重、跟手慢 */
    ROD_COUNT = FISH_ROD_COUNT
} rod_t;

typedef enum {
    SPOT_POND  = 0,  /* 静水塘：Lv1，新手友好 */
    SPOT_RIVER = 1,  /* 急流河：Lv2 */
    SPOT_SEA   = 2,  /* 深海：Lv3 */
    SPOT_COUNT = FISH_SPOT_COUNT
} spot_t;

typedef enum {
    RARITY_COMMON = 0,
    RARITY_UNCOMMON,
    RARITY_RARE,
    RARITY_LEGEND,
    RARITY_COUNT = FISH_RARITY_COUNT
} rarity_t;

typedef enum {
    MODE_SINGLE = 0,  /* 单机 */
    MODE_VS     = 1,  /* 联机对战（Phase 4 预留） */
    MODE_DAILY  = 2   /* 每日挑战（预留） */
} fishing_mode_t;

typedef enum {
    STATE_IDLE = 0,   /* 待机：OK 抛竿 / 长按开菜单 */
    STATE_CASTING,    /* 抛竿动画（短暂） */
    STATE_WAITING,    /* 等鱼咬钩 */
    STATE_BITE,       /* 咬钩窗口：限时提竿 */
    STATE_REELING,    /* 收线小游戏（核心玩法） */
    STATE_CATCH,      /* 上鱼结算 */
    STATE_ESCAPE,     /* 跑鱼结算 */
    STATE_MENU,       /* 主菜单 */
    STATE_CODEX,      /* 图鉴浏览 */
    STATE_CODEX_INFO, /* 图鉴详情 */
    STATE_COUNT
} game_state_t;

typedef enum {
    EVT_NONE        = 0,
    EVT_BITE_START,    /* 咬钩：进限时提竿窗口 */
    EVT_BITE_TIMEOUT,  /* 提竿太慢 -> 跑鱼 */
    EVT_CATCH,         /* 收线成功 */
    EVT_ESCAPE,        /* 收线失败 */
    EVT_NEW_RECORD,    /* 破纪录（渲染侧可放特效） */
    EVT_UNLOCK,        /* 解锁新关卡 */
    EVT_BACK_IDLE      /* 结算完毕回到待机 */
} fishing_event_t;

/* ===================== 数据表 ===================== */
typedef struct {
    const char *name;      /* 中文名 */
    const char *desc;      /* 一句话简介 */
    uint8_t  spot;         /* 栖息地 spot_t */
    uint8_t  rarity;       /* rarity_t */
    uint8_t  bait_mask;    /* 偏好饵料位掩码：bit0 蚯蚓 bit1 面团 bit2 亮片 */
    uint16_t len_min_mm;   /* 体长下限（毫米，定点存方便显示） */
    uint16_t len_max_mm;
    /* 体重用 32 位：旗鱼上限 80000g 超过 uint16_t 的 65535，
     * 用 16 位会被截断成 14464g —— 反而比下限 20000g 还小，
     * 图鉴里会出现"鱼越大体重越轻"。这个坑踩过，别改回 16 位。 */
    uint32_t wgt_min_g;    /* 体重下限（克） */
    uint32_t wgt_max_g;
    uint16_t base_score;   /* 基础分（未乘倍率） */
    uint8_t  dart;         /* 乱窜程度 0..100：越高越难跟 */
    uint8_t  power;        /* 挣扎力度 0..100：越高进度掉得越快 */
} fish_species_t;

typedef struct {
    const char *name;
    const char *desc;
    uint16_t bar_h;        /* 捕捉区高度（定点 0..1000 轨道内的占比 0..1000） */
    uint16_t rise_spd;     /* 按住时的上抬速度（单位/秒） */
    uint16_t fall_spd;     /* 松开时的下落速度（单位/秒） */
    uint16_t mult100;      /* 得分系数 *100 */
} rod_cfg_t;

typedef struct {
    const char *name;
    uint16_t wait_min_ms;  /* 等待咬钩最短时长 */
    uint16_t wait_max_ms;
    uint16_t bite_window_ms; /* 提竿窗口，越难越短 */
    uint16_t mult100;      /* 分值倍率 *100 */
    uint16_t unlock_catch; /* 解锁所需累计捕获数 */
} spot_cfg_t;

/* ===================== 结果 / 快照 ===================== */
typedef struct {
    int  species;     /* 鱼种索引 */
    int  len_mm;      /* 本次实测体长 */
    int  wgt_g;       /* 本次实测体重 */
    int  score;       /* 本次实得分（已乘全部倍率） */
    bool perfect;     /* 全程未脱区 */
    bool new_record;  /* 是否刷新图鉴记录（更大个体） */
    bool first_catch; /* 是否首次捕获（解锁图鉴条目） */
} catch_result_t;

typedef struct {
    game_state_t state;
    int score;
    int high_score;
    int time_in_state_ms;
    int bite_window_ms;
    bait_t bait;
    rod_t  rod;
    spot_t spot;

    /* --- 收线小游戏实时量（0..1000 定点，UI 按比例换算像素） --- */
    int reel_fish_pos;   /* 鱼在轨道中的位置 */
    int reel_bar_pos;    /* 捕捉区底部位置 */
    int reel_bar_h;      /* 捕捉区高度 */
    int reel_progress;   /* 收线进度 0..1000 */
    bool reel_holding;   /* OK 是否按住 */

    /* --- 当前目标鱼（REELING / CATCH 有意义） --- */
    int  cur_species;    /* -1 表示无 */
    int  cur_len_mm;
    int  cur_wgt_g;

    /* --- 图鉴 / 进度 --- */
    int  codex_total;    /* 已收录种类数 */
    int  total_catch;    /* 累计捕获数 */
} fishing_status_t;

/* ===================== 生命周期 ===================== */
void fishing_init(uint32_t seed);
void fishing_set_bait(bait_t b);
void fishing_set_rod(rod_t r);
void fishing_set_spot(spot_t s);

/* 菜单里"上下键换一项"的策略（逻辑层，可单测）。
 * 返回 true = 真的换掉了；fishing_cycle_spot 在全部已解锁钓点都到不了新位置时
 * 返回 false（当前就只有这一个钓点解锁），UI 据此显示"再钓 N 条解锁 X"。 */
bool fishing_cycle_bait(int dir);
bool fishing_cycle_rod(int dir);
bool fishing_cycle_spot(int dir);
void fishing_enter_menu(void);
void fishing_exit_menu(void);

/* 抛竿：IDLE/MENU -> CASTING。返回预计等待时长 ms（仅 UI 提示用） */
int fishing_cast(void);

/* 推进时间（单调毫秒），返回事件 */
fishing_event_t fishing_tick(int now_ms);

/* 提竿：仅在 STATE_BITE 且窗口内有效，成功后进入收线小游戏。
 * 注意 v2 起这里返回 NULL —— 真正的战果要等收线结束（EVT_CATCH）后用
 * fishing_last_catch() 读取，因为分数取决于玩家在收线小游戏里的表现。 */
const catch_result_t *fishing_strike(void);
const catch_result_t *fishing_last_catch(void);

/* 收线：每帧喂入 OK 键的当前电平（主循环 20ms 调一次）。
 * down = 这一帧 OK 是否仍被按住。按下立即生效，松开需连续 2 帧去抖。
 * 为什么必须是电平而不是按键事件：见 fishing_logic.c 的实现注释。 */
void fishing_reel_hold_sample(bool down);

/* 收线：直接落值（不去抖）。仅供按键电压读不到时的事件兜底路径与单元测试使用。 */
void fishing_reel_hold(bool down);

/* 长按 OK：全状态共用一套电平驱动判定（和收线共用同一路 ADC 读数）。
 * down = 这一帧 OK 是否被按住，now_ms = 单调毫秒。
 * 返回值 true 表示【本次按住刚刚跨过长按阈值】，只在跨过的那一帧报一次，
 * 手指不松就永远不会重复报 —— 所以"长按开菜单"不会开了又被自己的长按关掉。
 * UI 层拿到 true 后再按当前页面分发（钓场开菜单 / 菜单返回 / 图鉴返回）。
 * 为什么不用 BSP 的 LONG_PRESS_START 事件：见 fishing_logic.c 的实现注释。 */
bool fishing_hold_sample(bool down, int now_ms);

/* 停止长按计时（收线等"长按另有含义"的页面每帧调用），避免把别的页面里
 * 按住的时间带过来凑成一次长按。 */
void fishing_hold_reset(void);

/* 取用"这次单击是不是长按补发的"：返回 true = 应当丢弃这次单击。
 * iot_button 在按满 1.5 秒之前松手会补发一个 SINGLE_CLICK，长按刚开出来的
 * 菜单会被这个迟到的单击当成「OK 确认」而在第 0 项上生效（菜单一闪即关）。
 * 只吞一次：取用后自动复位，玩家后续真实的点按不受影响。 */
bool fishing_hold_take_stale_click(void);

/* 状态查询 */
void fishing_get_status(fishing_status_t *out);

/* ===================== 图鉴 ===================== */
bool fishing_codex_is_seen(int species);                 /* 是否捕获过 */
int  fishing_codex_count(int species);                   /* 捕获条数 */
bool fishing_codex_is_perfect(int species);              /* 是否达成完美钓获 */
int  fishing_codex_best_len(int species);                /* 最大体长记录 mm */
const fish_species_t *fishing_species_info(int species);
const char *fishing_bait_name(bait_t b);
const char *fishing_rod_name(rod_t r);
const char *fishing_spot_name(spot_t s);
const char *fishing_rarity_name(rarity_t r);
/* sprite 查表在 UI 层 fishing.c 实现（逻辑层不依赖 LVGL） */
bool fishing_spot_unlocked(spot_t s);
/* 还差多少条累计钓获才解锁（已解锁返回 0）。UI 拿它拼"再钓 N 条解锁 X"。
 * 门槛本身是设计（静水塘 0 / 急流河 8 / 深海 20），这里只把差距暴露出来。 */
int  fishing_spot_unlock_left(spot_t s);
/* 第一个还没解锁的钓点（按解锁顺序就是按门槛顺序）；全部解锁返回 -1 */
int  fishing_next_locked_spot(void);

/* 图鉴浏览游标（UI 层驱动） */
void fishing_codex_open(void);
void fishing_codex_close(void);
void fishing_codex_move(int delta);
void fishing_codex_toggle_info(void);
int  fishing_codex_cursor(void);

/* ===================== 存档（版本化，便于 v3 迁移） ===================== */
/* 序列化当前进度到 buf，返回实际字节数（0 表示容量不足） */
int  fishing_save_serialize(uint8_t *buf, int cap);
/* 载入存档：0=成功 1=已迁移(版本升级) -1=不认识的数据 -2=长度非法 */
int  fishing_save_apply(const uint8_t *buf, int len);
int  fishing_save_size(void);

/* 最高分（持久化由应用层 NVS 负责，这里只缓存） */
int  fishing_get_high_score(void);
void fishing_set_high_score(int hs);

/* 应用入口：BSP 初始化后由 main.c 调用 */
void fishing_app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FISHING_LOGIC_H */
