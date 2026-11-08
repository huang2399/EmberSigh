/*
 * EmberSigh 主程序 — 环境自适应四状态机
 *
 * 升级要点（相对上一版）：
 *   1. 开机基线校准：采集 3 秒环境音求平均 RMS 作为 Base_RMS，
 *      所有阈值改为相对基线的比例 + 最小增量，不再写死绝对值。
 *   2. 防误触发：高音量必须【连续持续 ≥ 2 秒】才判定为有效噪音，
 *      瞬时咳嗽 / 拍手只会有一次计时日志，不会改变状态。
 *   3. 四状态机：
 *        CALM    (深蓝)   正常监听
 *        RETREAT (紫红)   退让：降音量 + 断香氛 + 灯转暗红
 *        RECOVER (橙色)   恢复：音量 3 秒线性渐入 0→1
 *        IDLE    (极暗)   待机（预留：长按按键进入）
 *   4. 每秒串口打印：状态 / 实时 RMS / 平滑 RMS / 基线 / 当前阈值。
 *
 * 硬件：ESP32-S3-EYE + ES8311 麦克风(I2S0) + MAX98357(I2S1) + MOS 管 + WS2812
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
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

/* ============================================================
 * 可调参数
 * ============================================================ */
#define BUFFER_SAMPLES      512     /* 单次 I2S 读取样本数 */
#define REFRESH_MS          200     /* UI 刷新周期 */
#define SMOOTH_WINDOW       5       /* RMS 滑动平均窗口帧数 */

/* ---- 开机基线校准 ---- */
#define CALIB_MS            3000    /* 采集 3 秒环境音求 Base_RMS */
#define CALIB_WARMUP_MS     300     /* 丢弃最初 300ms 的脏数据 */

/* ---- 阈值（全部相对 Base_RMS） ---- */
#define NOISE_RATIO         2.2f    /* 噪音阈值 = Base × 2.2 */
#define NOISE_DELTA_MIN     120.0f  /* 但至少比基线高 120 */
#define NOISE_RELEASE       0.75f   /* 回落到阈值的 75% 才算噪音结束 */
#define QUIET_RATIO         1.30f   /* 安静阈值 = Base × 1.3 */
#define QUIET_DELTA_MIN     25.0f   /* 但至少比基线高 25 */

/* ---- 时间参数 ---- */
#define NOISE_SUSTAIN_MS    2000    /* 高音量必须连续维持 2 秒 */
#define QUIET_CONFIRM_MS    400     /* 安静确认时长，避免边界抖动 */
#define RECOVER_MS          3000    /* 恢复态持续时间 */

/* ---- 基线自适应（CALM 下缓慢跟踪环境） ---- */
#define BASE_ADAPT_ALPHA    0.06f   /* 每秒向环境均值靠近 6% */
#define BASE_ADAPT_MIN      0.5f    /* 基线下限 = 校准值 × 0.5 */
#define BASE_ADAPT_MAX      2.0f    /* 基线上限 = 校准值 × 2.0 */

#define LOG_PERIOD_MS       1000    /* 状态日志周期 */

/* ============================================================
 * 四状态机
 * ============================================================ */
typedef enum {
    ST_CALM = 0,    /* 正常监听 — 深蓝 */
    ST_RETREAT,     /* 退让     — 紫红 */
    ST_RECOVER,     /* 恢复     — 橙色 */
    ST_IDLE,        /* 待机     — 极暗（预留） */
} sys_state_t;

static const char *state_name(sys_state_t s)
{
    switch (s) {
    case ST_CALM:    return "CALM";
    case ST_RETREAT: return "RETREAT";
    case ST_RECOVER: return "RECOVER";
    case ST_IDLE:    return "IDLE";
    default:         return "?";
    }
}

/* 各状态的背景 / 文字 / 电平条配色 */
static void state_colors(sys_state_t s,
                         int *r, int *g, int *b,
                         uint32_t *text, uint32_t *bar)
{
    switch (s) {
    case ST_CALM:
        *r = 0x0A; *g = 0x0A; *b = 0x2E;
        *text = 0x6688CC; *bar = 0x3366FF;
        break;
    case ST_RETREAT:
        *r = 0x3A; *g = 0x0A; *b = 0x26;      /* 紫红 */
        *text = 0xFF3366; *bar = 0xFF3366;
        break;
    case ST_RECOVER:
        *r = 0x30; *g = 0x20; *b = 0x0A;      /* 橙色 */
        *text = 0xFF8844; *bar = 0xFF8844;
        break;
    case ST_IDLE:
    default:
        *r = 0x05; *g = 0x05; *b = 0x08;      /* 极暗 */
        *text = 0x1A1A22; *bar = 0x1A1A22;
        break;
    }
}

