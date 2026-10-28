/*
 * DreamState: 听觉+视觉联动闭环
 * BSP 驱动 LCD (LVGL) + ES8311 麦克风
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp/esp32_s3_eye.h"
#include "esp_codec_dev.h"

static const char *TAG = "dreamstate";

#define BUFFER_SAMPLES  512
#define RMS_QUIET       20000
#define RMS_NOISE       25000
#define REFRESH_MS      200

/* 只计算左声道 RMS（立体声交错: L,R,L,R,...） */
static float calc_rms_left(const int16_t *buf, int total_samples)
{
    float sum = 0;
    int cnt = 0;
    for (int i = 0; i < total_samples; i += 2) {
        float v = (float)buf[i];
        sum += v * v;
        cnt++;
    }
    return cnt > 0 ? sqrtf(sum / cnt) : 0;
}

void app_main(void)
{
    printf("\n=== DreamState Audio-Visual Feedback ===\n\n");
    fflush(stdout);

    /* ========================================================
     * 1. 初始化显示 + LVGL (BSP 自动引脚)
     * ======================================================== */
    ESP_LOGI(TAG, "Init display...");
    lv_display_t *disp = bsp_display_start();
    assert(disp);
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Display ready");

    /* ========================================================
     * 2. 初始化音频 ES8311 (BSP 自动引脚: SCLK=41, LCLK=42, DIN=2)
     * ======================================================== */
    ESP_LOGI(TAG, "Init audio...");
    esp_err_t ret = bsp_audio_init(NULL);   // 默认 48kHz, 16bit, mono
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(ret));
        return;
    }
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (!mic) {
        ESP_LOGE(TAG, "Microphone init failed");
        return;
    }
    ESP_LOGI(TAG, "Audio ready (48kHz, 16bit)");

    /* ========================================================
     * 3. 创建 LVGL UI 控件
     * ======================================================== */
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    /* 大字状态（居中偏上） */
    lv_obj_t *label_status = lv_label_create(scr);
    lv_label_set_text(label_status, "Listening...");
    lv_obj_set_style_text_color(label_status, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, -20);

    /* 小字 RMS 数值（居中偏下） */
    lv_obj_t *label_rms = lv_label_create(scr);
    lv_label_set_text(label_rms, "RMS: 0");
    lv_obj_set_style_text_color(label_rms, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_rms, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_rms, LV_ALIGN_CENTER, 0, 20);

    /* 初始背景：深蓝 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000040), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ========================================================
     * 4. 音频循环：读 I2S → 算 RMS → 每 200ms 刷新 UI
     * ======================================================== */
    int16_t *buf = malloc(BUFFER_SAMPLES * sizeof(int16_t));
    assert(buf);

    bool   last_noise  = false;
    TickType_t last_update = 0;
    ESP_LOGI(TAG, "Loop running (quiet <%d, noise >%d)...", RMS_QUIET, RMS_NOISE);

    while (1) {
        int bytes = esp_codec_dev_read(mic, buf, BUFFER_SAMPLES * sizeof(int16_t));

        if (bytes > 0) {
            int total = bytes / 2;
            float rms = calc_rms_left(buf, total);

            /* 200ms 刷新一次 UI，避免闪烁 + LVGL 过载 */
            TickType_t now = xTaskGetTickCount();
            if ((now - last_update) >= pdMS_TO_TICKS(REFRESH_MS)) {
                last_update = now;

                bool is_noise = (rms > RMS_NOISE);

                bsp_display_lock(0);

                if (is_noise) {
                    /* 噪音状态：深红背景 */
                    lv_obj_set_style_bg_color(scr, lv_color_hex(0x400000), LV_PART_MAIN);
                    lv_label_set_text(label_status, "Noise!");
                } else {
                    /* 安静状态：深蓝背景 */
                    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000040), LV_PART_MAIN);
                    lv_label_set_text(label_status, "Listening...");
                }

                char rms_buf[32];
                snprintf(rms_buf, sizeof(rms_buf), "RMS: %.0f", rms);
                lv_label_set_text(label_rms, rms_buf);

                bsp_display_unlock();

                /* 状态切换时打印日志 */
                if (is_noise != last_noise) {
                    if (is_noise) {
                        ESP_LOGI(TAG, ">>> NOISE! RMS=%.0f", rms);
                        printf("[模拟] 降低音量，停止香氛\n");
                        fflush(stdout);
                    } else {
                        ESP_LOGI(TAG, "<<< Quiet  RMS=%.0f", rms);
                    }
                }
                last_noise = is_noise;
            }
        } else {
            ESP_LOGW(TAG, "Audio read: %d bytes", bytes);
        }

        vTaskDelay(pdMS_TO_TICKS(10));   /* 10ms 间隔读取音频 */
    }
}