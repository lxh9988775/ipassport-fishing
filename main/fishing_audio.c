/*
 * fishing_audio.c - 音效实现（代码生成 16bit PCM，走官方 BSP 播放）
 *
 * 受限于无 PSRAM + 8MB，不加载任何音频素材，全部用 C 生成短促正弦波 PCM，
 * 交给官方 BSP 的音频接口播放。零资源占用。
 *
 * 已对照 components/bsp/include/bsp_audio.h 校准：
 *   bsp_audio_set_format(hz, bits, ch)
 *   bsp_audio_set_volume(percent)
 *   bsp_audio_write(const void *pcm, size_t bytes)  // bytes = 采样数 * 2
 * bsp_audio_init() 由 main.c 在 fishing_app_start() 之前完成。
 */

#include "fishing_audio.h"
#include "bsp_audio.h"

#include <math.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE 16000u
#define AUDIO_MAX_SAMPLES 2048

/* 生成一段正弦波 PCM 到 buf，返回采样数 */
static size_t gen_tone(int16_t *buf, size_t max_samples, float freq,
                       float dur_ms, float vol) {
    size_t n = (size_t)(AUDIO_SAMPLE_RATE * dur_ms / 1000.0f);
    if (n > max_samples) n = max_samples;
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / (float)AUDIO_SAMPLE_RATE;
        /* 简单包络，避免爆音 */
        float env = (i < n / 10) ? (10.0f * (float)i / (float)n) : 1.0f;
        if (i > n - n / 10) env = 10.0f * (float)(n - i) / (float)n;
        buf[i] = (int16_t)(vol * env * 32767.0f * sinf(2.0f * 3.14159265f * freq * t));
    }
    return n;
}

/* 设置采样格式 + 音量（bsp_audio_init 已在 main 完成） */
void fishing_audio_init(void) {
    bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1);
    bsp_audio_set_volume(70);
}

void fishing_audio_play(sfx_t sfx) {
    static int16_t buf[AUDIO_MAX_SAMPLES];
    size_t n = 0;
    switch (sfx) {
        case SFX_BITE:
            n = gen_tone(buf, AUDIO_MAX_SAMPLES, 1760.0f, 120.0f, 0.5f);
            break;
        case SFX_CATCH:
            n = gen_tone(buf, AUDIO_MAX_SAMPLES, 880.0f, 90.0f, 0.5f);
            break;
        case SFX_MISS:
            n = gen_tone(buf, AUDIO_MAX_SAMPLES, 330.0f, 160.0f, 0.45f);
            break;
        case SFX_CLICK:
            n = gen_tone(buf, AUDIO_MAX_SAMPLES, 1200.0f, 40.0f, 0.3f);
            break;
        default:
            return;
    }
    if (n > 0) {
        bsp_audio_write(buf, n * sizeof(int16_t));  /* bytes 不是采样数 */
    }
}
