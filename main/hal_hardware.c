/*
 * DreamState HAL — 真实硬件驱动
 *  MAX98357 I2S 功放   (BCLK=18, LRC=17, DIN=14, I2S_NUM_1)
 *  香氛 MOS 加湿器     (GPIO1, 光耦隔离 MOS 低边切负极，独立充电宝供电)
 *  WS2812 灯带         (GPIO16, RMT)
 *
 *  引脚已按官方原理图核对，严禁交叉：
 *    GPIO17 = I2S LRC（左右声道时钟），不可复用为灯带/香氛/普通 IO
 *    GPIO19 = USB D−，不可配置
 */
#include "hal_hardware.h"
#include <stdio.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hal";

/* ========== 引脚定义 ========== */
#define FRAGRANCE_GPIO      GPIO_NUM_1   /* 香氛控制：光耦 MOS 模块 SIG（低电平=关） */
#define GPIO_LED_STRIP      GPIO_NUM_16  /* WS2812 数据线 */
#define I2S_NUM_OUT         I2S_NUM_1
#define I2S_BCLK            GPIO_NUM_18  /* I2S 位时钟 BCLK */
#define I2S_LRC             GPIO_NUM_17  /* I2S 左右时钟 LRC（原 26/27 属 PSRAM 占用区，已改） */
#define I2S_DIN             GPIO_NUM_14  /* I2S 数据 DIN */
#define LED_NUM             8            /* 灯珠数量 */

/* ========== 香氛时间切片参数 ========== */
/* 控制窗口：一次完整的「通 - 断」周期（毫秒）。
 * 2s 是雾化片热惯性与香氛扩散速度的折中：窗口太短香量不稳，太长则调节迟钝。 */
#define FRAGRANCE_WINDOW_MS   2000
/* 恢复香氛时使用的占空比（%），50% 约等于「半功率持续释香」 */
#define FRAGRANCE_RESTORE_DUTY  50

/* ========== 公开句柄 & 音量 ========== */
i2s_chan_handle_t  hal_i2s_tx = NULL;
static led_strip_handle_t led = NULL;
volatile float hal_audio_volume = 1.0f;

/* ============================================================
 * 香氛控制：时间切片 PWM（软件实现，不使用 LEDC）
 * ============================================================ */

/**
 * @brief 初始化香氛控制 GPIO
 *
 * 配置 FRAGRANCE_GPIO 为推挽输出，并给一个内部下拉：
 * 上电默认输出低电平，即 MOS 关断、加湿器不通电，
 * 避免 ESP32 复位瞬间引脚悬空（浮空高阻可能被 MOS 栅极误判为高）导致加湿器乱喷。
 */
esp_err_t fragrance_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << FRAGRANCE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* 掉电/浮空时保持低电平 */
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "香氛 GPIO%d 配置失败: %d", FRAGRANCE_GPIO, ret);
        return ret;
    }

    gpio_set_level(FRAGRANCE_GPIO, 0);          /* 上电默认关闭 */
    ESP_LOGI(TAG, "香氛 MOS GPIO%d 初始化完成（默认 LOW = 关闭）", FRAGRANCE_GPIO);
    return ESP_OK;
}

/**
 * @brief 设置香氛释放占空比（0 ~ 100）
 *
 * 【为什么不能用 LEDC】
 * 驱动的是超声波雾化片（压电陶瓷，工作在 ~108kHz 机械谐振点）。
 * 它靠谐振持续振荡把水打成微米级雾滴，需要「连续直流导通」的供电窗口；
 * 若用 LEDC 输出几十 kHz 的 PWM 斩波，MOS 反复开关会打断谐振、
 * 造成雾化片发热、发出啸叫，长期还会损伤压电片。
 * 因此这里用「低频时间切片」模拟功率调节：
 * 在一个 2000ms 的大窗口里，让 MOS 连续导通 on_ms、完全关断 off_ms，
 * 用占空比控制单位时间的出雾总量 —— 这是开关式间歇控制，不是高频斩波。
 *
 * @param duty 0=完全关闭, 100=全功率常开, 1~99=按比例间歇工作
 *
 * @note 本函数内部使用 vTaskDelay 阻塞当前任务：
 *       - duty=50 时一次调用最长占用约 2000ms（1000ms 通 + 1000ms 断）；
 *       - MVP 阶段可接受（只在上电、退让、恢复等状态切换时调用）；
 *       - 若后续需要在主循环中高频调用，请把香氛控制迁到独立的 FreeRTOS 任务
 *         （任务内做时间切片，调用方只投递目标 duty），避免阻塞麦克风采样/UI 刷新。
 */
