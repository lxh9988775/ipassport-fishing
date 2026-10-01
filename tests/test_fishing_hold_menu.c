/*
 * test_fishing_hold_menu.c - 「长按 OK 开菜单」单元测试（纯 C，无需硬件）
 *
 * 为什么专门给这几行加单测：
 *   社区审核就是因为这条被打回的 —— 屏幕上明明写着「长按菜单」，实际按下去
 *   只会抛竿，菜单永远打不开。根因是两层叠加：
 *     1) 长按走的是 BSP 的 LONG_PRESS_START 事件，而按下那一瞬间
 *        BSP_BTN_PRESS 先到，IDLE 下 fishing_cast() 已经把状态推成 CASTING；
 *        1.5 秒后 LONG 事件到达时，调用点的 st.state == STATE_IDLE 守卫必然为假；
 *     2) fishing_enter_menu() 原先也只认 STATE_IDLE，等于第二道门也锁死。
 *   现在改成【每帧喂电平】的 fishing_hold_sample()（阈值 700ms），
 *   fishing_enter_menu() 放开 CASTING/WAITING/BITE 三态。
 *
 * 这个文件钉住四件事，任何一条被改回去都会红：
 *   A. 抛竿之后（状态已经是 CASTING/WAITING）长按仍然能进菜单 —— 用例 1/2；
 *   B. 长按到点只报一次，手指不松不会"开了又被自己关掉" —— 用例 4；
 *   C. 收线中 fishing_enter_menu() 不生效（正在对局，不能白送一条鱼）—— 用例 7；
 *   D. 判定为长按之后，fishing_hold_fired() 必须活到松手之后 —— 用例 9~11。
 *      否则 iot_button 在 0.7~1.5 秒区间补发的 SINGLE_CLICK 会被菜单当成
 *      「OK 确认」，菜单一闪即关还顺手抛竿（模拟器实测抓到的第二个 bug）。
 *
 * 帧长跟主循环走：非收线页面 20ms 一帧（见 fishing.c 的 LOOP_IDLE_MS），
 * 阈值与帧长的组合关系改了就重算这里的期望值。
 *
 * 编译运行（需 cc）：
 *   cc -std=c11 -Wall -Wextra -Werror \
 *      tests/test_fishing_hold_menu.c main/fishing_logic.c -o /tmp/hold_menu
 *   /tmp/hold_menu
 */

#include <stdio.h>
#include <stdlib.h>
#include "fishing_logic.h"

#define FRAME_MS   20    /* 非收线页面主循环 20ms 一帧，与 fishing.c 的 LOOP_IDLE_MS 一致 */
#define LONG_MS    700   /* 与 fishing_logic.c 的 HOLD_LONG_MS 一致 */

static int  g_now;
static int  g_fail;
static bool g_fired_this_frame;

static int state_now(void) {
    fishing_status_t st;
    fishing_get_status(&st);
    return (int)st.state;
}

/* 喂一帧：先给电平，再推进一个主循环周期（顺序与 game_task 里一致）。
 * fired 记录本帧是否报了长按 —— UI 层就是拿这个返回值去 handle_long_press()。 */
static void frame(bool ok_down) {
    g_fired_this_frame = fishing_hold_sample(ok_down, g_now);
    g_now += FRAME_MS;
    fishing_tick(g_now);
}

/* 按住 ok_down 保持 ms 毫秒，返回这期间"触发过几次长按" */
static int hold_for(int ms, bool ok_down) {
    int fired = 0;
    for (int t = 0; t < ms; t += FRAME_MS) {
        frame(ok_down);
        if (g_fired_this_frame) fired++;
    }
    return fired;
}

static void reset_world(void) {
    g_now = 0;
    g_fired_this_frame = false;
    /* 注意：这里【不能】清 g_fail —— 每个用例开头都会 reset，
     * 清了就等于把前面用例的失败一起抹掉，最后永远打印 PASS。 */
    fishing_init(20261001u);
    fishing_set_spot(SPOT_POND);
    fishing_set_rod(ROD_HAND);
    fishing_set_bait(BAIT_WORM);
    fishing_hold_reset();
}

static void check(const char *name, bool ok) {
    printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    if (!ok) g_fail++;
}

