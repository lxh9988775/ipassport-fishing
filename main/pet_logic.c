/*
 * pet_logic.c - 电子宠物「口袋兔」纯逻辑层实现（v2）
 *
 * 设计要点（对齐 fishing_logic.c 的工程约定）：
 *   - 零动态分配、零浮点：全部整数毫秒/点数
 *   - 衰减用"每点耗时"累加器实现，跨长 dt 也不会一次性跳变
 *   - 存档带 magic + schema_ver，字段只追加不搬家
 *   - 贴纸簿 12 枚用 uint16 位图持久化（NVS 复用同一份存档 blob）
 *
 * v2（2026-10-04）：
 *   - 分动作照顾计数（feed/clean/play/sleep_count）用于贴纸里程碑
 *   - 12 枚贴纸位图 + 跨会话持久化
 *   - 小游戏奖励接口 pet_add_fun()
 *   - 存档 schema_ver=2，旧 v1 存档自动迁移（只搬基础四维）
 */

#include <stddef.h>
#include <string.h>
#include "pet_logic.h"

/* ===================== 衰减节奏（策划方案定死的适龄参数） =====================
 * 满状态离机后见到第一个"不开心"的时间（约）：
 *   饱食 100 点 × 40min/点 ≈ 2.8 天
 *   清洁 100 点 × 60min/点 ≈ 4.2 天
 *   玩乐 100 点 × 30min/点 ≈ 2.1 天
 *   精力 100 点 × 45min/点 ≈ 3.1 天
 * 刻意比真实电子宠物慢一个量级：孩子两天不玩，回来宠物只是"有点想你"。 */
#define HUNGER_MS_PER_POINT  (40u  * 60u * 1000u)
#define CLEAN_MS_PER_POINT   (60u  * 60u * 1000u)
#define FUN_MS_PER_POINT     (30u  * 60u * 1000u)
#define ENERGY_MS_PER_POINT  (45u  * 60u * 1000u)
/* 睡觉时：精力 3 分钟回 1 点（0→100 睡 5 小时），饱食衰减放缓一倍，清洁/玩乐冻结 */
#define SLEEP_ENERGY_MS_PER_POINT (3u * 60u * 1000u)

/* 动作收益 */
#define FEED_GAIN    30
#define CLEAN_GAIN   40
#define PLAY_GAIN    25
#define PLAY_COST_E  10
#define PLAY_COST_H  5
#define PLAY_MIN_E   15

#define XP_PER_CARE  5
#define XP_PER_PLAY  8
#define XP_PER_LEVEL 60

/* 贴纸里程碑阈值 */
#define FEED5_N  5
#define CLEAN5_N 5
#define PLAY5_N  5

/* ===================== 内部状态 ===================== */
typedef struct {
    int hunger, clean, fun, energy;
    bool sleeping;
    int xp, care_count;
    int feed_count, clean_count, play_count, sleep_count;  /* v2 分动作计数 */
    uint16_t sticker_mask;                                 /* v2 12 枚贴纸位图 */
    int last_ms;
    bool inited;
    /* 衰减累加器（毫秒） */
    uint32_t acc_hunger, acc_clean, acc_fun, acc_energy;
} pet_state_t;

static pet_state_t S;

static void acc_reset_all(void) {
    S.acc_hunger = S.acc_clean = S.acc_fun = S.acc_energy = 0;
}

void pet_init(void) {
    memset(&S, 0, sizeof(S));
    S.hunger = 80; S.clean = 80; S.fun = 70; S.energy = 90;
    S.sleeping = false;
    S.last_ms = 0;
    S.inited = true;
    acc_reset_all();
    /* 初次见面贴纸（仅当尚未记录过，避免每局重开后反复发） */
    pet_sticker_grant(PST_FIRST);
}

static int clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

static void grant_xp(int amount, pet_event_t *ev) {
    int old_level = S.xp / XP_PER_LEVEL;
    S.xp += amount;
    if (ev && *ev == PET_EVT_NONE && S.xp / XP_PER_LEVEL > old_level) {
        *ev = PET_EVT_LEVEL_UP;
    }
}

/* 单维度慢衰减：acc 累加 dt，每攒满 rate_ms 扣 1 点，扣到 0 为止 */
static void decay(int *val, uint32_t *acc, uint32_t dt, uint32_t rate_ms) {
    if (*val <= 0) { *acc = 0; return; }
    *acc += dt;
    while (*acc >= rate_ms && *val > 0) {
        *acc -= rate_ms;
        (*val)--;
    }
    if (*val <= 0) *acc = 0;
}

/* ===================== 贴纸 ===================== */
void pet_sticker_grant(unsigned id) {
    if (id >= PET_STICKER_MAX) return;
    S.sticker_mask |= (uint16_t)(1u << id);
}

bool pet_sticker_has(unsigned id) {
    if (id >= PET_STICKER_MAX) return false;
    return (S.sticker_mask & (uint16_t)(1u << id)) != 0;
}

