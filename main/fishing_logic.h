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
#define FISHING_SCHEMA_VER   3      /* 存档结构版本：结构变动才 +1，触发迁移。
                                     * v4: +first_day[CAP] +best_wgt[CAP]，schema 1 存档自动迁移填 0；
                                     * v5: +codex_subspot[CAP]，schema 1/2 存档自动迁移填 0 */
#define FISHING_CONTENT_VER  2      /* 内容版本：改数值/文案 +1，不触发迁移 */
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

/* 鱼的时段 / 天气偏好位掩码（v4「天气决定鱼」）。
 * 时段 bit 与 UI 层 env_period_t 取值一一对应：晨0/午1/昏2/夜3；
 * 天气 bit 与 env_weather_t 对应：晴0/阴1/雨2。UI 层禁止另造编号。 */
#define FISH_PERF_MORNING 0x01u
#define FISH_PERF_NOON    0x02u
#define FISH_PERF_DUSK    0x04u
#define FISH_PERF_NIGHT   0x08u
#define FISH_WPREF_CLEAR  0x01u
#define FISH_WPREF_CLOUDY 0x02u
#define FISH_WPREF_RAIN   0x04u

/* 内容体量（遍历一律用 *_COUNT，不要写死数字，方便后续追加） */
#define FISH_SPECIES_COUNT 27
#define FISH_BAIT_COUNT    3
#define FISH_ROD_COUNT     4
#define FISH_SPOT_COUNT    5
#define FISH_SUBSPOT_COUNT 10     /* 每场 2 处，按场次连续编号 0..9 */
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
    ROD_HEAVY = 3,   /* 巨物竿：绿区最大、专治冲刺猛的传说鱼，但得分系数最低 */
    ROD_COUNT = FISH_ROD_COUNT
} rod_t;

typedef enum {
    SPOT_POND    = 0,  /* 静水塘：Lv1，新手友好 */
    SPOT_RIVER   = 1,  /* 急流河：Lv2 */
    SPOT_SEA     = 2,  /* 深海：Lv3 */
    SPOT_ESTUARY = 3,  /* 入海口：Lv4，36 条解锁 */
    SPOT_PIER    = 4,  /* 夜钓栈桥：Lv5，56 条解锁 */
    SPOT_COUNT = FISH_SPOT_COUNT
} spot_t;

/* 场内标点（v5 C+2）：每场固定 2 处，按场次连续编号，属钓场不跨场。
 * 标点只提高主出鱼概率，不设门槛、不倒计时、不惩罚。 */
typedef enum {
    SUB_REED      = 0,  /* 静水塘 · 芦苇荡 */
    SUB_DEEP      = 1,  /* 静水塘 · 深水湾 */
    SUB_SHOAL     = 2,  /* 急流河 · 浅滩 */
    SUB_BACKWATER = 3,  /* 急流河 · 回水沱 */
    SUB_REEF      = 4,  /* 深海 · 礁石区 */
    SUB_OFFSHORE  = 5,  /* 深海 · 远洋 */
    SUB_MUD       = 6,  /* 入海口 · 泥滩 */
    SUB_TIDE      = 7,  /* 入海口 · 潮汐道 */
    SUB_LIGHT     = 8,  /* 夜钓栈桥 · 灯影 */
    SUB_PIER      = 9,  /* 夜钓栈桥 · 桥墩 */
    SUBSPOT_COUNT = FISH_SUBSPOT_COUNT
} subspot_t;

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
    STATE_SUBSPOT,    /* v5：选标点页（追加在 STATE_COUNT 前，现有状态值不变） */
    STATE_SPOT_SELECT,/* v5：选钓场页 */
    STATE_CODEX_LIST, /* v5：图鉴列表页 */
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
    const char *lore;      /* 性格钩子：一句"这鱼什么脾气/有什么故事"。
                             * desc 是客观说明（长什么样、多大），lore 是性格与故事，
                             * 两者分开：玩家看 desc 判断能不能钓，看 lore 决定想不想钓。
                             * 纯文案，不参与任何平衡计算。 */
    uint8_t  spot;         /* 主栖息地 spot_t（图鉴展示用，不随跨场改写） */
    uint8_t  spot_mask;    /* 可出现钓场位掩码：bit0塘/bit1河/bit2海/bit3河口/bit4栈桥。
                            * 默认 1u<<spot；跨场鱼按生态追加位（如鲈鱼 河|河口）。 */
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
    uint8_t  period_pref;  /* 时段偏好位掩码：FISH_PERF_*。命中权重×3，否则×0.6（不锁死） */
    uint8_t  weather_pref; /* 天气偏好位掩码：FISH_WPREF_*。命中权重×2 */
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

