/* 本文件由 tools/gen_mage_sprites.py 自动生成，请勿手改。 */
#ifndef MAGE_SPRITES_H
#define MAGE_SPRITES_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char            *name;
    uint16_t               w;
    uint16_t               h;
    const lv_image_dsc_t  *dsc;
} mage_sprite_t;

extern const mage_sprite_t mage_sprites[];
extern const int mage_sprite_count;

#ifdef __cplusplus
}
#endif
#endif