/* UI 层 handle_long_press() 的分发规则（钓场三态 -> 开菜单）在这里复刻一遍，
 * 用真状态机跑，确保"从 IDLE 按住 700ms 之后状态已经不是 IDLE"这个陷阱被覆盖。 */
static void ui_long_press_dispatch(void) {
    switch (state_now()) {
        case STATE_IDLE:
        case STATE_CASTING:
        case STATE_WAITING:
        case STATE_BITE:
            fishing_enter_menu();
            break;
        default:
            break;
    }
}

/* ============ 1. 回归：抛竿后长按仍能进菜单（审核打回的那条） ============ */
static void case_cast_then_hold_opens_menu(void) {
    reset_world();
    check("初始在钓场待机", state_now() == STATE_IDLE);

    fishing_cast();                       /* 按下 OK 的那一瞬间：抛竿立即生效 */
    check("按下 OK 立刻抛竿（状态离开 IDLE）", state_now() == STATE_CASTING);

    int fired = 0;
    for (int t = 0; t < LONG_MS + 200; t += FRAME_MS) {
        frame(true);                      /* 手指不松 */
        if (g_fired_this_frame) { fired++; ui_long_press_dispatch(); }
    }
    check("抛竿后长按 700ms 触发一次", fired == 1);
    check("长按后进入菜单（审核打回的就是这条）", state_now() == STATE_MENU);
}

/* ============ 2. 抛竿动画跑完（已进 WAITING）再长按，同样要能进菜单 ============ */
static void case_waiting_then_hold_opens_menu(void) {
    reset_world();
    fishing_cast();
    for (int t = 0; t < 700; t += FRAME_MS) frame(true);   /* 600ms 动画 + 余量 */
    check("抛竿动画结束进入等鱼", state_now() == STATE_WAITING);

    int fired = hold_for(LONG_MS + 100, true);   /* hold_for 内已按帧计数 */
    check("等鱼时长按触发一次", fired == 1);
    ui_long_press_dispatch();
    check("等鱼时长按能进菜单", state_now() == STATE_MENU);
}

/* ============ 3. 短按不能误触发（抛竿是高频操作，一次都不能误判） ============ */
static void case_short_tap_never_fires(void) {
    reset_world();
    for (int rep = 0; rep < 20; ++rep) {       /* 连点 20 次 */
        int fired = hold_for(180, true);       /* 一次点按约 180ms */
        frame(false);                          /* 松手 */
        if (fired != 0) { check("短按绝不能触发长按", false); return; }
    }
    check("连点 20 次短按都没有误触发长按", state_now() == STATE_IDLE);
}

/* ============ 4. 触发一次后，手指不松不再重复报 ============ */
static void case_fires_exactly_once(void) {
    reset_world();
    int fired = hold_for(3000, true);          /* 一直按住 3 秒 */
    check("按住 3 秒只报一次（不会开了又被自己关掉）", fired == 1);
    frame(false);                              /* 松手 */
    check("松手后不报", !g_fired_this_frame);

    fired = hold_for(LONG_MS + 100, true);     /* 松手后重新按 */
    check("松手后重新按住能再报一次", fired == 1);
}

/* ============ 5. 阈值边界：699ms 不报、700ms 报 ============ */
static void case_threshold_boundary(void) {
    reset_world();
    /* 取样的时间戳是"喂电平那一刻的 g_now"，帧尾才 +FRAME_MS：
     * 起点在 0，所以最后一次"还没到点"的取样落在 680ms，下下帧才到 700ms。 */
    int fired = 0;
    while (g_now < 700 - FRAME_MS) {
        frame(true);
        if (g_fired_this_frame) fired++;
    }
    check("按住 660ms 还不触发", fired == 0);

    frame(true);                               /* 取样在 680ms */
    check("按住 680ms 还不触发", !g_fired_this_frame);

    frame(true);                               /* 取样在 700ms，正好到点 */
    check("按住满 700ms 触发", g_fired_this_frame);
}

/* ============ 6. fishing_hold_reset() 清零计时（收线期间每帧调用） ============ */
static void case_reset_clears_timer(void) {
    reset_world();
    hold_for(500, true);                       /* 已经按了 500ms */
    fishing_hold_reset();                      /* 收线页面把它清掉 */
    int fired = hold_for(600, true);           /* 重新计时，先按 600ms */
    check("reset 后 600ms 还不算长按", fired == 0);

    int more = hold_for(200, true);
    check("reset 后重新计满 700ms 才触发", more == 1);
}

