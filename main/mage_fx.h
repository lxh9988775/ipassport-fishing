/* 魔法帝之路 —— 战斗演出层接口（1:1 复刻 HTML 实时战斗演出） */
#ifndef MAGE_FX_H
#define MAGE_FX_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 粒子类型 */
#define FX_K_PARTICLE 0
#define FX_K_RING     1
#define FX_K_CIRCLE   2
#define FX_K_CHARGE   3

/* 把演出层挂到指定父对象（一般是战斗专用容器），只做一次对象池构建 */
void mage_fx_attach(lv_obj_t *parent);

/* 演出层是否已构建 */
bool mage_fx_ready(void);

/* 进入/离开战斗时显示或隐藏（离开必须隐藏，否则会盖在后续页面上持续重绘） */
void mage_fx_show(bool on);

/* 探索事件面板：show_kind = 0 宝箱 / 1 陷阱 / 2 泉水 / 3 药草；-1 隐藏
 * 1:1 复刻 HTML drawEventPanel（金边面板 + 图形 + 名称 + "C 绕开"） */
void mage_fx_event(int show_kind);

/* 每波结算面板：1:1 复刻 HTML drawResult —— box(20,150,W-40,150)，
 * 金标题 + #cfc 明细多行 + 底部金字操作提示。show<=0 隐藏。 */
void mage_fx_result(int show, const char *title, const char *footer, const char **lines, int n);

/* 远征/BOSS 汇总面板：1:1 复刻 HTML drawExpoSummary —— box(14,40,W-28,250)
 * 上下 2px 金条、✦ 标题、「XX 已肃清」副题、名目左对齐/数值右对齐收益行、
 * ★ BOSS 战利品块（extra 多行）、底部提示。rowsL/rowsR 为 '\n' 分隔的等行数文本。 */
void mage_fx_expo(int show, const char *title, const char *sub,
                  const char *rowsL, const char *rowsR, int nrows,
                  const char **extra, int nextra, const char *footer);

/* 宝箱/采集战利品面板：1:1 复刻 HTML drawOverlay 的 lootShow —— 顶部小面板
 * box(28,54,W-56)，无底部提示（HTML 3 秒自动消失，C 端 battle_tick 同步 3 秒自动继续）。 */
void mage_fx_loot(int show, const char *title, const char *footer, const char **lines, int n);

/* 魔法/道具选择列表面板：复用 evtRoot/evtBox 的单列多行，▶ 标当前选中项，
 * 与城镇菜单一致的选中高亮语义。show<=0 隐藏。 */
void mage_fx_choices(int show, const char *title, const char **lines, int n, const char *footer);

/* 每帧推进时间轴（背景滚动、跳跃、粒子、突进、受击），dt 单位秒 */
void mage_fx_tick(float dt);

/* 每帧绘制：只改对象属性，不新建对象（ESP32-C3 防闪屏铁律） */
void mage_fx_render(void);

/* 生成一个粒子 */
void mage_fx_spawn(int kind, float x, float y, int color, float r, float life);

/* 生成一条伤害飘字（1:1 复刻 HTML bt.nums）
 * worldX/worldY 为世界坐标（会减去 cam），value 为伤害数字，color 为字色。
 * 英雄受击传 (BTPX, 200)；敌人受击传 (e->x, 250-34)。 */
void mage_fx_num(int worldX, int worldY, int value, uint32_t color);

#ifdef __cplusplus
}
#endif
#endif
