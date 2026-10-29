/*
 * 白噪音播放器 — 实时 LFSR 噪声生成，无限循环无毛刺
 *
 * I2S_NUM_1: BCLK=18, LRC=19, DIN=14  → MAX98357
 * 16-bit 立体声 (mono 复制到双声道), 48kHz
 * 音量通过 hal_audio_volume 实时控制 (0.0 ~ 1.0)
 */

#include "audio_player.h"
#include "hal_hardware.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

#define SAMPLE_RATE        48000
#define CHUNK_SAMPLES      512     /* 每次写入 512 采样/声道 */
#define STEREO_SAMPLES     (CHUNK_SAMPLES * 2)

static const char *TAG = "audio";

/* 16-bit LFSR 白噪音 — 经典 (15,14,13,4) 反馈多项式 */
static uint16_t lfsr = 0xACE1;

static inline int16_t white_noise_sample(void)
{
    uint16_t bit = (lfsr ^ (lfsr >> 1) ^ (lfsr >> 2) ^ (lfsr >> 11)) & 1;
    lfsr = (lfsr >> 1) | (bit << 15);
    /* 映射到 ±8192 范围（避免削顶，留余量给音量变化） */
    return (int16_t)((lfsr & 0x3FFF) - 0x1FFF);
}

/* ==========================================================
 * 播放任务
 * ========================================================== */
static void audio_task(void *arg)
{
    /* 立体声缓冲区: L,R,L,R,... 逐对写入 */
    int16_t *stereo_buf = malloc(STEREO_SAMPLES * sizeof(int16_t));
    if (!stereo_buf) {
        ESP_LOGE(TAG, "Failed to allocate stereo buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "White noise playback started on I2S_NUM_1");
    TickType_t last_log = 0;

    while (1) {
        /* 生成一帧白噪音 + 音量缩放 */
        float vol = hal_audio_volume;
        for (int i = 0; i < CHUNK_SAMPLES; i++) {
            int16_t s = white_noise_sample();
            int16_t scaled = (int16_t)((float)s * vol);
            stereo_buf[i * 2]     = scaled;  /* L */
            stereo_buf[i * 2 + 1] = scaled;  /* R */
        }

        /* 写入 I2S */
        size_t written = 0;
        esp_err_t ret = i2s_channel_write(hal_i2s_tx, stereo_buf,
                                          STEREO_SAMPLES * sizeof(int16_t),
                                          &written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S write: %d (written=%d)", ret, written);
        }

        /* 每 5 秒打印一次状态 */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_log) >= pdMS_TO_TICKS(5000)) {
            last_log = now;
            printf("[Audio] white noise running | volume=%.2f\n", hal_audio_volume);
            fflush(stdout);
        }
    }

    free(stereo_buf);
    vTaskDelete(NULL);
}

/* ==========================================================
 * 公开接口
 * ========================================================== */
void audio_player_start(void)
{
    if (!hal_i2s_tx) {
        ESP_LOGE(TAG, "I2S not initialized — call hal_hardware_init() first");
        return;
    }

    /* 创建高优先级播放任务，栈 3KB */
    BaseType_t ret = xTaskCreate(audio_task, "audio_play", 3072,
                                 NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
    }
}