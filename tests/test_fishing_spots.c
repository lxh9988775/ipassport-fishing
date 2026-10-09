/*
 * test_fishing_spots.c - 「菜单里换鱼竿 / 饵料 / 钓点」单元测试（纯 C，无需硬件）
 *
 * 为什么专门加这一组：
 *   社区反馈「不能更换钓点」。根因不是按键坏了，而是钓点这一项的策略：
 *     - 只有静水塘默认解锁，急流河要累计钓满 8 条、深海 20 条；
 *     - 老代码在 fishing.c 里静默跳过没解锁的钓点，一个都跳不动时【什么都不做】，
 *       也不给任何提示 —— 玩家按住上下键画面纹丝不动，只能判断成"坏了"。
 *   现在策略下沉到逻辑层（fishing_cycle_spot），并且新增
 *   fishing_spot_unlock_left() / fishing_next_locked_spot() 让 UI 能说明白
 *   "再钓 N 条解锁 X"。门槛本身是设计，不动；这里钉的是"跳不动要能被上层知道"。
 *
 * 这个文件钉住三件事，任何一条被改回去都会红：
 *   A. 静水塘没钓够时，cycle_spot 必须返回 false（上层据此显示提示）—— 用例 1；
 *   B. 门槛边界：7 条还锁着、8 条解锁、19 条还锁着、20 条全开 —— 用例 2/3；
 *   C. 已解锁的钓点必须能双向循环，且永远跳不到未解锁的钓点上 —— 用例 2/3/4。
 *
 * 编译运行（需 cc）：
 *   cc -std=c11 -Wall -Wextra -Werror \
 *      tests/test_fishing_spots.c main/fishing_logic.c -o /tmp/spots
 *   /tmp/spots
 */

#include <stdio.h>
#include <string.h>
#include "fishing_logic.h"

/* 门槛真值（与 fishing_logic.c 的 SPOTS 表一致；改表必须一起改这里） */
#define RIVER_NEED 8
#define SEA_NEED   20

/* 存档里 total_catch 的字节偏移：magic4 + schema2 + content2 + features4 + high4 */
#define SAVE_OFF_TOTAL 16

static int g_fail;

static void check(const char *name, bool ok) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_fail++;
}

/* 用真实存档格式喂累计钓获：顺带把"门槛读的是存档里的数"这条也钉住 */
static void set_total(int n) {
    uint8_t buf[512];
    int len = fishing_save_serialize(buf, (int)sizeof(buf));
    if (len <= 0) { printf("  [FAIL] 存档序列化失败\n"); g_fail++; return; }
    buf[SAVE_OFF_TOTAL + 0] = (uint8_t)(n & 0xFF);
    buf[SAVE_OFF_TOTAL + 1] = (uint8_t)((n >> 8) & 0xFF);
    buf[SAVE_OFF_TOTAL + 2] = (uint8_t)((n >> 16) & 0xFF);
    buf[SAVE_OFF_TOTAL + 3] = (uint8_t)((n >> 24) & 0xFF);
    int r = fishing_save_apply(buf, len);
    if (r < 0) { printf("  [FAIL] 存档载入失败 r=%d\n", r); g_fail++; }
}

static spot_t spot_now(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    return st.spot;
}

static rod_t rod_now(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    return st.rod;
}

static bait_t bait_now(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    return st.bait;
}

/* ============ 1. 新档：只有静水塘，跳不动要如实返回 false ============ */
static void case_fresh_only_pond(void) {
    printf("[1] 新档（0 条）\n");
    fishing_init(20261009u);
    set_total(0);
    fishing_set_spot(SPOT_POND);

    check("静水塘默认解锁", fishing_spot_unlocked(SPOT_POND));
    check("急流河未解锁", !fishing_spot_unlocked(SPOT_RIVER));
    check("深海未解锁", !fishing_spot_unlocked(SPOT_SEA));

    check("下一个待解锁 = 急流河", fishing_next_locked_spot() == SPOT_RIVER);
    check("静水塘还差 0 条", fishing_spot_unlock_left(SPOT_POND) == 0);
    check("急流河还差 8 条", fishing_spot_unlock_left(SPOT_RIVER) == RIVER_NEED);
    check("深海还差 20 条", fishing_spot_unlock_left(SPOT_SEA) == SEA_NEED);

    /* 关键：换不动必须返回 false。老代码在这里静默什么都不做，
     * UI 拿不到"没换成功"这个事实，也就没法解释给玩家听。 */
    check("向上换：无路可走 -> false", fishing_cycle_spot(-1) == false);
    check("向上换之后仍是静水塘", spot_now() == SPOT_POND);
    check("向下换：无路可走 -> false", fishing_cycle_spot(1) == false);
    check("向下换之后仍是静水塘", spot_now() == SPOT_POND);
}

