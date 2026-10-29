/*
 * DreamState HAL — 真实硬件驱动
 *  MAX98357 I2S 功放 (BCLK=27, LRC=26, DIN=14, I2S_NUM_1)
 *  MOS 管加湿器 (GPIO1)
 *  WS2812 灯带 (GPIO17, 8 LEDs, RMT)
 */
#include "hal_hardware.h"
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "led_strip.h"

static const char *TAG = "hal";

/* ========== 引脚定义 ========== */
#define GPIO_MOS_FRAGRANCE  1       /* MOS管信号线（加湿器电源） */
#define GPIO_LED_STRIP      17      /* WS2812 数据线 (不冲突) */
#define I2S_NUM_OUT         I2S_NUM_1
#define I2S_BCLK            27      /* I2S位时钟 (不冲突) */
#define I2S_LRC             26      /* I2S左右时钟 (原19=USB_D- 冲突!) */
#define I2S_DIN             14
#define LED_NUM             8       /* 灯珠数量 */

/* ========== 公开句柄 & 音量 ========== */
i2s_chan_handle_t  hal_i2s_tx = NULL;
static led_strip_handle_t led = NULL;
volatile float hal_audio_volume = 1.0f;

/* ============================================================
 * 初始化
 * ============================================================ */
esp_err_t hal_hardware_init(void)
{
    esp_err_t ret;

    /* ---- 1. MOS 管 GPIO 初始化 ---- */
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << GPIO_MOS_FRAGRANCE),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);
    gpio_set_level(GPIO_MOS_FRAGRANCE, 0);
    ESP_LOGI(TAG, "MOS GPIO%d init OK (LOW)", GPIO_MOS_FRAGRANCE);

    /* ---- 2. MAX98357 I2S TX 初始化 ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_OUT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ret = i2s_new_channel(&chan_cfg, &hal_i2s_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX new channel failed: %d", ret);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = I2S_BCLK,
            .ws   = I2S_LRC,
            .dout = I2S_DIN,
            .din  = GPIO_NUM_NC,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(hal_i2s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX std init failed: %d", ret);
        return ret;
    }
    ret = i2s_channel_enable(hal_i2s_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX enable failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "MAX98357 I2S init OK (BCLK=%d, LRC=%d, DIN=%d, 48kHz)", I2S_BCLK, I2S_LRC, I2S_DIN);

    /* ---- 3. WS2812 LED 灯带初始化 ---- */
    led_strip_config_t strip_cfg = {
        .strip_gpio_num        = GPIO_LED_STRIP,
        .max_leds              = LED_NUM,
        .led_model             = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out      = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = 10 * 1000 * 1000,
        .mem_block_symbols  = 64,
        .flags.with_dma     = false,
    };
    ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %d", ret);
        return ret;
    }

    /* 初始状态：深蓝暖光 */
    for (int i = 0; i < LED_NUM; i++) {
        led_strip_set_pixel(led, i, 0, 0, 64);
    }
    led_strip_refresh(led);
    ESP_LOGI(TAG, "WS2812 LED init OK (GPIO%d, %d LEDs)", GPIO_LED_STRIP, LED_NUM);

    ESP_LOGI(TAG, "=== All HAL hardware initialized ===");
    return ESP_OK;
}

/* ============================================================
 * 噪音响应
 * ============================================================ */
void hal_lower_volume(void)
{
    hal_audio_volume = 0.0f;
    printf("[HAL] 硬件真实降低音量 (volume=0)\n");
    fflush(stdout);
}

void hal_stop_fragrance(void)
{
    /* 切断加湿器 */
    gpio_set_level(GPIO_MOS_FRAGRANCE, 0);
    printf("[HAL] 切断加湿器电源\n");
    fflush(stdout);

    /* 灯带变暗红 */
    if (led) {
        for (int i = 0; i < LED_NUM; i++) {
            led_strip_set_pixel(led, i, 32, 0, 0);
        }
        led_strip_refresh(led);
    }
}

/* ============================================================
 * 恢复环境
 * ============================================================ */
void hal_restore_environment(void)
{
    /* 恢复音量 */
    hal_audio_volume = 1.0f;
    printf("[HAL] 硬件真实恢复音量 (volume=1.0)\n");
    fflush(stdout);

    /* 开启加湿器 */
    gpio_set_level(GPIO_MOS_FRAGRANCE, 1);
    printf("[HAL] 开启加湿器电源\n");
    fflush(stdout);

    /* 灯带恢复深蓝暖光 */
    if (led) {
        for (int i = 0; i < LED_NUM; i++) {
            led_strip_set_pixel(led, i, 0, 0, 64);
        }
        led_strip_refresh(led);
    }
}