/*
 * mage_state.c —— 《魔法帝之路》状态层实现（1:1 复刻 index.html 公式/存档）
 *
 * 重点：
 *  - newState / 默认档：与 HTML newState() 逐字段对应。
 *  - spellName/Power/Cost/Desc、expNeed、promoteReady、eqAtk/Def/Pow、
 *    rollRarity/rollIRar/rollEquip、slv/stoneN：逐字翻译 HTML 公式。
 *  - 存档：版本化二进制 blob（magic+ver + MageState 字节），整体 memcpy，规避 JSON。
 */
#include "mage_state.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"

/* ===================== 全局 ===================== */
MageState  g;
BattleState bt;
int        g_hasSave = 0;

/* ===================== 随机数（xorshift32） ===================== */
static uint32_t g_rng = 0x9E3779B9u;
void mage_srand(uint32_t s) { g_rng = (s == 0) ? 0x9E3779B9u : s; }
uint32_t mage_rand(void) {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rng = x;
    return x;
}
int mage_ri(int a, int b) {
    if (b < a) { int t = a; a = b; b = t; }
    uint32_t r = mage_rand();
    return a + (int)(r % (uint32_t)(b - a + 1));
}
float mage_rf(float a, float b) {
    uint32_t r = mage_rand();
    float t = (float)r / (float)0xFFFFFFFFu;
    return a + t * (b - a);
}

/* ===================== 默认档（= HTML newState） ===================== */
void mage_defaults(void) {
    memset(&g, 0, sizeof(g));
    g.rank = 0; g.level = 1; g.exp = 0;
    g.maxHp = 80; g.hp = 80; g.maxMp = 50; g.mp = 50;
    g.gold = 30; g.kills = 0; g.cats = 0;
    g.orbs = 3; g.potions = 2; g.herbs = 2; g.mats = 1;
    g.crystals = 0; g.seals = 1;
    g.diff = 1;
    g.questDone = 0; g.qBase = 0; g.accepted = -1; g.arank = 0;
    g.studyCnt = 0; g.atkBonus = 0; g.weaponLv = 0; g.armorLv = 0;
    g.sound = 1; g.autoMode = 1; g.autoCapture = 1;
    g.mapChapter = 0; g.mapLayer = 0;
    g.activePartner = 0; g.npartners = 0;
    g.spell[0] = 0; g.spell[1] = 0; g.spell[2] = 0;
    g.equipped = spell_key(0, 0, 0);            /* '0-0-0' */
    /* 初始习得三法：火弹/水弹/雷弹 */
    g.discovered[spell_key(0, 0, 0)] = 1;
    g.discovered[spell_key(0, 1, 0)] = 1;
    g.discovered[spell_key(0, 2, 0)] = 1;
    g.ndisc = 3;
    g.loadout[0] = spell_key(0, 0, 0);
    g.loadout[1] = spell_key(0, 1, 0);
    g.loadout[2] = spell_key(0, 2, 0);
    g.nload = 3;
    g.nebag = 0; g.ndoffers = 0;
    for (int i = 0; i < 3; i++) { g.equip[i].rar = -1; g.equip[i].name[0] = '\0'; }
    g.name[0] = '\0';
    strcpy(g.scene, "TITLE");
}

void mage_init(void) {
    mage_srand((uint32_t)esp_random());
    mage_defaults();
    mage_load_from_nvs();
    /* 启动永远停在标题（设备习惯），已读档则允许"继续冒险" */
    strcpy(g.scene, "TITLE");
}

void mage_new_game(void) {
    mage_defaults();
    strcpy(g.scene, "NAME");
}

/* ===================== spell key 工具 ===================== */
void spell_split(int key, int *f, int *e, int *m) {
    *f = key / 45;
    int rem = key % 45;
    *e = rem / 5;
    *m = rem % 5;
}
const char *spell_key_str(int key, char *buf, int n) {
    int f, e, m; spell_split(key, &f, &e, &m);
    snprintf(buf, n, "%d-%d-%d", f, e, m);
    return buf;
}

/* ===================== 魔法公式（逐字翻译 HTML） ===================== */
int slv(int key) {
    if (key < 0 || key >= 135) return 0;
    return g.spellLv[key];
}
void slvSet(int key, int v) {
    if (key < 0 || key >= 135) return;
    g.spellLv[key] = v;
}
int stoneN(int i) {
    if (i < 0 || i > 4) return 0;
    return g.stones[i] > 0 ? g.stones[i] : 0;
}
void stoneAdd(int i, int n) {
    if (i < 0 || i > 4) return;
    g.stones[i] += n;
}