/* ============ 2. 门槛边界：7 条还锁着，8 条正好开 ============ */
static void case_river_threshold(void) {
    printf("[2] 急流河门槛 = %d 条\n", RIVER_NEED);
    set_total(RIVER_NEED - 1);
    fishing_set_spot(SPOT_POND);
    check("钓 7 条：急流河仍锁着", !fishing_spot_unlocked(SPOT_RIVER));
    check("钓 7 条：还差 1 条", fishing_spot_unlock_left(SPOT_RIVER) == 1);
    check("钓 7 条：跳不动", fishing_cycle_spot(1) == false);

    set_total(RIVER_NEED);
    fishing_set_spot(SPOT_POND);
    check("钓 8 条：急流河解锁", fishing_spot_unlocked(SPOT_RIVER));
    check("下一个待解锁变成深海", fishing_next_locked_spot() == SPOT_SEA);
    check("往下换成功 -> true", fishing_cycle_spot(1) == true);
    check("换到了急流河", spot_now() == SPOT_RIVER);

    /* 深海还锁着的时候再往下换，只能在已解锁的两个之间循环 */
    check("再往下换回到静水塘", fishing_cycle_spot(1) == true && spot_now() == SPOT_POND);
    check("往上换到急流河", fishing_cycle_spot(-1) == true && spot_now() == SPOT_RIVER);
    check("深海始终进不去", !fishing_spot_unlocked(SPOT_SEA));
}

/* ============ 3. 全解锁：三个钓点双向循环 ============ */
static void case_all_unlocked(void) {
    printf("[3] 深海门槛 = %d 条，全解锁后循环\n", SEA_NEED);
    set_total(SEA_NEED - 1);
    check("钓 19 条：深海仍锁着", !fishing_spot_unlocked(SPOT_SEA));

    set_total(SEA_NEED);
    fishing_set_spot(SPOT_POND);
    check("钓 20 条：深海解锁", fishing_spot_unlocked(SPOT_SEA));
    check("没有待解锁钓点了 -> -1", fishing_next_locked_spot() == -1);
    check("深海还差 0 条", fishing_spot_unlock_left(SPOT_SEA) == 0);

    check("静水塘 -> 急流河", fishing_cycle_spot(1) == true && spot_now() == SPOT_RIVER);
    check("急流河 -> 深海", fishing_cycle_spot(1) == true && spot_now() == SPOT_SEA);
    check("深海 -> 静水塘（绕回）", fishing_cycle_spot(1) == true && spot_now() == SPOT_POND);
    check("静水塘 -> 深海（反着绕）", fishing_cycle_spot(-1) == true && spot_now() == SPOT_SEA);
    check("深海 -> 急流河", fishing_cycle_spot(-1) == true && spot_now() == SPOT_RIVER);
}

/* ============ 4. 鱼竿 / 饵料：无门槛，双向循环 ============ */
static void case_rod_and_bait(void) {
    printf("[4] 鱼竿 / 饵料循环\n");
    fishing_set_rod(ROD_HAND);
    fishing_set_bait(BAIT_WORM);

    check("竿 手竿 -> 路亚竿", fishing_cycle_rod(1) == true && rod_now() == ROD_LURE);
    check("竿 路亚竿 -> 海竿", fishing_cycle_rod(1) == true && rod_now() == ROD_SEA);
    check("竿 海竿 -> 手竿（绕回）", fishing_cycle_rod(1) == true && rod_now() == ROD_HAND);
    check("竿 手竿 -> 海竿（反向）", fishing_cycle_rod(-1) == true && rod_now() == ROD_SEA);

    check("饵 蚯蚓 -> 面团", fishing_cycle_bait(1) == true && bait_now() == BAIT_DOUGH);
    check("饵 面团 -> 亮片", fishing_cycle_bait(1) == true && bait_now() == BAIT_LURE);
    check("饵 亮片 -> 蚯蚓（绕回）", fishing_cycle_bait(1) == true && bait_now() == BAIT_WORM);
    check("饵 蚯蚓 -> 亮片（反向）", fishing_cycle_bait(-1) == true && bait_now() == BAIT_LURE);
}

/* ============ 5. 越界枚举不能读崩 ============ */
static void case_bad_index(void) {
    printf("[5] 越界入参\n");
    check("unlock_left(-1) = 0", fishing_spot_unlock_left((spot_t)-1) == 0);
    check("unlock_left(99) = 0", fishing_spot_unlock_left((spot_t)99) == 0);
    check("unlocked(-1) = false", !fishing_spot_unlocked((spot_t)-1));
    check("unlocked(99) = false", !fishing_spot_unlocked((spot_t)99));
}

int main(void) {
    case_fresh_only_pond();
    case_river_threshold();
    case_all_unlocked();
    case_rod_and_bait();
    case_bad_index();

    if (g_fail) {
        printf("\n%d CHECK(S) FAILED\n", g_fail);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