/* 标点配置（v5 C+2）：gold_a 为鱼 ID 位掩码（低 27 位），命中的主出鱼 ×3。
 * hook 是一句界面钩子文案，纯展示不参与计算。 */
typedef struct {
    const char *name;
    uint8_t     spot;      /* 所属钓场 spot_t */
    uint32_t    gold_a;    /* 主出鱼 ID 位掩码：bit i = 鱼 i */
    const char *hook;      /* 一句钩子（界面显示） */
} subspot_cfg_t;

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
    int  reel_phase;     /* 鱼的节奏：0巡游 1预警 2冲刺 3歇息 */
    int  reel_gear;      /* 竿速档位：0慢（精细） 1快（追击） */

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
/* 场内标点（v5 C+2）：-1 = 未选，兼容 v4 老行为（不叠加标点权重）。
 * 标点只改抽鱼概率，不改解锁、不设门槛；进入新场时用 fishing_first_subspot 自动选中。 */
void     fishing_set_subspot(int id);
int      fishing_subspot(void);
const char *fishing_subspot_name(int id);
const subspot_cfg_t *fishing_subspot_info(int id);
int      fishing_first_subspot(spot_t s);
/* 环境（v4 天气决定鱼）：由 UI 层在时段/天气算出后喂进来，只影响抽鱼概率，
 * 不改解锁门槛、不引入失败惩罚。period 0..3=晨午昏夜，weather 0..2=晴阴雨。
 * 不调用时默认 晨/晴（等价于 v3 行为），单测可固定环境做分布断言。 */
void fishing_set_env(int period, int weather);
/* 当前钓鱼日序号（用于③钓鱼履历记"首次钓到那天"）。不调用默认为 0。 */
void fishing_set_day(int day);

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

/* 竿速档位：0=慢（精细）1=快（追击）。收线中上下键切换，跨局保留。 */
void fishing_set_reel_gear(int gear);
int  fishing_get_reel_gear(void);

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
int  fishing_codex_first_day(int species);               /* 首次钓到的钓鱼日序号，0=未知(老存档/未捕获) */
int  fishing_codex_best_wgt(int species);                /* 最大重量记录 g，0=未知 */
/* 首次钓到该鱼时的标点 ID（0..9），-1=未知（老存档 / -1 标点捕获）。
 * 存档里存的是 id+1，0 留给"未知"，避免和第 0 号标点「芦苇荡」混淆。 */
int  fishing_codex_subspot(int species);
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

/* v5 C+2 页面流转（纯状态切换，UI 只递动作；老状态值不变）。
 * 选钓场页 OK 进入选标点页；未解锁钓场只选中查看，不进标点页。
 * 选标点页 OK 回主菜单「钓点」行，下次抛竿生效。 */
void fishing_open_spot_select(void);
void fishing_select_spot(int spot);
void fishing_open_subspot_select(void);
void fishing_back_to_spot_select(void);
void fishing_confirm_subspot(void);
void fishing_cancel_spot_select(void);
/* 图鉴浏览：v5 起菜单先进列表页，OK 进详情；长按逐层返回。 */
void fishing_open_codex_list(void);
void fishing_codex_list_move(int delta);
void fishing_codex_list_open_info(void);
void fishing_codex_back_to_list(void);
void fishing_codex_close(void);

/* 图鉴浏览游标（UI 层驱动） */
void fishing_codex_open(void);
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
