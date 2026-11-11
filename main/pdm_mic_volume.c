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
#include "embersigh_fonts.h"

static const char *TAG = "dreamstate";

/* ============================================================
 * 可调参数
 * ============================================================ */
#define BUFFER_SAMPLES      512     /* 单次 I2S 读取样本数 */
#define REFRESH_MS          80      /* UI 刷新周期（80ms ≈ 12.5fps，呼吸动画更顺滑） */
#define SMOOTH_WINDOW       5       /* RMS 滑动平均窗口帧数 */

/* ---- 开机基线校准 ---- */
#define CALIB_MS            3000    /* 采集 3 秒环境音求 Base_RMS */
#define CALIB_WARMUP_MS     300     /* 丢弃最初 300ms 的脏数据 */

/* ---- 阈值（全部相对 Base_RMS） ----
 *
 * 自适应思路：
 *   - 底噪低（安静房间 base≈15）→ 阈值 ≈ base×2 ≈ 30~47，轻咳、拍手即触发
 *   - 底噪高（吵闹环境 base≈120）→ 比例项主导 ≈ 240，不会一点动静就误触
 * 因此 DELTA_MIN 只做「最低增量保护」，不再压过比例项。 */
#define NOISE_RATIO         1.8f    /* 噪音阈值 = Base × 1.8 */
#define NOISE_DELTA_MIN     22.0f   /* 至少比基线高 22（原 120 太高，手拍根本到不了） */
#define NOISE_THR_FLOOR     25.0f   /* 阈值下限，防止绝对安静时被风扇/电流底噪误触 */
#define NOISE_RELEASE       0.75f   /* 回落到阈值的 75% 才算噪音结束 */
#define QUIET_RATIO         1.25f   /* 安静阈值 = Base × 1.25 */
#define QUIET_DELTA_MIN     12.0f   /* 但至少比基线高 12 */

/* ---- 瞬发强触发 ----
 * 拍手 / 一声大声说话是「短促强瞬态」，能量往往撑不满判定时长。
 *
 * 注意：不能只看 smooth（5 帧滑动平均）。一次 50~100ms 的拍手会被平均
 * 稀释掉一半以上，smooth 可能只冒出 1~2 帧就掉回去，永远凑不满计时。
 * 因此额外用「瞬时 raw」做峰值判定 —— raw 是单帧（约 11ms）RMS，
 * 能可靠抓到拍手尖峰。 */
#define NOISE_INSTANT_RATIO     1.6f  /* smooth 超过阈值 1.6 倍：持续强噪音，立即触发 */
#define NOISE_INSTANT_RAW_RATIO 1.3f  /* raw 超过阈值 1.3 倍：拍手尖峰，立即触发 */

/* ---- 时间参数 ---- */
#define NOISE_SUSTAIN_MS    300     /* 高音量维持 300ms 即判定（原 2000ms，演示太难触发） */
#define QUIET_CONFIRM_MS    400     /* 安静确认时长，避免边界抖动 */
#define RECOVER_MS          3000    /* 恢复态持续时间 */

/* ---- 基线自适应（CALM 下缓慢跟踪环境） ---- */
#define BASE_ADAPT_ALPHA    0.06f   /* 每秒向环境均值靠近 6% */
#define BASE_ADAPT_MIN      0.5f    /* 基线下限 = 校准值 × 0.5 */
#define BASE_ADAPT_MAX      2.0f    /* 基线上限 = 校准值 × 2.0 */

#define LOG_PERIOD_MS       1000    /* 状态日志周期 */
#define DATA_PERIOD_MS      500     /* 网页 Web Serial 数据行周期 */

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

/* 供网页 Web Serial 解析用的状态名（RETREAT 对外呈现为 NOISE） */
static const char *web_state_name(sys_state_t s)
{
    switch (s) {
    case ST_CALM:    return "CALM";
    case ST_RETREAT: return "NOISE";
    case ST_RECOVER: return "RECOVER";
    case ST_IDLE:    return "IDLE";
    default:         return "UNKNOWN";
    }
}