int pet_sticker_count(void) {
    int c = 0;
    for (int i = 0; i < PET_STICKER_MAX; ++i)
        if (S.sticker_mask & (uint16_t)(1u << i)) c++;
    return c;
}

/* 照顾类里程碑：在动作计数变化后调用 */
static void check_care_stickers(void) {
    if (S.feed_count  >= FEED5_N)  pet_sticker_grant(PST_FEED5);
    if (S.clean_count >= CLEAN5_N) pet_sticker_grant(PST_CLEAN5);
    if (S.play_count  >= PLAY5_N)  pet_sticker_grant(PST_PLAY5);
    int lvl = S.xp / XP_PER_LEVEL + 1;
    if (lvl >= 2) pet_sticker_grant(PST_LVL2);
    if (lvl >= 3) pet_sticker_grant(PST_LVL3);
}

pet_event_t pet_tick(int now_ms) {
    pet_event_t ev = PET_EVT_NONE;
    if (!S.inited) pet_init();

    int dt = now_ms - S.last_ms;
    if (dt < 0) dt = 0;                 /* 时钟回绕/首次：当 0 处理 */
    S.last_ms = now_ms;
    if (dt == 0) return PET_EVT_NONE;

    if (S.sleeping) {
        /* 睡觉：精力回复；饱食衰减放缓一倍；清洁/玩乐冻结 */
        uint32_t udt = (uint32_t)dt;
        S.acc_energy += udt;
        while (S.acc_energy >= SLEEP_ENERGY_MS_PER_POINT && S.energy < 100) {
            S.acc_energy -= SLEEP_ENERGY_MS_PER_POINT;
            S.energy++;
        }
        if (S.energy >= 100) {
            S.sleeping = false;
            acc_reset_all();
            S.sleep_count++;
            pet_sticker_grant(PST_SLEEP1);
            ev = PET_EVT_SLEEP_FULL;    /* 睡饱自动醒 */
        }
        decay(&S.hunger, &S.acc_hunger, udt, HUNGER_MS_PER_POINT * 2u);
    } else {
        uint32_t udt = (uint32_t)dt;
        decay(&S.hunger, &S.acc_hunger, udt, HUNGER_MS_PER_POINT);
        decay(&S.clean,  &S.acc_clean,  udt, CLEAN_MS_PER_POINT);
        decay(&S.fun,    &S.acc_fun,    udt, FUN_MS_PER_POINT);
        decay(&S.energy, &S.acc_energy, udt, ENERGY_MS_PER_POINT);
    }
    return ev;
}

/* ===================== 动作 ===================== */
pet_event_t pet_feed(void) {
    if (!S.inited) pet_init();
    if (S.hunger >= 98) return PET_EVT_ACTION_FULL;
    S.hunger = clamp100(S.hunger + FEED_GAIN);
    S.fun = clamp100(S.fun + 2);
    S.care_count++;
    S.feed_count++;
    pet_event_t ev = PET_EVT_ACTION_OK;
    grant_xp(XP_PER_CARE, &ev);
    check_care_stickers();
    return ev;
}

pet_event_t pet_clean(void) {
    if (!S.inited) pet_init();
    if (S.clean >= 98) return PET_EVT_ACTION_FULL;
    S.clean = clamp100(S.clean + CLEAN_GAIN);
    S.care_count++;
    S.clean_count++;
    pet_event_t ev = PET_EVT_ACTION_OK;
    grant_xp(XP_PER_CARE, &ev);
    check_care_stickers();
    return ev;
}

pet_event_t pet_play(void) {
    if (!S.inited) pet_init();
    if (S.energy < PLAY_MIN_E) return PET_EVT_TOO_TIRED;
    S.fun = clamp100(S.fun + PLAY_GAIN);
    S.energy = clamp100(S.energy - PLAY_COST_E);
    S.hunger = clamp100(S.hunger - PLAY_COST_H);
    S.care_count++;
    S.play_count++;
    pet_event_t ev = PET_EVT_ACTION_OK;
    grant_xp(XP_PER_PLAY, &ev);
    check_care_stickers();
    return ev;
}

pet_event_t pet_toggle_sleep(void) {
    if (!S.inited) pet_init();
    if (S.sleeping) {
        S.sleeping = false;
        acc_reset_all();
        return PET_EVT_WAKE;
    }
    S.sleeping = true;
    acc_reset_all();
    return PET_EVT_SLEEP_START;
}

/* ===================== 小游戏奖励（只涨不扣） ===================== */
void pet_add_fun(int delta) {
    if (!S.inited) pet_init();
    S.fun = clamp100(S.fun + delta);
}

/* ===================== 查询 ===================== */
void pet_get_status(pet_status_t *out) {
    if (!out) return;
    if (!S.inited) pet_init();
    out->hunger = S.hunger;
    out->clean = S.clean;
    out->fun = S.fun;
    out->energy = S.energy;
    out->sleeping = S.sleeping;
    out->xp = S.xp;
    out->level = S.xp / XP_PER_LEVEL + 1;
    out->care_count = S.care_count;
    out->feed_count = S.feed_count;
    out->clean_count = S.clean_count;
    out->play_count = S.play_count;
    out->sleep_count = S.sleep_count;
}

