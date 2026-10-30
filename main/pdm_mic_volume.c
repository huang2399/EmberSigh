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
#include "hal_hardware.h"
#include "audio_player.h"

static const char *TAG = "dreamstate";

#define BUFFER_SAMPLES  512
#define RMS_QUIET        50      /* 低于此 = 安静 */
#define RMS_NOISE       150     /* 高于此 = 拍手噪音 */
#define REFRESH_MS       200
#define COOLDOWN_MS     3000    /* 噪音结束后需安静 3 秒才恢复 */
#define SMOOTH_WINDOW    5       /* 滑动平均窗口大小 */

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

/* 滑动平均平滑 RMS：用长度为 SMOOTH_WINDOW 的历史值求平均 */
static float smooth_rms(float *window, int *idx, int *count, float new_rms)
{
    window[*idx] = new_rms;
    *idx = (*idx + 1) % SMOOTH_WINDOW;
    if (*count < SMOOTH_WINDOW) (*count)++;

    float sum = 0;
    for (int i = 0; i < *count; i++) {
        sum += window[i];
    }
    return sum / (float)(*count);
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

    /* 初始化所有外部硬件（MAX98357 / MOS管 / WS2812） */
    ESP_ERROR_CHECK(hal_hardware_init());

    /* 启动白噪音播放任务 */
    audio_player_start();

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

    /* 初始背景：深蓝 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ---- 标题 ---- */
    lv_obj_t *label_title = lv_label_create(scr);
    lv_label_set_text(label_title, "EmberSigh");
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x8888CC), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 12);

    /* ---- 脉动圆环（背景光圈） ---- */
    lv_obj_t *pulse_ring = lv_obj_create(scr);
    lv_obj_set_size(pulse_ring, 80, 80);
    lv_obj_set_style_radius(pulse_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pulse_ring, lv_color_hex(0x1A1A50), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pulse_ring, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(pulse_ring, 0, LV_PART_MAIN);
    lv_obj_align(pulse_ring, LV_ALIGN_CENTER, 0, -25);

    /* ---- RMS 大字数值 ---- */
    lv_obj_t *label_rms = lv_label_create(scr);
    lv_label_set_text(label_rms, "0");
    lv_obj_set_style_text_color(label_rms, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_rms, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_rms, LV_ALIGN_CENTER, 0, -25);

    /* ---- RMS 电平条 ---- */
    lv_obj_t *bar_rms = lv_bar_create(scr);
    lv_obj_set_size(bar_rms, 180, 10);
    lv_obj_set_style_radius(bar_rms, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0x1A1A40), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_rms, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0x3366FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_rms, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar_rms, 0, 500);
    lv_bar_set_value(bar_rms, 0, LV_ANIM_OFF);
    lv_obj_align(bar_rms, LV_ALIGN_CENTER, 0, 45);

    /* ---- 状态文字 ---- */
    lv_obj_t *label_status = lv_label_create(scr);
    lv_label_set_text(label_status, "Calm");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x6688CC), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -24);

    /* ---- 副标题 ---- */
    lv_obj_t *label_sub = lv_label_create(scr);
    lv_label_set_text(label_sub, "DreamState");
    lv_obj_set_style_text_color(label_sub, lv_color_hex(0x444466), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_sub, LV_ALIGN_BOTTOM_MID, 0, -8);

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

    /* 滑动平均窗口 */
    float  rms_window[SMOOTH_WINDOW] = {0};
    int    rms_win_idx   = 0;
    int    rms_win_count = 0;

    /* UI 阻尼平滑变量 */
    float  ui_rms = 0.0f;
    /* 背景色平滑插值变量 */
    int    bg_r_cur = 0x0A, bg_g_cur = 0x0A, bg_b_cur = 0x2E;

    ESP_LOGI(TAG, "Loop running (quiet <%d, noise >%d, cooldown %d ms)...",
             RMS_QUIET, RMS_NOISE, COOLDOWN_MS);

    while (1) {
        size_t raw_bytes = 0;
        i2s_channel_read(i2s_rx, buf, BUFFER_SAMPLES * sizeof(int16_t), &raw_bytes, pdMS_TO_TICKS(50));
        int bytes = (int)raw_bytes;

        if (bytes > 0) {
            int total = bytes / 2;
            float rms = calc_rms_left(buf, total);
            float smooth_val = smooth_rms(rms_window, &rms_win_idx, &rms_win_count, rms);

            /* UI 阻尼平滑：像呼吸一样缓慢变化 */
            ui_rms = ui_rms * 0.8f + smooth_val * 0.2f;

            TickType_t now = xTaskGetTickCount();

            /* --- 迟滞状态机：smooth_val 用于快速检测，UI 用 ui_rms 显示 --- */
            if (smooth_val > RMS_NOISE) {
                /* 超过 150：立刻触发噪音状态，重置冷却 */
                if (st != STATE_NOISE) {
                    hal_lower_volume();
                    hal_stop_fragrance();
                }
                st = STATE_NOISE;
                cooldown_start = 0;
            } else if (st == STATE_NOISE || st == STATE_COOLDOWN) {
                /* 当前处于噪音/冷却中 */
                if (smooth_val < RMS_QUIET) {
                    /* 安静中（<50）：计时冷却 */
                    if (cooldown_start == 0) {
                        cooldown_start = now;
                    }
                    if ((now - cooldown_start) >= pdMS_TO_TICKS(COOLDOWN_MS)) {
                        /* 安静持续 3 秒 → 恢复 */
                        st = STATE_QUIET;
                        cooldown_start = 0;
                        hal_restore_environment();
                    } else {
                        /* 冷却中但未到时 */
                        st = STATE_COOLDOWN;
                    }
                } else {
                    /* 50~150 灰色地带：保持噪音态，重置冷却 */
                    st = STATE_NOISE;
                    cooldown_start = 0;
                }
            }
            /* STATE_QUIET + noise_val < 150: 保持安静，什么也不做 */

            /* 200ms 刷新一次 UI */
            if ((now - last_update) >= pdMS_TO_TICKS(REFRESH_MS)) {
                last_update = now;

                bsp_display_lock(0);

                /* ---- RMS 数值（用 UI 阻尼值 ---- */
                char rms_buf[16];
                snprintf(rms_buf, sizeof(rms_buf), "%.0f", ui_rms);
                lv_label_set_text(label_rms, rms_buf);

                /* ---- 电平条（用 UI 阻尼值） ---- */
                int bar_val = (int)ui_rms;
                if (bar_val > 500) bar_val = 500;
                lv_bar_set_value(bar_rms, bar_val, LV_ANIM_ON);

                /* ---- 脉动圆环（用 UI 阻尼值） ---- */
                int ring_size = 60 + (int)(ui_rms * 0.15f);
                if (ring_size > 140) ring_size = 140;
                if (ring_size < 60)  ring_size = 60;
                lv_obj_set_size(pulse_ring, ring_size, ring_size);
                lv_obj_align(pulse_ring, LV_ALIGN_CENTER, 0, -25);

                /* ---- 背景色平滑渐变（300ms 过渡效果） ---- */
                int tr, tg, tb;
                if (st == STATE_QUIET) {
                    /* 目标：深蓝 0x0A0A2E */
                    tr = 0x0A; tg = 0x0A; tb = 0x2E;
                    lv_label_set_text(label_status, "Calm");
                    lv_obj_set_style_text_color(label_status, lv_color_hex(0x6688CC), LV_PART_MAIN);
                    lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0x3366FF), LV_PART_INDICATOR);
                } else if (st == STATE_COOLDOWN) {
                    /* 冷却中 → 暖橙色过渡 */
                    tr = 0x30; tg = 0x0A; tb = 0x10;
                    lv_label_set_text(label_status, "Calming...");
                    lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF8844), LV_PART_MAIN);
                    lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0xFF6600), LV_PART_INDICATOR);
                } else {
                    /* 噪音：暗红 0x3C0A0A */
                    tr = 0x3C; tg = 0x0A; tb = 0x0A;
                    lv_label_set_text(label_status, "Noise!");
                    lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF4444), LV_PART_MAIN);
                    lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0xFF3333), LV_PART_INDICATOR);
                }

                /* 每帧向目标色移动 25%（约 200ms*4=800ms 过渡到稳态，配合 200ms 刷新 = 4 帧） */
                bg_r_cur = bg_r_cur + (tr - bg_r_cur) / 4;
                bg_g_cur = bg_g_cur + (tg - bg_g_cur) / 4;
                bg_b_cur = bg_b_cur + (tb - bg_b_cur) / 4;
                if (bg_r_cur == tr && bg_g_cur == tg && bg_b_cur == tb) {
                    /* 到位了，确保精确 */
                    bg_r_cur = tr; bg_g_cur = tg; bg_b_cur = tb;
                }
                lv_obj_set_style_bg_color(scr,
                    lv_color_hex((bg_r_cur << 16) | (bg_g_cur << 8) | bg_b_cur),
                    LV_PART_MAIN);

                bsp_display_unlock();

                /* 状态切换日志 */
                bool noise_active = (st != STATE_QUIET);
                if (noise_active != last_noise) {
                    if (noise_active) {
                        ESP_LOGI(TAG, ">>> NOISE!  smooth=%.1f", smooth_val);
                    } else {
                        ESP_LOGI(TAG, "<<< Quiet  smooth=%.1f", smooth_val);
                    }
                }
                last_noise = noise_active;
            }
        } else {
            ESP_LOGW(TAG, "Audio read: %d bytes", bytes);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}