void set_fragrance_duty(uint8_t duty)
{
    /* 边界 1：全关 —— 立即拉低并返回，不做延时 */
    if (duty == 0) {
        gpio_set_level(FRAGRANCE_GPIO, 0);
        return;
    }

    /* 边界 2：全开 —— 立即拉高并返回，常通（注意：长时间满功率易过雾） */
    if (duty >= 100) {
        gpio_set_level(FRAGRANCE_GPIO, 1);
        return;
    }

    /* 1~99：按比例切分 2000ms 窗口 */
    uint32_t on_ms  = (FRAGRANCE_WINDOW_MS * (uint32_t)duty) / 100u;
    uint32_t off_ms = FRAGRANCE_WINDOW_MS - on_ms;

    gpio_set_level(FRAGRANCE_GPIO, 1);          /* 导通窗口 */
    vTaskDelay(pdMS_TO_TICKS(on_ms));

    gpio_set_level(FRAGRANCE_GPIO, 0);          /* 关断窗口 */
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

/* ============================================================
 * 初始化
 * ============================================================ */
esp_err_t hal_hardware_init(void)
{
    esp_err_t ret;

    /* ---- 1. 香氛 MOS 管 GPIO 初始化（默认关闭） ---- */
    ret = fragrance_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "fragrance_init failed: %d", ret);
        return ret;
    }

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
    ESP_LOGI(TAG, "MAX98357 I2S init OK (BCLK=%d, LRC=%d, DIN=%d, 48kHz)",
             I2S_BCLK, I2S_LRC, I2S_DIN);

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

void hal_set_audio_volume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    hal_audio_volume = vol;
}

/* 切断香氛：占空比 0，立即关断（无阻塞延时） */
void hal_stop_fragrance(void)
{
    set_fragrance_duty(0);
    printf("[HAL] 切断加湿器电源（duty=0）\n");
    fflush(stdout);

    /* 灯带变暗红 */
    if (led) {
        for (int i = 0; i < LED_NUM; i++) {
            led_strip_set_pixel(led, i, 32, 0, 0);
        }
        led_strip_refresh(led);
    }
}

/* 仅恢复香氛 + 灯带，音量保持当前值（供 RECOVER 渐入使用）
 * 注意：set_fragrance_duty(50) 会阻塞约 2000ms，本函数被麦克风检测任务调用，
 *       期间 RMS 采样与 UI 状态色刷新会顺延一次；若需绝对流畅，
 *       可改为向独立香氛任务投递目标 duty（非阻塞）。 */
void hal_restore_fragrance_led(void)
{
    set_fragrance_duty(FRAGRANCE_RESTORE_DUTY);

    if (led) {
        for (int i = 0; i < LED_NUM; i++) {
            led_strip_set_pixel(led, i, 0, 0, 64);
        }
        led_strip_refresh(led);
    }
}

/* 待机：断香 + 灭灯 + 静音 */
void hal_enter_standby(void)
{
    hal_audio_volume = 0.0f;
    set_fragrance_duty(0);
    if (led) {
        for (int i = 0; i < LED_NUM; i++) {
            led_strip_set_pixel(led, i, 0, 0, 0);
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

    /* 恢复香氛：50% 时间切片占空比（阻塞约 2s，MVP 可接受） */
    set_fragrance_duty(FRAGRANCE_RESTORE_DUTY);
    printf("[HAL] 恢复香氛释放（duty=%d%%，窗口 %dms）\n",
           FRAGRANCE_RESTORE_DUTY, FRAGRANCE_WINDOW_MS);
    fflush(stdout);

    /* 灯带恢复深蓝暖光 */
    if (led) {
        for (int i = 0; i < LED_NUM; i++) {
            led_strip_set_pixel(led, i, 0, 0, 64);
        }
        led_strip_refresh(led);
    }
}