const char *pet_mood_name(const pet_status_t *st) {
    if (!st) return "";
    if (st->sleeping) return "睡得香";
    int worst = st->hunger;
    if (st->clean < worst) worst = st->clean;
    if (st->fun < worst) worst = st->fun;
    if (st->energy < worst) worst = st->energy;
    if (worst < 20) return "不开心";
    if (worst < 45) return "还行";
    return "开心";
}

/* ===================== 存档（v2，含 v1 迁移） ===================== */
typedef struct {
    uint32_t magic;
    uint8_t  schema_ver;
    uint8_t  hunger, clean, fun, energy;
    uint8_t  sleeping;
    uint16_t xp;
    uint16_t care_count;
    /* v2 追加字段 */
    uint16_t feed_count, clean_count, play_count, sleep_count;
    uint16_t sticker_mask;
} pet_save_t;

int pet_save_size(void) { return (int)sizeof(pet_save_t); }

int pet_save_serialize(uint8_t *buf, int cap) {
    if (!buf || cap < (int)sizeof(pet_save_t)) return 0;
    pet_save_t s;
    memset(&s, 0, sizeof(s));
    s.magic = PET_SAVE_MAGIC;
    s.schema_ver = PET_SCHEMA_VER;
    s.hunger = (uint8_t)S.hunger;
    s.clean = (uint8_t)S.clean;
    s.fun = (uint8_t)S.fun;
    s.energy = (uint8_t)S.energy;
    s.sleeping = S.sleeping ? 1 : 0;
    s.xp = (uint16_t)S.xp;
    s.care_count = (uint16_t)S.care_count;
    s.feed_count = (uint16_t)S.feed_count;
    s.clean_count = (uint16_t)S.clean_count;
    s.play_count = (uint16_t)S.play_count;
    s.sleep_count = (uint16_t)S.sleep_count;
    s.sticker_mask = S.sticker_mask;
    memcpy(buf, &s, sizeof(s));
    return (int)sizeof(s);
}

int pet_save_apply(const uint8_t *buf, int len) {
    /* 注意：不能直接用 len < sizeof(pet_save_t) 拒绝。
       v1 存档的结构比现在短（没有 v2 追加的分动作计数与贴纸位图），
       若按当前结构体长度卡，旧档会被判为非法而**静默丢弃**（玩家进度清零）。
       因为存档字段历来只追加、从不搬家，前段布局完全一致，所以允许"短读"：
       按 min(len, sizeof) 拷贝，剩余部分置 0，再由下面的版本分支迁移。 */
    if (!buf || len <= 0) return -2;

    pet_save_t s;
    memset(&s, 0, sizeof(s));

    size_t n = sizeof(s);
    if ((size_t)len < n) n = (size_t)len;

    /* magic + schema_ver 必须完整读到，否则无从判断版本 */
    size_t need = offsetof(pet_save_t, schema_ver) + sizeof(s.schema_ver);
    if (n < need) return -2;

    memcpy(&s, buf, n);
    if (s.magic != PET_SAVE_MAGIC) return -1;

    if (!S.inited) pet_init();

    if (s.schema_ver == PET_SCHEMA_VER) {
        /* 同版本：全量恢复 */
        S.hunger = clamp100(s.hunger);
        S.clean = clamp100(s.clean);
        S.fun = clamp100(s.fun);
        S.energy = clamp100(s.energy);
        S.sleeping = s.sleeping != 0;
        S.xp = s.xp;
        S.care_count = s.care_count;
        S.feed_count = s.feed_count;
        S.clean_count = s.clean_count;
        S.play_count = s.play_count;
        S.sleep_count = s.sleep_count;
        S.sticker_mask = s.sticker_mask;
        acc_reset_all();
        return 0;
    }

    if (s.schema_ver == 1) {
        /* v1 → v2 迁移：只搬基础四维，新字段归零（旧档没这些数据） */
        S.hunger = clamp100(s.hunger);
        S.clean = clamp100(s.clean);
        S.fun = clamp100(s.fun);
        S.energy = clamp100(s.energy);
        S.sleeping = s.sleeping != 0;
        S.xp = s.xp;
        S.care_count = s.care_count;
        S.feed_count = S.clean_count = S.play_count = S.sleep_count = 0;
        /* 保留 v1 已发的初次见面贴纸，其余归零 */
        S.sticker_mask = (uint16_t)(1u << PST_FIRST);
        acc_reset_all();
        return 0;   /* 迁移成功 */
    }

    /* 比当前更新的版本：结构未知，当新号处理 */
    return 1;
}

void pet_test_set_stats(int hunger, int clean, int fun, int energy) {
    if (!S.inited) pet_init();
    S.hunger = clamp100(hunger);
    S.clean = clamp100(clean);
    S.fun = clamp100(fun);
    S.energy = clamp100(energy);
}
