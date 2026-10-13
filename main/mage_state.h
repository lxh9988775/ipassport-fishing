/*
 * mage_state.h —— 《魔法帝之路》运行时状态 + 存档 + 核心算法（1:1 复刻）
 *
 * 说明：设备端存档用"带版本号的二进制 blob"（NVS），不复用 HTML 的 JSON 文本；
 * 1:1 指"游戏状态字段、数值、玩法逻辑完全一致"，不是字节级 JSON 一致。
 * MageState 全部为 POD（无指针），可整体 memcpy 序列化进 NVS。
 */
#ifndef MAGE_STATE_H
#define MAGE_STATE_H
#include <stdbool.h>
#include <stdint.h>
#include "mage_data.h"

/* ============ 伙伴 ============ */
typedef struct {
    int typeIdx;          /* ENEMY 下标 */
    char name[16];        /* 昵称 */
    int level;
    int exp;
    int skillIdx;         /* SKILLS 下标 */
    int traitIdx;         /* TRAITS 下标 */
    int evolved;          /* 0/1 */
    int rar;              /* 稀有度 0..4（进化时提升） */
    int ivs[3];           /* 资质 0..20 */
    int hp, maxhp, atk;   /* 出战数值（按等级/资质计算） */
} Partner;

/* ============ 装备（三槽，含名称，均为 POD） ============ */
typedef struct {
    char slot;            /* 'w'/'a'/'t' */
    int rar;              /* -1 = 空；0..3 */
    int atk;              /* 武器 */
    int def;              /* 护甲 */
    float pow;            /* 饰品 */
    char name[16];
} EquipItem;

/* ============ 装备背包格 ============ */
typedef struct { char slot; int rar; int atk, def; float pow; char name[16]; } EbagItem;

/* ============ 战斗状态（瞬态，不入存档） ============ */
typedef struct {
    int active;           /* 是否进行中 */
    int mobIdx;           /* ENEMY 下标 */
    int mobHp, mobMax;
    int shield;           /* 破防盾层数 */
    int turn;             /* 0=玩家 1=敌方 */
    int round;
    int boss;             /* 0/1 */
    int chapter;          /* 章节下标（BOSS 战） */
    int dungeonId;        /* 副本下标（-1=普通野外） */
    int captured;         /* 本场是否已捕获 */
    int fled;
    int lootGold, lootExp, lootHerb, lootMat, lootStone, lootStoneN;
    int autoT2;           /* 结算倒计时（百分秒） */
    int result;           /* 0=胜 1=败 2=捕获 3=逃跑 */
    int hpPct;            /* 敌方血线比例（捕捉门槛判定） */
} BattleState;

/* ============ 主状态（与 HTML newState 逐字段对应） ============ */
typedef struct {
    char scene[16];
    char name[16];
    int rank, level, exp;
    int maxHp, hp, maxMp, mp;
    int gold, kills, cats, orbs, potions, herbs, mats, crystals, seals;
    int diff;             /* DIFFS 下标 */
    int questDone, qBase, accepted, arank;
    int studyCnt, atkBonus, weaponLv, armorLv;
    int sound, autoMode, autoCapture;
    int mapChapter, mapLayer;
    int activePartner;
    int dungeonOffers[4]; /* 当前野外刷出的副本 id，-1 空 */
    int ndoffers;
    int stones[5];        /* 魔石数量，下标 1..4（古代=4）*/
    int pbag[20];         /* 药水数量，下标 = POTIONS */
    int mbag[40];         /* 素材数量，下标 = MATERIALS */
    int dexSeen[50], dexCaught[50];   /* 图鉴，下标 = ENEMY */
    int bossKilled[5], storySeen[10]; /* 主线进度 */
    int spell[3];         /* 合成槽 [形态,元素,阶级] */
    int equipped;         /* 当前装配魔法 key */
    int loadout[16];      /* 出战编队（spell key） */
    int nload;
    int spellLv[135];     /* 魔法等级，下标 = spell key */
    int ndisc;            /* 已习得数 */
    int discovered[135];  /* 0/1 是否已习得 */
    Partner partners[6];
    int npartners;
    EquipItem equip[3];   /* w,a,t 顺序；rar<0 表示空 */
    EbagItem ebag[24];
    int nebag;
    /* 瞬时 UI 导航（不入存档语义，但随档一起落盘也无害） */
    int titleSel, nameIdx, locSel, funcSel, cityMode;
} MageState;

/* ============ 全局 ============ */
extern MageState g;          /* 当前状态 */
extern BattleState bt;       /* 当前战斗 */
extern int g_hasSave;        /* NVS 是否有存档 */

/* ============ 生命周期 ============ */
void mage_defaults(void);    /* 填默认值（= HTML newState） */
void mage_init(void);        /* 默认值 + 尝试读档 */
void mage_new_game(void);    /* 新游戏：默认值 + 进入 NAME */
void mage_load_from_nvs(void);
void mage_app_start(void);   /* 入口（main.c 调用） */

/* spell key = form*45 + elem*5 + mod （0..134） */
static inline int spell_key(int f, int e, int m) { return f * 45 + e * 5 + m; }
const char *spell_key_str(int key, char *buf, int n);
void spell_split(int key, int *f, int *e, int *m);

const char *spellName(int key);
int  spellPower(int key);
int  spellCost(int key);
const char *spellElem(int key);
const char *spellDesc(int key);
int  slv(int key);
void slvSet(int key, int v);
int  stoneN(int i);          /* i=1..4 */
void stoneAdd(int i, int n);

int  expNeed(void);
int  promoteReady(void);
const DiffDef *diffC(void);
int  eqAtk(void); int eqDef(void); float eqPow(void);
int  rollRarity(void);
int  rollIRar(int luck);
void rollEquip(int minRar, EbagItem *out);

/* 随机数（xorshift，mage_init 时用 esp_random 播种） */
void mage_srand(uint32_t s);
uint32_t mage_rand(void);
int  mage_ri(int a, int b);          /* 闭区间 [a,b] */
float mage_rf(float a, float b);

/* 存档（版本化二进制） */
int  mage_save_serialize(uint8_t *buf, int cap);
void mage_save_apply(const uint8_t *buf, int len);
void mage_persist(void);

/* 战斗（后续里程碑实现，先声明） */
void battle_start_wild(int chapter);
void battle_start_dungeon(int dungeonId);
void battle_start_boss(int chapter);

/* 衍生查询（供 UI 场景使用，1:1 复刻 HTML 辅助函数） */
int  discoveredCount(void);
int  discoveredKeyAt(int i);            /* 第 i 个已习得魔法 key（越界返回 -1） */
int  enemyIndex(const char *type);      /* DEXORDER 下标；找不到返回 -1 */
int  activeLoadoutKeys(int *out, int cap); /* 返回有效出战魔法 key 数 */
int  spellPower2(int key, int lv);      /* 指定等级威力（research/mupgrade 预览） */
int  questProg(const QuestDef *q);
const char *speciesZones(int idx);      /* 返回静态缓冲 */
int  mage_can_craft(const RecipeDef *r);
int  skillIndex(const char *s);         /* 技能字符串 → SKILLS 下标 */
int  traitIndex(const char *s);         /* 特性字符串 → TRAITS 下标 */

#endif /* MAGE_STATE_H */
