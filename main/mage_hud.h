#ifndef MAGE_HUD_H
#define MAGE_HUD_H

#include "lvgl.h"
#include <stdbool.h>

/* 战斗 HUD：1:1 复刻 HTML index.html drawBattleHUD() + drawActionCards()
 * 顶部 26px 血条面板 + 底部 44px 三张动作卡，中间纯画面零文字。 */
void mage_hud_attach(lv_obj_t *parent);
void mage_hud_build(void);
void mage_hud_show(bool on);
void mage_hud_cards_show(bool on);   /* 底部三卡+回合徽标独立显隐（非交互战斗阶段藏卡） */
void mage_hud_update(int hp, int maxHp, int mp, int maxMp,
                     int wave, int waveMax, bool crawl,
                     bool autoMode, bool autoCap, int loadout,
                     int orbs, int seals, int potions, int turn);
bool mage_hud_ready(void);

#endif /* MAGE_HUD_H */
