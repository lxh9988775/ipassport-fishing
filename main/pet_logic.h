/*
 * pet_logic.h - 电子宠物「口袋兔」纯逻辑层（平台无关）
 *
 * 这一层不依赖 LVGL / BSP / ESP-IDF：只有状态机 + 定时衰减 + 动作规则 + 存档。
 * 目的（对齐 fishing_logic 的分层约定）：
 *   1. Windows / Linux 上可用 gcc 直接单测
 *   2. 真机 pet.c 只负责"喂按键/时间进逻辑层，把中间量画出去"
 *
 * 适龄设计铁律（改数值前先读）：
 *   - 衰减必须慢：满状态离机 ~2 天才见底，绝不制造"再不喂就死"的焦虑
 *   - 不死亡、不惩罚：任何数值到 0 都不会"死"，只是不开心
 *   - 动作只涨不扣（除陪玩耗一点精力），反馈永远是鼓励式
 *
 * v2 新增（2026-10-04）：贴纸簿 + 分动作照顾计数 + 小游戏奖励接口。
 */

#ifndef PET_LOGIC_H
#define PET_LOGIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 版本 / 存档 ===================== */
#define PET_SCHEMA_VER   2
#define PET_SAVE_MAGIC   0x50455431u   /* 'PET1' */

/* ===================== 事件 ===================== */
typedef enum {
    PET_EVT_NONE = 0,
    PET_EVT_ACTION_OK,     /* 动作生效（喂了/洗了/玩了） */
    PET_EVT_ACTION_FULL,   /* 该项已满，无需再做（温和提示，不算失败） */
    PET_EVT_TOO_TIRED,     /* 精力不足，陪玩被婉拒 */
    PET_EVT_SLEEP_START,   /* 开始睡觉 */
    PET_EVT_SLEEP_FULL,    /* 睡饱自动醒来 */
    PET_EVT_WAKE,          /* 被按键唤醒 */
    PET_EVT_LEVEL_UP,      /* 升级 */
} pet_event_t;

/* ===================== 状态快照 ===================== */
typedef struct {
    int  hunger;      /* 饱食度 0..100，越低越饿 */
    int  clean;       /* 清洁度 0..100 */
    int  fun;         /* 玩乐值 0..100 */
    int  energy;      /* 精力 0..100 */
    bool sleeping;    /* 是否在睡觉 */
    int  xp;          /* 累计照顾经验 */
    int  level;       /* 等级 = xp/60 + 1 */
    int  care_count;  /* 累计照顾次数（喂+洗+玩） */
    /* v2：分动作计数（贴纸簿里程碑用） */
    int  feed_count;
    int  clean_count;
    int  play_count;
    int  sleep_count;   /* 睡饱自动醒的次数 */
} pet_status_t;

/* ===================== 贴纸簿（12 枚，NVS 持久化） ===================== */
#define PET_STICKER_MAX 12
enum {
    PST_FIRST = 0,   /* 初次见面 */
    PST_FEED5,       /* 喂食 5 次 */
    PST_CLEAN5,      /* 洗澡 5 次 */
    PST_PLAY5,       /* 陪玩 5 次 */
    PST_SLEEP1,      /* 睡饱 1 次 */
    PST_LVL2,        /* 到达 2 级 */
    PST_LVL3,        /* 到达 3 级 */
    PST_GAME_CATCH,  /* 接胡萝卜通关 */
    PST_GAME_COLOR,  /* 颜色配对通关 */
    PST_GAME_MEMORY, /* 记忆翻牌通关 */
    PST_GAME_PEEK,   /* 躲猫猫通关 */
    PST_GAME_RHYTHM, /* 节奏蹦蹦通关 */
};

void pet_sticker_grant(unsigned id);      /* 已拥有则无操作 */
bool pet_sticker_has(unsigned id);
int  pet_sticker_count(void);             /* 0..PET_STICKER_MAX */

/* ===================== 生命周期 ===================== */
void pet_init(void);

/* 推进时间（单调毫秒）。睡觉时精力回复、其它衰减放缓；醒着时四维慢衰减。 */
pet_event_t pet_tick(int now_ms);

/* ===================== 动作（6-9 岁：一键即反馈） ===================== */
pet_event_t pet_feed(void);          /* 喂食：饱食 +30 */
pet_event_t pet_clean(void);         /* 洗澡：清洁 +40 */
pet_event_t pet_play(void);          /* 陪玩：玩乐 +25，精力 -10（精力<15 婉拒） */
pet_event_t pet_toggle_sleep(void);  /* 睡觉/唤醒 开关 */

/* ===================== 小游戏奖励（只涨不扣） ===================== */
void pet_add_fun(int delta);         /* 玩乐 +delta（上限 100） */

/* ===================== 查询 ===================== */
void pet_get_status(pet_status_t *out);
const char *pet_mood_name(const pet_status_t *st);   /* 开心/还行/不开心/睡得香 */

/* ===================== 存档（版本化，v1 自动迁移） ===================== */
int  pet_save_serialize(uint8_t *buf, int cap);
/* 0=成功 1=旧版本已迁移 -1=不认识的数据 -2=长度非法 */
int  pet_save_apply(const uint8_t *buf, int len);
int  pet_save_size(void);

/* ===================== 测试辅助（仅单测使用） ===================== */
void pet_test_set_stats(int hunger, int clean, int fun, int energy);

/* 应用入口：main.c 完成 BSP 初始化后调用 */
void pet_app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* PET_LOGIC_H */
