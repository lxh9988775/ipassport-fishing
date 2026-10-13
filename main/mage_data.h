/*
 * mage_data.h —— 《魔法帝之路》静态数据表（1:1 复刻 HTML 版）
 *
 * 所有数值、文案、配置与原 index.html 完全一致，仅做 JS→C 的忠实翻译：
 *   - 对象数组 → C struct 数组
 *   - ELEM_OF / WEAK_OF 按怪物类型查表 → 并入 MobDef.elemOf / weakOf[]（等价行为）
 *   - 颜色统一用 #RRGGBB 字符串（绘制层用 lv_color_hex 解析），保持与 HTML 一致
 */
#ifndef MAGE_DATA_H
#define MAGE_DATA_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>   /* NULL */

/* ============ 进阶 / 魔法 ============ */
extern const char *RANKS[7];
extern const int   RANK_KILLS[7];          /* 晋升所需击杀 */
extern const char *TIER_NAMES[5];           /* 初级/中级/高级/国家级/古代 */

typedef struct { const char *n; float pow; float cost; int range; const char *des; } FormDef;
extern const FormDef FORMS[3];              /* 弹/光束/爆发 */
typedef struct { const char *n; const char *c; float pow; } ElemDef;
extern const ElemDef ELEMS[9];              /* 火水土雷风光暗虚无 */
typedef struct { const char *n; const char *pre; float pow; float cost; const char *des; } ModDef;
extern const ModDef MODS[5];                /* 普通/强力/广域/连发/古代 */

typedef struct { const char *k; const char *c; int w; float cap; } RarityDef;
extern const RarityDef RARITY[5];           /* N/R/SR/SSR/UR */
extern const char *CAT_NAMES[8];

/* ============ 怪物 ============ */
typedef struct { int lvl; const char *title; int rarityTo; } Evo;
typedef struct { const char *tool; float hp; } Cap;          /* tool: orb/seal；hp: 收服所需血量比例(0..1) */
typedef struct { const char *shape; const char *c; const char *c2; const char *eye;
                 int ear; int horn; int wing; int tail; } Art;
typedef struct {
    const char *type; const char *n;
    int hp, atk, chase, gold[2], exp, h;
    int capturable;
    Cap cap; int hasCap;
    const char *names[8]; int nNames;
    const char *skills[2]; int nSkills;
    const char *traits[3]; int nTraits;
    Evo evo; int hasEvo;
    const char *dex;
    int fly;
    Art art; int hasArt;
    int elemOf;                 /* ELEMS 下标：自身属性 */
    int weakOf[3]; int nWeak;   /* 弱点属性下标（最多 3） */
} MobDef;

extern const MobDef ENEMY[];            /* 基础 11 + 扩充 39 = 50 */
extern const int    ENEMY_COUNT;
extern const char  *DEXORDER[];         /* Object.keys(ENEMY) 顺序 */
extern const int    DEXORDER_COUNT;

/* 每章专属怪物池（扩充后每章 9~10 种；猫灵全图小概率） */
extern const char  *ZONE_MOBS[5][10];
extern const int    ZONE_MOBS_N[5];

/* ============ 角色名 / 副本 ============ */
extern const char *NAMES[7];
typedef struct {
    const char *name; const char *kind; const char *desc;
    const char *pool[5]; int minR, maxR, dropRar, reqLv;
} DungeonDef;
extern const DungeonDef DUNGEONS[30];
extern const char *CHAPTERS[5];

/* ============ 难度 ============ */
typedef struct { const char *n; float hp; float atk; float bossAtk; float dmgTaken; } DiffDef;
extern const DiffDef DIFFS[4];          /* 轻松/普通/困难/炼狱 */

/* ============ BOSS / 剧情 ============ */
typedef struct { const char *type; const char *n; } BossDef;
extern const BossDef BOSSES[5];
extern const char *STORY_ENTER[5];
extern const char *STORY_BOSS[5];

/* ============ 装备 ============ */
typedef struct { const char *n; const char *names[4]; } EqSlotDef;
extern const EqSlotDef EQ_SLOTS[3];      /* w 武器 / a 护甲 / t 饰品 */

/* ============ 物品稀有度 / 药水 / 素材 ============ */
typedef struct { const char *k; const char *c; int w; } IRarityDef;
extern const IRarityDef IRARITY[4];      /* 普通/稀有/史诗/传说 */

typedef struct {
    const char *n; float hp, mp; int cure, res, atkB, powB, defB, spd, expB, rev, ir;
    const char *des;
} PotionDef;
extern const PotionDef POTIONS[20];

typedef struct { const char *n; const char *des; int ir; } MaterialDef;
extern const MaterialDef MATERIALS[40];

extern const char *STONE_NAMES[5];       /* 下标 1..4 */
extern const char *STONE_C[5];

/* ============ 伙伴技能 / 特性 ============ */
typedef struct { const char *n; const char *d; } SkillDef;
extern const SkillDef SKILLS[7];         /* claw/swift/venom/leech/ember/frost/guard */
typedef struct { const char *n; const char *d; } TraitDef;
extern const TraitDef TRAITS[7];         /* fierce/tough/nimble/calm/lucky/flex/greedy */

/* ============ 城镇 ============ */
typedef struct { const char *n; const char *d; int scene; } LocDef;
extern const LocDef LOCATIONS[7];
typedef struct { const char *n; int scene; } FuncDef;
extern const FuncDef FUNCS[8];

#endif /* MAGE_DATA_H */