const char *spellName(int key) {
    int f, e, m; spell_split(key, &f, &e, &m);
    static char buf[32];
    snprintf(buf, sizeof(buf), "%s%s%s", MODS[m].pre, ELEMS[e].n, FORMS[f].n);
    return buf;
}
int spellPower2(int key, int lv) {
    if (lv < 0) lv = 0;
    int a = key;
    int f = a / 45, e = (a % 45) / 5, m = a % 5;
    if (f < 0 || f > 2 || e < 0 || e > 8 || m < 0 || m > 4) return 0;
    float p = (9 + g.level * 3 + g.rank * 5)
              * FORMS[f].pow * ELEMS[e].pow * MODS[m].pow
              * (1 + 0.15f * (float)lv);
    return (int)(p + 0.5f);
}
int spellPower(int key) {
    return spellPower2(key, slv(key));
}
int spellCost(int key) {
    int f, e, m; spell_split(key, &f, &e, &m);
    float c = 3 * FORMS[f].pow * MODS[m].cost + (float)m * 8;
    return (int)(c + 0.5f);
}
const char *spellElem(int key) {
    int f, e, m; spell_split(key, &f, &e, &m);
    return ELEMS[e].c;
}
const char *spellDesc(int key) {
    int f, e, m; spell_split(key, &f, &e, &m);
    static char buf[64];
    snprintf(buf, sizeof(buf), "%s·%s属性·%s",
             FORMS[f].des, ELEMS[e].n, MODS[m].des);
    return buf;
}
int expNeed(void) { return (int)(16 + g.level * 11 + 0.5f); }
int promoteReady(void) {
    /* RANKS 共 7 项，最高阶索引 6，故升阶上限为 rank < 6 */
    return g.rank < 6 &&
           g.kills >= RANK_KILLS[g.rank] &&
           g.questDone >= g.rank + 1;
}
const DiffDef *diffC(void) {
    int d = g.diff;
    if (d < 0) d = 1;
    if (d > 3) d = 3;
    return &DIFFS[d];
}
int eqAtk(void) { return g.equip[0].rar >= 0 ? g.equip[0].atk : 0; }
int eqDef(void) { return g.equip[1].rar >= 0 ? g.equip[1].def : 0; }
float eqPow(void) { return g.equip[2].rar >= 0 ? g.equip[2].pow : 1.0f; }