/* ============ 7. 收线中 / 结算中不给进菜单 ============ */
static void case_reeling_rejects_menu(void) {
    reset_world();
    fishing_cast();
    while (g_now < 200000) {                   /* 推进到咬钩 */
        g_now += FRAME_MS;
        if (fishing_tick(g_now) == EVT_BITE_START) break;
    }
    check("推进到咬钩窗口", state_now() == STATE_BITE);
    g_now += 50;
    fishing_strike();
    check("提竿后进入收线", state_now() == STATE_REELING);

    fishing_enter_menu();
    check("收线中 fishing_enter_menu() 不生效", state_now() == STATE_REELING);

    /* 收线走完（CATCH/ESCAPE 结算页同样不给进） */
    while (g_now < 200000) {
        g_now += 20;
        fishing_event_t e = fishing_tick(g_now);
        if (e == EVT_BACK_IDLE) break;
    }
    check("对局结束后回到待机", state_now() == STATE_IDLE);
}

/* ============ 8. 菜单里长按 = 返回钓场 ============ */
static void case_menu_long_press_exits(void) {
    reset_world();
    fishing_enter_menu();
    check("能进菜单", state_now() == STATE_MENU);

    int fired = hold_for(LONG_MS + 100, true);
    check("菜单里长按触发一次", fired == 1);
    fishing_exit_menu();                       /* UI 层在 MENU 下的分发 */
    check("长按后退出菜单回到钓场", state_now() == STATE_IDLE);
}

/* ============ 9. 松手后标记仍为 true —— 供 UI 吞掉迟到的单击 ============
 *
 * 这是模拟器实测抓到的第二个 bug：iot_button 只在按满 1.5 秒时才走长按分支，
 * 玩家按 0.7~1.5 秒松手时，组件眼里是一次"普通点击"，于是补发 SINGLE_CLICK。
 * 长按刚开出来的菜单会把这个迟到的单击当成「OK 确认」，落在第 0 项
 * 「开始钓鱼」上 —— 菜单一闪即关还顺手抛竿。
 * 所以判定为长按之后，标记必须【活到松手之后】，让 UI 有机会认出并吞掉它。 */
static void case_fired_latches_until_next_press(void) {
    reset_world();
    check("初始：没有待吞掉的单击", !fishing_hold_take_stale_click());

    int fired = hold_for(LONG_MS + 100, true);
    check("长按触发一次", fired == 1);

    frame(false);                                            /* 松手 */
    check("松手后那个迟到的单击会被吞掉（标记活过了松手）",
          fishing_hold_take_stale_click());
    check("只吞一次，紧接着的单击正常放行", !fishing_hold_take_stale_click());

    /* 没有单击补发的情况：下一次按下要自动复位 */
    hold_for(LONG_MS + 100, true);
    frame(false);
    frame(true);                                             /* 下一次按下 */
    check("下一次按下之后不再误吞（玩家真实点按不受影响）",
          !fishing_hold_take_stale_click());
}

/* ============ 10. 短按不能留下"待吞掉的单击" ============ */
static void case_short_tap_leaves_no_pending_click(void) {
    reset_world();
    hold_for(400, true);
    frame(false);
    check("短按松手后没有待吞掉的单击（普通点按不受影响）",
          !fishing_hold_take_stale_click());
}

/* ============ 11. fishing_hold_reset() 也要清掉待吞标记 ============ */
static void case_reset_clears_pending_click(void) {
    reset_world();
    hold_for(LONG_MS + 100, true);
    fishing_hold_reset();
    check("reset 后待吞标记一并清掉", !fishing_hold_take_stale_click());
}

int main(void) {
    case_cast_then_hold_opens_menu();
    case_waiting_then_hold_opens_menu();
    case_short_tap_never_fires();
    case_fires_exactly_once();
    case_threshold_boundary();
    case_reset_clears_timer();
    case_reeling_rejects_menu();
    case_menu_long_press_exits();
    case_fired_latches_until_next_press();
    case_short_tap_leaves_no_pending_click();
    case_reset_clears_pending_click();

    if (g_fail) {
        printf("\n%d CHECK(S) FAILED\n", g_fail);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