/* ============================================================
 * 工具函数
 * ============================================================ */
static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

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

/* 滑动平均平滑 RMS */
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

/* 阈值计算：比例 + 最小增量取较大者 */
static void update_thresholds(float base, float *noise_thr, float *quiet_thr)
{
    *noise_thr = fmaxf(base * NOISE_RATIO, base + NOISE_DELTA_MIN);
    *quiet_thr = fmaxf(base * QUIET_RATIO, base + QUIET_DELTA_MIN);
}

/* ============================================================
 * 开机基线校准：采集 CALIB_MS 毫秒环境音，返回【中位数】RMS
 *
 * 用中位数而非平均值：若 3 秒内恰好有人说话/关门，平均值会被严重拉高
 * （实测一次 RMS 279 的瞬态可把 Base 从 6.8 抬到 15.4），中位数不受影响。
 * ============================================================ */
#define CALIB_MAX_SAMPLES  400

static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static float calibrate_baseline(i2s_chan_handle_t rx, int16_t *buf, int buf_samples)
{
    printf("\n[HAL] === 开机基线校准：采集 %d ms 环境音，请保持安静 ===\n", CALIB_MS);
    fflush(stdout);

    float *samples = malloc(CALIB_MAX_SAMPLES * sizeof(float));
    if (!samples) {
        ESP_LOGE(TAG, "校准缓冲区分配失败，回退 Base=20");
        return 20.0f;
    }

    int n = 0;
    TickType_t t0 = xTaskGetTickCount();

    while (1) {
        size_t bytes = 0;
        i2s_channel_read(rx, buf, buf_samples * sizeof(int16_t),
                         &bytes, pdMS_TO_TICKS(100));

        if (bytes > 0) {
            float r = calc_rms_left(buf, (int)(bytes / 2));
            TickType_t elapsed = xTaskGetTickCount() - t0;

            /* 丢弃前 CALIB_WARMUP_MS，避开 ADC 启动瞬态 */
            if (elapsed >= pdMS_TO_TICKS(CALIB_WARMUP_MS) && n < CALIB_MAX_SAMPLES) {
                samples[n++] = r;
            }
        }

        if ((xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(CALIB_MS)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    float base = 20.0f;
    if (n > 0) {
        qsort(samples, n, sizeof(float), cmp_float);
        base = samples[n / 2];              /* 中位数 */
    }
    free(samples);

    if (base < 5.0f) base = 5.0f;   /* 全静音保护，避免阈值塌到 0 */

    printf("[HAL] 校准完成 → Base_RMS(中位数) = %.1f  (样本 %d)\n", base, n);
    fflush(stdout);

    return base;
}

/* ============================================================
 * 主程序
 * ============================================================ */
void app_main(void)
{
    printf("\n=== EmberSigh 环境自适应状态机 v2 ===\n\n");
    fflush(stdout);

    /* ---- 1. 显示 + LVGL ---- */
    ESP_LOGI(TAG, "Init display...");
    lv_display_t *disp = bsp_display_start();
    assert(disp);
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Display ready");

    /* ---- 2. 外部硬件（MAX98357 / MOS / WS2812） ---- */
    ESP_ERROR_CHECK(hal_hardware_init());

    /* ---- 3. 声景播放任务 ---- */
    audio_player_start();

    /* ---- 4. 麦克风：I2C + I2S RX + ES8311 ---- */
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

    /* 手动创建 I2S RX 通道 */
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
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx));   /* 时钟先起，再配 ES8311 */
    ESP_LOGI(TAG, "I2S enabled with clocks running");

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = i2s_rx,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    assert(data_if);

    /* 探测 ES8311（0x18 或 0x12） */
    uint8_t es8311_addr = 0x18;
    esp_err_t probe = i2c_master_probe(i2c_bus, es8311_addr, 20);
    ESP_LOGI(TAG, "ES8311 probe @0x%02X: %s", es8311_addr,
             probe == ESP_OK ? "FOUND" : "NOT FOUND");

    if (probe != ESP_OK) {
        es8311_addr = 0x12;
        probe = i2c_master_probe(i2c_bus, es8311_addr, 20);
        ESP_LOGI(TAG, "ES8311 probe @0x%02X: %s", es8311_addr,
                 probe == ESP_OK ? "FOUND" : "NOT FOUND");
    }
    if (probe != ESP_OK) {
        printf("ES8311 not found on I2C bus, no audio available.\n");
        fflush(stdout);
        return;
    }

    esp_codec_dev_handle_t mic = NULL;
    {
        ESP_LOGI(TAG, "ES8311 online @0x%02X, full I2C init...", es8311_addr);

        audio_codec_i2c_cfg_t i2c_cfg = {
            .addr = (es8311_addr << 1),   /* 转 8-bit 地址 */
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
     * 5. 创建 LVGL UI
     * ======================================================== */
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A2E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* 标题 */
    lv_obj_t *label_title = lv_label_create(scr);
    lv_label_set_text(label_title, "EmberSigh");
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x8888CC), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 12);

    /* 脉动光圈 */
    lv_obj_t *pulse_ring = lv_obj_create(scr);
    lv_obj_set_size(pulse_ring, 80, 80);
    lv_obj_set_style_radius(pulse_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pulse_ring, lv_color_hex(0x1A1A50), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pulse_ring, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(pulse_ring, 0, LV_PART_MAIN);
    lv_obj_align(pulse_ring, LV_ALIGN_CENTER, 0, -25);

    /* RMS 数值 */
    lv_obj_t *label_rms = lv_label_create(scr);
    lv_label_set_text(label_rms, "--");
    lv_obj_set_style_text_color(label_rms, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_rms, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_rms, LV_ALIGN_CENTER, 0, -25);

    /* RMS 电平条 */
    lv_obj_t *bar_rms = lv_bar_create(scr);
    lv_obj_set_size(bar_rms, 180, 10);
    lv_obj_set_style_radius(bar_rms, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0x1A1A40), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_rms, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0x3366FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_rms, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar_rms, 0, 600);
    lv_bar_set_value(bar_rms, 0, LV_ANIM_OFF);
    lv_obj_align(bar_rms, LV_ALIGN_CENTER, 0, 45);

    /* 状态文字 */
    lv_obj_t *label_status = lv_label_create(scr);
    lv_label_set_text(label_status, "CALIB");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x6688CC), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -24);

    /* 副标题：显示基线 */
    lv_obj_t *label_sub = lv_label_create(scr);
    lv_label_set_text(label_sub, "calibrating");
    lv_obj_set_style_text_color(label_sub, lv_color_hex(0x444466), LV_PART_MAIN);
    lv_obj_set_style_text_font(label_sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label_sub, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* ========================================================
     * 6. 开机基线校准（3 秒）
     * ======================================================== */
    int16_t *buf = malloc(BUFFER_SAMPLES * sizeof(int16_t));
    assert(buf);

    float calib_base = calibrate_baseline(i2s_rx, buf, BUFFER_SAMPLES);
    float base_rms   = calib_base;

    float noise_thr = 0, quiet_thr = 0;
    update_thresholds(base_rms, &noise_thr, &quiet_thr);
    float release_thr = noise_thr * NOISE_RELEASE;

    printf("[HAL] 阈值 → 噪音 ≥ %.0f | 安静 < %.0f | 释放线 %.0f\n",
           noise_thr, quiet_thr, release_thr);
    printf("[HAL] 防误触发：高音量需连续维持 %d ms 才判为有效噪音\n\n",
           NOISE_SUSTAIN_MS);
    fflush(stdout);

    /* ========================================================
     * 7. 主循环：四状态机
     * ======================================================== */
    sys_state_t st = ST_CALM;

    /* 持续时长判定 */
    bool       high_active = false;
    TickType_t high_since  = 0;

    /* 安静确认 */
    TickType_t quiet_since = 0;

    /* 恢复态起点 */
    TickType_t recover_start = 0;

    /* 周期任务 */
    TickType_t last_ui  = 0;
    TickType_t last_log = 0;

    /* 每秒统计（用于日志与基线自适应） */
    float sec_sum = 0.0f;
    int   sec_n   = 0;

    /* 滑动平均窗口 */
    float rms_window[SMOOTH_WINDOW] = {0};
    int   rms_win_idx   = 0;
    int   rms_win_count = 0;

    /* UI 阻尼 */
    float ui_rms = 0.0f;
    int   bg_r = 0x0A, bg_g = 0x0A, bg_b = 0x2E;

    ESP_LOGI(TAG, "主循环启动 · 状态 = %s", state_name(st));

    while (1) {
        size_t raw_bytes = 0;
        i2s_channel_read(i2s_rx, buf, BUFFER_SAMPLES * sizeof(int16_t),
                         &raw_bytes, pdMS_TO_TICKS(50));
        int bytes = (int)raw_bytes;

        if (bytes <= 0) {
            ESP_LOGW(TAG, "I2S read 返回 0 字节");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        float raw      = calc_rms_left(buf, bytes / 2);
        float sm       = smooth_rms(rms_window, &rms_win_idx, &rms_win_count, raw);
        ui_rms         = ui_rms * 0.8f + sm * 0.2f;

        TickType_t now = xTaskGetTickCount();

        /* 每秒统计累加 */
        sec_sum     += sm;
        sec_n++;

        /* ---------- 持续时长判定（CALM / RECOVER 共用） ---------- */
        bool noisy = (sm >= noise_thr);

        if (noisy && !high_active) {
            high_active = true;
            high_since  = now;
            ESP_LOGI(TAG, "[判定] RMS %.1f ≥ %.0f，开始计时（需持续 %d ms）",
                     sm, noise_thr, NOISE_SUSTAIN_MS);
        } else if (!noisy && sm < release_thr && high_active) {
            high_active = false;
            ESP_LOGI(TAG, "[判定] 回落至 %.1f，未满 %d ms，不触发",
                     sm, NOISE_SUSTAIN_MS);
        }

        bool sustained = high_active &&
                         ((now - high_since) >= pdMS_TO_TICKS(NOISE_SUSTAIN_MS));

        /* ---------- 状态机 ---------- */
        switch (st) {

        /* ============ CALM：正常监听 ============ */
        case ST_CALM:
            if (sustained) {
                ESP_LOGI(TAG, ">>> 噪音已持续 %d ms → 进入 RETREAT", NOISE_SUSTAIN_MS);
                hal_lower_volume();
                hal_stop_fragrance();
                printf("[HAL] 音量降低，暂停香氛\n");
                fflush(stdout);

                st = ST_RETREAT;
                high_active = false;
                quiet_since = 0;
            }
            break;

        /* ============ RETREAT：退让 ============ */
        case ST_RETREAT:
            if (sm < quiet_thr) {
                if (quiet_since == 0) quiet_since = now;

                if ((now - quiet_since) >= pdMS_TO_TICKS(QUIET_CONFIRM_MS)) {
                    ESP_LOGI(TAG, ">>> 环境安静，进入 RECOVER（音量 %d ms 渐入）",
                             RECOVER_MS);
                    hal_set_audio_volume(0.0f);
                    hal_restore_fragrance_led();
                    printf("[HAL] 音量渐入恢复\n");
                    fflush(stdout);

                    st = ST_RECOVER;
                    recover_start = now;
                    quiet_since = 0;
                    high_active = false;
                }
            } else {
                quiet_since = 0;
            }
            break;

        /* ============ RECOVER：恢复（音量线性渐入） ============ */
        case ST_RECOVER: {
            /* 恢复期若再次满足持续噪音，直接退回 RETREAT */
            if (sustained) {
                ESP_LOGI(TAG, ">>> 恢复期再次出现持续噪音 → 退回 RETREAT");
                hal_lower_volume();
                hal_stop_fragrance();
                printf("[HAL] 音量降低，暂停香氛\n");
                fflush(stdout);

                st = ST_RETREAT;
                high_active = false;
                quiet_since = 0;
                break;
            }

            TickType_t elapsed = now - recover_start;

            if (elapsed >= pdMS_TO_TICKS(RECOVER_MS)) {
                hal_set_audio_volume(1.0f);
                st = ST_CALM;
                high_active = false;
                quiet_since = 0;
                ESP_LOGI(TAG, ">>> 恢复完成（音量 1.0）→ 回到 CALM");
            } else {
                /* 线性渐入 0.0 → 1.0 */
                float p = (float)elapsed / (float)pdMS_TO_TICKS(RECOVER_MS);
                hal_set_audio_volume(clampf(p, 0.0f, 1.0f));
            }
            break;
        }

        /* ============ IDLE：极暗待机（预留） ============ */
        case ST_IDLE:
            /* 进入方式（预留）：长按按键时执行下面几行
             *   hal_enter_standby();
             *   st = ST_IDLE;
             * 当前未绑定物理按键，故该分支只在被手动置位后生效。
             * 唤醒条件：检测到持续噪音即回到 CALM。
             */
            if (sustained) {
                ESP_LOGI(TAG, ">>> 待机中检测到噪音，唤醒 → CALM");
                hal_set_audio_volume(1.0f);
                hal_restore_fragrance_led();
                st = ST_CALM;
                high_active = false;
            }
            break;
        }

        /* ---------- UI 刷新（200ms） ---------- */
        if ((now - last_ui) >= pdMS_TO_TICKS(REFRESH_MS)) {
            last_ui = now;

            int tr, tg, tb;
            uint32_t text_col, bar_col;
            state_colors(st, &tr, &tg, &tb, &text_col, &bar_col);

            bsp_display_lock(0);

            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%.0f", ui_rms);
            lv_label_set_text(label_rms, tmp);

            int bar_val = (int)ui_rms;
            if (bar_val > 600) bar_val = 600;
            lv_bar_set_value(bar_rms, bar_val, LV_ANIM_ON);

            int ring_size = 60 + (int)(ui_rms * 0.12f);
            if (ring_size > 140) ring_size = 140;
            if (ring_size < 60)  ring_size = 60;
            lv_obj_set_size(pulse_ring, ring_size, ring_size);
            lv_obj_align(pulse_ring, LV_ALIGN_CENTER, 0, -25);

            lv_label_set_text(label_status, state_name(st));
            lv_obj_set_style_text_color(label_status, lv_color_hex(text_col), LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar_rms, lv_color_hex(bar_col), LV_PART_INDICATOR);

            snprintf(tmp, sizeof(tmp), "base %.0f", base_rms);
            lv_label_set_text(label_sub, tmp);

            /* 背景色平滑插值 */
            bg_r = bg_r + (tr - bg_r) / 4;
            bg_g = bg_g + (tg - bg_g) / 4;
            bg_b = bg_b + (tb - bg_b) / 4;
            lv_obj_set_style_bg_color(scr,
                lv_color_hex((bg_r << 16) | (bg_g << 8) | bg_b), LV_PART_MAIN);

            bsp_display_unlock();
        }

        /* ---------- 每秒：日志 + 基线自适应 ---------- */
        if ((now - last_log) >= pdMS_TO_TICKS(LOG_PERIOD_MS)) {
            last_log = now;

            float avg_smooth = (sec_n > 0) ? (sec_sum / (float)sec_n) : sm;

            /* 串口日志：状态 / 实时 RMS / 平滑 RMS / 基线 / 阈值 */
            printf("[状态] %-7s | RMS raw=%6.1f smooth=%6.1f ui=%5.1f | "
                   "base=%5.1f | 噪音≥%.0f 安静<%.0f\n",
                   state_name(st), raw, sm, ui_rms,
                   base_rms, noise_thr, quiet_thr);
            fflush(stdout);

            /* CALM 下缓慢跟踪环境，实现长期自适应。
             *
             * 关键守卫：只有当这一秒确实「安静」（均值低于安静阈值）才更新基线。
             * 否则一次咳嗽/说话会把基线抬上去，导致之后很久都检测不到噪音
             * （实测瞬态可把 base 从 16.6 顶到 30.9）。 */
            if (st == ST_CALM && avg_smooth < quiet_thr) {
                float lo = calib_base * BASE_ADAPT_MIN;
                float hi = calib_base * BASE_ADAPT_MAX;
                base_rms = clampf(base_rms * (1.0f - BASE_ADAPT_ALPHA) +
                                  avg_smooth * BASE_ADAPT_ALPHA, lo, hi);

                update_thresholds(base_rms, &noise_thr, &quiet_thr);
                release_thr = noise_thr * NOISE_RELEASE;
            }

            sec_sum = 0.0f;
            sec_n = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