/* ===================== 掉落 / 装备 ===================== */
int rollRarity(void) {
    float r = mage_rf(0, 100);
    float acc = 0;
    for (int i = 0; i < 5; i++) {
        acc += (float)RARITY[i].w;
        if (r < acc) return i;
    }
    return 0;
}
int rollIRar(int luck) {
    float r = mage_rf(0, 100) - (float)(luck > 0 ? luck : 0);
    float acc = 0;
    for (int i = 0; i < 4; i++) {
        acc += (float)IRARITY[i].w;
        if (r < acc) return i;
    }
    return 0;
}
void rollEquip(int minRar, EbagItem *out) {
    static const char slots[3] = { 'w', 'a', 't' };
    int slot = mage_ri(0, 2);
    int rar = rollRarity();
    if (rar < minRar) rar = minRar;
    if (mage_rf(0, 1) < 0.25f) rar = (rar < 3) ? rar + 1 : 3;
    out->slot = slots[slot];
    out->rar  = rar;
    const char *nm = EQ_SLOTS[slot].names[rar < 3 ? rar : 3];
    strncpy(out->name, nm, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    if (slots[slot] == 'w')      out->atk  = (rar + 1) * 6;
    else if (slots[slot] == 'a') out->def  = (rar + 1) * 3;
    else                         out->pow  = 1 + 0.12f * (rar + 1);
}

/* ===================== 存档（NVS 版本化二进制） ===================== */
#define MAGE_SAVE_MAGIC 0x4D473154u   /* "MGT1" */
#define MAGE_SAVE_VER   2   /* v2: 新增 chapterStars[5] 字段；旧v1存档加载时按"新号"丢弃（开发期一次性重置） */

int mage_save_serialize(uint8_t *buf, int cap) {
    int need = (int)(8 + sizeof(MageState));
    if (cap < need) return -1;
    uint32_t *h = (uint32_t *)buf;
    h[0] = MAGE_SAVE_MAGIC;
    h[1] = MAGE_SAVE_VER;
    memcpy(buf + 8, &g, sizeof(MageState));
    return need;
}
void mage_save_apply(const uint8_t *buf, int len) {
    if (len < (int)(8 + sizeof(MageState))) return;
    const uint32_t *h = (const uint32_t *)buf;
    if (h[0] != MAGE_SAVE_MAGIC) return;
    if (h[1] != MAGE_SAVE_VER)   return;   /* 版本不符：丢弃，当新号 */
    memcpy(&g, buf + 8, sizeof(MageState));
}
void mage_load_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("mage", NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, "save", NULL, &len) == ESP_OK && len > 0) {
        uint8_t *buf = (uint8_t *)malloc(len);
        if (buf) {
            if (nvs_get_blob(h, "save", buf, &len) == ESP_OK) {
                mage_save_apply(buf, (int)len);
                g_hasSave = 1;
            }
            free(buf);
        }
    }
    nvs_close(h);
}
void mage_persist(void) {
    uint8_t buf[8 + sizeof(MageState)];
    int n = mage_save_serialize(buf, (int)sizeof(buf));
    if (n <= 0) return;
    nvs_handle_t h;
    if (nvs_open("mage", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "save", buf, (size_t)n);
    nvs_commit(h);
    nvs_close(h);
    g_hasSave = 1;
}

/* ===================== 衍生查询（供 UI 场景，1:1 复刻 HTML 辅助） ===================== */
int discoveredCount(void) {
    int n = 0;
    for (int i = 0; i < 135; i++) if (g.discovered[i]) n++;
    return n;
}
int discoveredKeyAt(int i) {
    int n = 0;
    for (int k = 0; k < 135; k++) if (g.discovered[k]) { if (n == i) return k; n++; }
    return -1;
}
int enemyIndex(const char *type) {
    for (int i = 0; i < DEXORDER_COUNT; i++)
        if (strcmp(DEXORDER[i], type) == 0) return i;
    return -1;
}
int activeLoadoutKeys(int *out, int cap) {
    int n = 0;
    for (int i = 0; i < g.nload; i++) {
        int k = g.loadout[i];
        if (k >= 0 && k < 135 && g.discovered[k]) { if (n < cap) out[n] = k; n++; }
    }
    if (n == 0) {
        int k = g.equipped;
        if (k < 0 || k >= 135 || !g.discovered[k]) k = 0;
        if (cap > 0) out[0] = k;
        return 1;
    }
    return n;
}
int questProg(const QuestDef *q) {
    if (q->type[0] == 'k') return g.kills - g.qBase;   /* kill 类：累计击杀 */
    return g.herbs;                                     /* herb 类：月光草数 */
}
static char szone_buf[96];
const char *speciesZones(int idx) {
    if (idx < 0 || idx >= ENEMY_COUNT) return "—";
    if (strcmp(DEXORDER[idx], "cat") == 0) return "全区域（小概率）";
    szone_buf[0] = '\0'; int nz = 0;
    for (int c = 0; c < 5; c++) {
        for (int j = 0; j < ZONE_MOBS_N[c]; j++) {
            if (strcmp(ZONE_MOBS[c][j], DEXORDER[idx]) == 0) {
                if (nz > 0) strncat(szone_buf, " / ", sizeof(szone_buf) - strlen(szone_buf) - 1);
                strncat(szone_buf, CHAPTERS[c], sizeof(szone_buf) - strlen(szone_buf) - 1);
                nz++; break;
            }
        }
    }
    if (nz == 0) return "遗迹秘境";
    return szone_buf;
}
int mage_can_craft(const RecipeDef *r) {
    return g.herbs >= r->cH && g.mats >= r->cM && g.crystals >= r->cC;
}
int skillIndex(const char *s) {
    if (!s) return 0;
    static const char *K[7] = {"claw","swift","venom","leech","ember","frost","guard"};
    for (int i = 0; i < 7; i++) if (strcmp(s, K[i]) == 0) return i;
    return 0;
}
int traitIndex(const char *s) {
    if (!s) return 0;
    static const char *K[7] = {"fierce","tough","nimble","calm","lucky","flex","greedy"};
    for (int i = 0; i < 7; i++) if (strcmp(s, K[i]) == 0) return i;
    return 0;
}

/* ===================== 战斗（实现见 mage.c，1:1 复刻 HTML 战斗逻辑） ===================== */
