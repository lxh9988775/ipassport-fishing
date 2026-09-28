/*
 * fishing_audio.h - 钓鱼音效接口
 *
 * 无 PSRAM 约束下，音效不加载音频文件，而是用代码生成短促 PCM 正弦波，
 * 通过官方 BSP 音频接口播放，零资源占用。
 */

#ifndef FISHING_AUDIO_H
#define FISHING_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音效类型 */
typedef enum {
    SFX_BITE  = 0, /* 咬钩："叮"（高频短促）*/
    SFX_CATCH = 1, /* 钓上：中音 */
    SFX_MISS  = 2, /* 跑鱼：低音 */
    SFX_CLICK = 3  /* 菜单点击 */
} sfx_t;

/* 初始化音频（设置采样格式 + 音量；bsp_audio_init 已由 main 调用）*/
void fishing_audio_init(void);

/* 播放一个音效（PCM 在内部生成并送入音频接口）*/
void fishing_audio_play(sfx_t sfx);

#ifdef __cplusplus
}
#endif

#endif /* FISHING_AUDIO_H */
