/*
 * 森林声景播放器 — 循环播放嵌入的 PCM 音频 (EMBED_FILES)
 *
 * I2S_NUM_1: BCLK=27, LRC=26, DIN=14  → MAX98357
 * 输入: 16kHz mono 16-bit → 上采样至 48kHz 立体声
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

/* EMBED_FILES 生成的符号 */
extern const uint8_t forest_audio_wav_start[] asm("_binary_forest_audio_wav_start");
extern const uint8_t forest_audio_wav_end[]   asm("_binary_forest_audio_wav_end");

#define WAV_HEADER_SIZE   44
#define SAMPLE_RATE_IN    16000
#define SAMPLE_RATE_OUT   48000
#define CHUNK_SAMPLES     512
#define STEREO_SAMPLES    (CHUNK_SAMPLES * 2)

static const char *TAG = "audio";

/* ==========================================================
 * 播放任务：从嵌入的 WAV PCM → 上采样 → I2S
 * ========================================================== */
static void audio_task(void *arg)
{
    int16_t *stereo_buf = malloc(STEREO_SAMPLES * sizeof(int16_t));
    if (!stereo_buf) {
        ESP_LOGE(TAG, "Failed to allocate stereo buffer");
        vTaskDelete(NULL);
        return;
    }

    /* 跳过 WAV 头，直接访问 16-bit PCM */
    const int16_t *pcm = (const int16_t *)(forest_audio_wav_start + WAV_HEADER_SIZE);
    uint32_t pcm_len = (uint32_t)(forest_audio_wav_end - forest_audio_wav_start - WAV_HEADER_SIZE);
    uint32_t num_samples = pcm_len / 2;

    uint32_t audio_pos = 0;
    TickType_t last_log = 0;

    ESP_LOGI(TAG, "Forest soundscape: %lu samples @ %dHz (%lu bytes)",
             num_samples, SAMPLE_RATE_IN, pcm_len);

    while (1) {
        float vol = hal_audio_volume;

        for (int i = 0; i < CHUNK_SAMPLES; i++) {
            /* 48kHz 输出位置 → 16kHz 源位置 */
            uint32_t in_idx = (uint32_t)(((uint64_t)audio_pos * SAMPLE_RATE_IN) / SAMPLE_RATE_OUT);
            in_idx = in_idx % num_samples;

            int16_t s = pcm[in_idx];
            int16_t scaled = (int16_t)((float)s * vol);
            stereo_buf[i * 2]     = scaled;
            stereo_buf[i * 2 + 1] = scaled;
            audio_pos++;
        }

        size_t written = 0;
        esp_err_t ret = i2s_channel_write(hal_i2s_tx, stereo_buf,
                                          STEREO_SAMPLES * sizeof(int16_t),
                                          &written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S write: %d (written=%d)", ret, written);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log) >= pdMS_TO_TICKS(12000)) {
            last_log = now;
            printf("[Audio] forest soundscape | vol=%.2f | pos=%lu\n",
                   hal_audio_volume, audio_pos);
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

    BaseType_t ret = xTaskCreate(audio_task, "audio_play", 3072,
                                 NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
    }
}