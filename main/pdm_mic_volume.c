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
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

static const char *TAG = "dreamstate";

#define BUFFER_SAMPLES  512
#define RMS_QUIET        50      /* 低于此 = 安静 */
#define RMS_NOISE       100     /* 高于此 = 拍手噪音 */
#define REFRESH_MS       200
#define COOLDOWN_MS     5000    /* 噪音结束后需安静 5 秒才恢复 */

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

    /* 1a. 先初始化 I2C + 手动创建 I2S（不启用）*/
    ESP_LOGI(TAG, "Init I2C...");
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = 0,
        .sda_io_num = GPIO_NUM_4,
        .scl_io_num = GPIO_NUM_5,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));
    ESP_LOGI(TAG, "I2C bus created");

    /* 手动创建 I2S RX 通道（不 enable，由 esp_codec_dev_open 启用）*/
    ESP_LOGI(TAG, "Init I2S (not enabled yet)...");
    i2s_chan_handle_t i2s_rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,    /* ESP32-S3-EYE 无 MCLK */
            .bclk = GPIO_NUM_41,
            .ws   = GPIO_NUM_42,
            .dout = GPIO_NUM_NC,
            .din  = GPIO_NUM_2,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx, &std_cfg));
    /* 显式启用 I2S，让 BCLK/LRCK 在 ES8311 配置前就存在 */
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx));
    ESP_LOGI(TAG, "I2S enabled with clocks running");

    /* 创建 I2S data interface */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = i2s_rx,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    assert(data_if);

    /* 再探测 ES8311（7-bit 多种可能地址）*/
    uint8_t es8311_addr = 0x18;  /* 默认地址 */
    esp_err_t probe = i2c_master_probe(i2c_bus, es8311_addr, 20);
    ESP_LOGI(TAG, "ES8311 probe @0x%02X: %s", es8311_addr,
             probe == ESP_OK ? "FOUND" : "NOT FOUND");

    if (probe != ESP_OK) {
        /* 尝试 0x12 — 某些板子 ES8311 CE 引脚接法不同 */
        es8311_addr = 0x12;
        probe = i2c_master_probe(i2c_bus, es8311_addr, 20);
        ESP_LOGI(TAG, "ES8311 probe @0x%02X: %s", es8311_addr,
                 probe == ESP_OK ? "FOUND" : "NOT FOUND");
    }
    if (probe != ESP_OK) {
        printf("ES8311 not found on I2C bus, no audio available.\n");
        fflush(stdout);
    }

    esp_codec_dev_handle_t mic = NULL;
    if (probe == ESP_OK) {
        /* ---- ES8311 在线：完整 I2C 初始化 ---- */
        ESP_LOGI(TAG, "ES8311 online @0x%02X, full I2C init...", es8311_addr);

        audio_codec_i2c_cfg_t i2c_cfg = {
            .addr = (es8311_addr << 1), /* 转 8-bit 地址 */
            .bus_handle = i2c_bus,
        };
        const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        assert(ctrl_if);

        const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

        es8311_codec_cfg_t es8311_cfg = {
            .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
            .ctrl_if     = ctrl_if,
            .gpio_if     = gpio_if,
            .pa_pin      = -1,
            .use_mclk    = false,
            .master_mode = false,
            .hw_gain     = {},
        };
        const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
        assert(codec_if);

        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = codec_if,
            .data_if  = data_if,
        };
        mic = esp_codec_dev_new(&dev_cfg);
        assert(mic);

        esp_codec_dev_set_in_gain(mic, 30.0);
        ESP_LOGI(TAG, "ES8311 ADC initialized via I2C");
    } else {
        /* ---- ES8311 不在线 ---- */
        ESP_LOGE(TAG, "ES8311 not found on I2C, cannot continue");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 48000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    esp_err_t ret = esp_codec_dev_open(mic, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "Audio ready");

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

    typedef enum { STATE_QUIET, STATE_NOISE, STATE_COOLDOWN } state_t;
    state_t   st           = STATE_QUIET;
    bool      last_noise   = false;
    TickType_t cooldown_start = 0;
    TickType_t last_update = 0;
    TickType_t last_rms_print = 0;
    ESP_LOGI(TAG, "Loop running (quiet <%d, noise >%d, cooldown %d ms)...",
             RMS_QUIET, RMS_NOISE, COOLDOWN_MS);

    while (1) {
        /* 用原始 I2S 读（esp_codec_dev_read 与已 enable 的 I2S data_if 冲突）*/
        size_t raw_bytes = 0;
        i2s_channel_read(i2s_rx, buf, BUFFER_SAMPLES * sizeof(int16_t), &raw_bytes, pdMS_TO_TICKS(50));
        int bytes = (int)raw_bytes;

        if (bytes > 0) {
            int total = bytes / 2;
            float rms = calc_rms_left(buf, total);

            /* 每秒打印一次 RMS */
            TickType_t now = xTaskGetTickCount();
            if ((now - last_rms_print) >= pdMS_TO_TICKS(1000)) {
                last_rms_print = now;
                printf("RMS: %.2f | bytes=%d | buf[0]=%d buf[1]=%d buf[2]=%d  [state=%d]\n",
                       rms, bytes, buf[0], buf[1], buf[2], st);
                fflush(stdout);
            }

            /* --- 状态机 --- */
            bool is_noise_now = (rms > RMS_NOISE);

            if (is_noise_now) {
                /* 任何噪音立刻切入 noise，重置退让计时 */
                if (st != STATE_NOISE) {
                    printf("[HAL] 降低音量/停止香氛\n");
                    fflush(stdout);
                }
                st = STATE_NOISE;
                cooldown_start = 0;
            } else if (st == STATE_NOISE && rms < RMS_QUIET) {
                /* 刚从噪音回落 → 开始退让计时 */
                if (cooldown_start == 0) {
                    cooldown_start = now;
                }
                if ((now - cooldown_start) >= pdMS_TO_TICKS(COOLDOWN_MS)) {
                    /* 安静满 5 秒 → 恢复 */
                    st = STATE_QUIET;
                    cooldown_start = 0;
                    printf("[HAL] 恢复环境\n");
                    fflush(stdout);
                }
            } else if (st == STATE_QUIET) {
                /* 保持安静，无需改动 */
            }

            /* 200ms 刷新一次 UI */
            if ((now - last_update) >= pdMS_TO_TICKS(REFRESH_MS)) {
                last_update = now;

                bsp_display_lock(0);

                if (st == STATE_QUIET) {
                    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000040), LV_PART_MAIN);
                    lv_label_set_text(label_status, "Listening...");
                } else {
                    /* NOISE 或 COOLDOWN 期间保持红色 */
                    lv_obj_set_style_bg_color(scr, lv_color_hex(0x400000), LV_PART_MAIN);
                    lv_label_set_text(label_status, "Noise!");
                }

                char rms_buf[32];
                snprintf(rms_buf, sizeof(rms_buf), "RMS: %.0f", rms);
                lv_label_set_text(label_rms, rms_buf);

                bsp_display_unlock();

                /* 状态切换时打印简要日志 */
                if (is_noise_now != last_noise) {
                    if (is_noise_now) {
                        ESP_LOGI(TAG, ">>> NOISE! RMS=%.0f", rms);
                    } else {
                        ESP_LOGI(TAG, "<<< Quiet  RMS=%.0f (cooldown started)", rms);
                    }
                }
                last_noise = is_noise_now;
            }
        } else {
            ESP_LOGW(TAG, "Audio read: %d bytes", bytes);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}