/* 各状态的背景 / 文字 / 电平条配色 */
static void state_colors(sys_state_t s,
                         int *r, int *g, int *b,
                         uint32_t *text, uint32_t *bar)
{
    switch (s) {
    case ST_CALM:
        /* 深蓝：安静、沉静 */
        *r = 0x0A; *g = 0x0C; *b = 0x2C;
        *text = 0x8FA0C0; *bar = 0x3366FF;
        break;
    case ST_RETREAT:
        /* 柔和暗赤：比紫红更收敛，不刺眼，呼应"懂退让"的人格 */
        *r = 0x2C; *g = 0x14; *b = 0x16;
        *text = 0xD98A8A; *bar = 0xC2585E;
        break;
    case ST_RECOVER:
        /* 温橙：回暖、渐入 */
        *r = 0x2A; *g = 0x1E; *b = 0x0C;
        *text = 0xFFB266; *bar = 0xE09040;
        break;
    case ST_IDLE:
    default:
        /* 极暗：待机 */
        *r = 0x05; *g = 0x05; *b = 0x08;
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

/* 阈值计算：比例 + 最小增量取较大者，并保证不低于下限 */
static void update_thresholds(float base, float *noise_thr, float *quiet_thr)
{
    *noise_thr = fmaxf(base * NOISE_RATIO, base + NOISE_DELTA_MIN);
    if (*noise_thr < NOISE_THR_FLOOR) {
        *noise_thr = NOISE_THR_FLOOR;
    }
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
     *
     * 所有 LVGL 调用都必须在 bsp_display_lock() 保护下进行：
     * LVGL 在其独立任务里跑渲染，跨任务操作对象必须加锁，否则会
     * 出现断言失败 / 内存踩踏 / 偶发死机。
     * ======================================================== */
    lv_obj_t *scr          = NULL;
    lv_obj_t *label_title  = NULL;   /* 顶部：听眠 */
    lv_obj_t *pulse_ring   = NULL;   /* 中央：脉动/呼吸光圈 */
    lv_obj_t *label_icon   = NULL;   /* 中央：闭眼图标（RETREAT） */
    lv_obj_t *label_rms    = NULL;   /* 中央：RMS 数值（CALM） */
    lv_obj_t *label_main   = NULL;   /* 中央：主文案（RECOVER） */
    lv_obj_t *bar_rms      = NULL;   /* 电平条 */
    lv_obj_t *label_ambient= NULL;   /* 底部：极淡环境状态 */
    lv_obj_t *label_state  = NULL;   /* 底部：状态名（小字） */

    bsp_display_lock(0);
    {
        scr = lv_disp_get_scr_act(disp);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A2E), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        /* ---- 标题：听眠 ---- */
        label_title = lv_label_create(scr);
        lv_label_set_text(label_title, "听眠");
        lv_obj_set_style_text_color(label_title, lv_color_hex(0x8FA0C0), LV_PART_MAIN);
        lv_obj_set_style_text_font(label_title, &embersigh_font_cjk16, LV_PART_MAIN);
        lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 8);

        /* ---- 中央光圈（CALM 脉动 / RECOVER 呼吸） ---- */
        pulse_ring = lv_obj_create(scr);
        lv_obj_set_size(pulse_ring, 84, 84);
        lv_obj_set_style_radius(pulse_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pulse_ring, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(pulse_ring, lv_color_hex(0x3355AA), LV_PART_MAIN);
        lv_obj_set_style_border_width(pulse_ring, 2, LV_PART_MAIN);
        lv_obj_set_style_border_opa(pulse_ring, LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_scroll_dir(pulse_ring, LV_DIR_NONE);
        lv_obj_align(pulse_ring, LV_ALIGN_CENTER, 0, -20);

        /* ---- 中央图标：闭眼 / 嘘声（RETREAT 专用） ---- */
        label_icon = lv_label_create(scr);
        lv_label_set_text(label_icon, LV_SYMBOL_EYE_CLOSE);
        lv_obj_set_style_text_font(label_icon, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(label_icon, lv_color_hex(0xD98A8A), LV_PART_MAIN);
        lv_obj_align(label_icon, LV_ALIGN_CENTER, 0, -20);
        lv_obj_set_hidden(label_icon, true);

        /* ---- 中央 RMS 数值（CALM） ---- */
        label_rms = lv_label_create(scr);
        lv_label_set_text(label_rms, "--");
        lv_obj_set_style_text_color(label_rms, lv_color_hex(0xE8EFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(label_rms, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_align(label_rms, LV_ALIGN_CENTER, 0, -20);

        /* ---- 中央主文案（RECOVER："正在恢复..."） ---- */
        label_main = lv_label_create(scr);
        lv_label_set_text(label_main, "");
        lv_obj_set_style_text_color(label_main, lv_color_hex(0xFFB266), LV_PART_MAIN);
        lv_obj_set_style_text_font(label_main, &embersigh_font_cjk16, LV_PART_MAIN);
        lv_obj_align(label_main, LV_ALIGN_CENTER, 0, -20);
        lv_obj_set_hidden(label_main, true);

        /* ---- 电平条 ---- */
        bar_rms = lv_bar_create(scr);
        lv_obj_set_size(bar_rms, 176, 8);
        lv_obj_set_style_radius(bar_rms, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0x16203A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar_rms, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_rms, lv_color_hex(0x3366FF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar_rms, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_rms, 4, LV_PART_INDICATOR);
        lv_bar_set_range(bar_rms, 0, 600);
        lv_bar_set_value(bar_rms, 0, LV_ANIM_OFF);
        lv_obj_align(bar_rms, LV_ALIGN_CENTER, 0, 46);

        /* ---- 底部：极淡的环境状态行（CJK 14，透明度 45%） ---- */
        label_ambient = lv_label_create(scr);
        lv_label_set_text(label_ambient, "环境静谧");
        lv_obj_set_style_text_color(label_ambient, lv_color_hex(0x7A8DB8), LV_PART_MAIN);
        lv_obj_set_style_text_opa(label_ambient, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_text_font(label_ambient, &embersigh_font_cjk14, LV_PART_MAIN);
        lv_obj_align(label_ambient, LV_ALIGN_BOTTOM_MID, 0, -22);

        /* ---- 底部：状态名（10px，极淡） ---- */
        label_state = lv_label_create(scr);
        lv_label_set_text(label_state, "CALIB");
        lv_obj_set_style_text_color(label_state, lv_color_hex(0x4A5A78), LV_PART_MAIN);
        lv_obj_set_style_text_opa(label_state, LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_text_font(label_state, &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_align(label_state, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
    bsp_display_unlock();

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
    TickType_t last_data = 0;

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

        /* 瞬发强触发：raw 抓拍手尖峰，sm 抓持续强噪音 */
        bool instant = (sm  >= noise_thr * NOISE_INSTANT_RATIO) ||
                       (raw >= noise_thr * NOISE_INSTANT_RAW_RATIO);

        if (instant && !sustained) {
            ESP_LOGI(TAG, "[判定] 瞬发触发 raw=%.1f sm=%.1f 阈=%.1f 进入 RETREAT",
                     raw, sm, noise_thr);
        }

        bool trigger = sustained || instant;

        /* ---------- 状态机 ---------- */
        switch (st) {

        /* ============ CALM：正常监听 ============ */
        case ST_CALM:
            if (trigger) {
                ESP_LOGI(TAG, ">>> 噪音已确认（%s）→ 进入 RETREAT",
                         instant ? "瞬态直触发" : "持续判定");
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
            if (trigger) {
                ESP_LOGI(TAG, ">>> 恢复期再次出现噪音 → 退回 RETREAT");
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
            if (trigger) {
                ESP_LOGI(TAG, ">>> 待机中检测到噪音，唤醒 → CALM");
                hal_set_audio_volume(1.0f);
                hal_restore_fragrance_led();
                st = ST_CALM;
                high_active = false;
            }
            break;
        }

        /* ---------- UI 刷新 ---------- */
        if ((now - last_ui) >= pdMS_TO_TICKS(REFRESH_MS)) {
            last_ui = now;

            int tr, tg, tb;
            uint32_t text_col, bar_col;
            state_colors(st, &tr, &tg, &tb, &text_col, &bar_col);

            /* 背景色平滑插值（在锁外算好，锁内只做一次赋值） */
            bg_r = bg_r + (tr - bg_r) / 4;
            bg_g = bg_g + (tg - bg_g) / 4;
            bg_b = bg_b + (tb - bg_b) / 4;
            if (abs(bg_r - tr) <= 1) bg_r = tr;
            if (abs(bg_g - tg) <= 1) bg_g = tg;
            if (abs(bg_b - tb) <= 1) bg_b = tb;
            uint32_t bg_now = (uint32_t)((bg_r << 16) | (bg_g << 8) | bg_b);

            /* ---- 依据状态决定中央显示什么 ---- */
            const char *ambient_txt;
            bool show_ring   = false;
            bool show_icon   = false;
            bool show_rms    = false;
            bool show_main   = false;
            int  ring_size   = 84;

            switch (st) {
            case ST_CALM:
                /* 光圈 + RMS 数值 + 极淡环境状态 */
                show_ring = true;
                show_rms  = true;
                ring_size = 84 + (int)(ui_rms * 0.10f);
                if (ring_size > 128) ring_size = 128;
                if (ring_size < 84)  ring_size = 84;

                /* 环境状态文案随音量分三档 */
                if (sm < base_rms * 1.6f)      ambient_txt = "环境静谧";
                else if (sm < noise_thr * 0.6f) ambient_txt = "环境平稳";
                else                            ambient_txt = "环境稍响";
                break;

            case ST_RETREAT:
                /* 中央换成闭眼图标，隐藏光圈与数值 */
                show_icon  = true;
                ambient_txt = "已暂停香氛";
                break;

            case ST_RECOVER:
                /* 呼吸光圈（70↔130 正弦）+ 主文案 */
                show_ring = true;
                show_main = true;
                {
                    /* 以 2.4s 为周期做正弦呼吸 */
                    uint32_t period = 2400;
                    uint32_t phase  = (uint32_t)((now - recover_start) * 1000 /
                                                 configTICK_RATE_HZ) % period;
                    float t = (float)phase / (float)period;      /* 0..1 */
                    float s = 0.5f - 0.5f * cosf(t * 6.2831853f); /* 0..1 平滑呼吸 */
                    ring_size = 70 + (int)(s * 60.0f);           /* 70..130 */
                }
                ambient_txt = "音量渐入";
                break;

            case ST_IDLE:
            default:
                ambient_txt = "待机中";
                break;
            }

            char rms_buf[16];
            snprintf(rms_buf, sizeof(rms_buf), "%.0f", ui_rms);

            bsp_display_lock(0);

            /* 背景 */
            lv_obj_set_style_bg_color(scr, lv_color_hex(bg_now), LV_PART_MAIN);

            /* 中央元素显隐 */
            if (show_ring) {
                lv_obj_set_hidden(pulse_ring, false);
                lv_obj_set_size(pulse_ring, ring_size, ring_size);
                lv_obj_align(pulse_ring, LV_ALIGN_CENTER, 0, -20);
                lv_obj_set_style_border_color(pulse_ring,
                    lv_color_hex(st == ST_RECOVER ? 0xE09040 : 0x3355AA), LV_PART_MAIN);
            } else {
                lv_obj_set_hidden(pulse_ring, true);
            }

            if (show_rms) {
                lv_obj_set_hidden(label_rms, false);
                lv_label_set_text(label_rms, rms_buf);
            } else {
                lv_obj_set_hidden(label_rms, true);
            }

            if (show_icon) lv_obj_set_hidden(label_icon, false);
            else           lv_obj_set_hidden(label_icon, true);

            if (show_main) {
                lv_obj_set_hidden(label_main, false);
                lv_label_set_text(label_main, "正在恢复...");
            } else {
                lv_obj_set_hidden(label_main, true);
            }

            /* 电平条 */
            int bar_val = (int)ui_rms;
            if (bar_val > 600) bar_val = 600;
            lv_bar_set_value(bar_rms, bar_val, LV_ANIM_ON);
            lv_obj_set_style_bg_color(bar_rms, lv_color_hex(bar_col), LV_PART_INDICATOR);

            /* 底部极淡环境状态行 */
            lv_label_set_text(label_ambient, ambient_txt);
            lv_obj_set_style_text_color(label_ambient, lv_color_hex(text_col), LV_PART_MAIN);

            /* 底部状态名 */
            lv_label_set_text(label_state, state_name(st));

            bsp_display_unlock();
        }

        /* ---------- 每 500ms：给网页 Web Serial 解析的数据行 ---------- */
        if ((now - last_data) >= pdMS_TO_TICKS(DATA_PERIOD_MS)) {
            last_data = now;

            int rms_out = (int)(ui_rms + 0.5f);
            if (rms_out < 0) rms_out = 0;

            printf("DATA | RMS: %d | STATE: %s\n", rms_out, web_state_name(st));
            fflush(stdout);
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
