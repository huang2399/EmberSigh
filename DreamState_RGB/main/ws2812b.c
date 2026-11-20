/*
 * ws2812b.c - WS2812B driver built directly on the ESP-IDF RMT TX peripheral.
 *
 * Timing model
 * ------------
 * The RMT channel runs at 40 MHz, so one tick is 25 ns. A WS2812B bit is
 * encoded as a single RMT symbol (two adjacent durations):
 *
 *      symbol = [ level0=1 for T_HIGH ][ level1=0 for T_LOW ]
 *
 *      '0' -> 400 ns high, 850 ns low   (16 / 34 ticks)
 *      '1' -> 800 ns high, 450 ns low   (32 / 18 ticks)
 *
 * Both bit cells are 1250 ns wide, which is the WS2812B nominal period.
 * The frame is terminated by a 60 us low pulse (the latch/reset time).
 *
 * Byte order on the wire is G7..G0, R7..R0, B7..B0 (GRB), MSB first.
 */
#include "ws2812b.h"

#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ws2812b";

/* ---- RMT timing (40 MHz -> 25 ns per tick) ---- */
#define RMT_RESOLUTION_HZ   40000000
#define T0H_TICKS           16      /* 400 ns */
#define T0L_TICKS           34      /* 850 ns */
#define T1H_TICKS           32      /* 800 ns */
#define T1L_TICKS           18      /* 450 ns */
#define RESET_TICKS         2400    /* 60 us latch */

#define BITS_PER_LED        24
#define FRAME_SYMBOLS       (WS2812B_LED_COUNT * BITS_PER_LED + 1)

/* ---- Driver state ---- */
static rmt_channel_handle_t s_tx_channel   = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static rmt_symbol_word_t    s_symbols[FRAME_SYMBOLS];
static uint8_t              s_grb[WS2812B_LED_COUNT * 3];

esp_err_t led_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = WS2812B_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_tx_channel),
                        TAG, "create RMT TX channel failed");

    /* rmt_copy_encoder_config_t is an empty struct in IDF v5.4: there is
     * nothing to configure, but it must be a valid object to pass by address. */
    rmt_copy_encoder_config_t enc_cfg;
    memset(&enc_cfg, 0, sizeof(enc_cfg));
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&enc_cfg, &s_copy_encoder),
                        TAG, "create RMT copy encoder failed");

    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_channel),
                        TAG, "enable RMT channel failed");

    memset(s_grb, 0, sizeof(s_grb));

    ESP_LOGI(TAG, "RMT ready: gpio=%d leds=%d res=%dHz",
             WS2812B_GPIO, WS2812B_LED_COUNT, RMT_RESOLUTION_HZ);
    return ESP_OK;
}

esp_err_t led_set_all_color(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < WS2812B_LED_COUNT; i++) {
        s_grb[i * 3 + 0] = g;
        s_grb[i * 3 + 1] = r;
        s_grb[i * 3 + 2] = b;
    }
    return ESP_OK;
}

/* Encode one bit into a single RMT symbol. */
static inline void encode_bit(rmt_symbol_word_t *sym, int one)
{
    if (one) {
        sym->level0    = 1;
        sym->duration0 = T1H_TICKS;
        sym->level1    = 0;
        sym->duration1 = T1L_TICKS;
    } else {
        sym->level0    = 1;
        sym->duration0 = T0H_TICKS;
        sym->level1    = 0;
        sym->duration1 = T0L_TICKS;
    }
}

esp_err_t led_refresh(void)
{
    ESP_RETURN_ON_FALSE(s_tx_channel != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "led_init() not called");

    int idx = 0;
    for (int byte_i = 0; byte_i < WS2812B_LED_COUNT * 3; byte_i++) {
        uint8_t byte = s_grb[byte_i];
        for (int bit = 7; bit >= 0; bit--) {
            encode_bit(&s_symbols[idx++], (byte >> bit) & 0x01);
        }
    }

    /* Trailing latch: hold the line low, duration1 == 0 marks end of frame. */
    s_symbols[idx].level0    = 0;
    s_symbols[idx].duration0 = RESET_TICKS;
    s_symbols[idx].level1    = 0;
    s_symbols[idx].duration1 = 0;
    idx++;

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    ESP_RETURN_ON_ERROR(
        rmt_transmit(s_tx_channel, s_copy_encoder, s_symbols,
                     (size_t)idx * sizeof(rmt_symbol_word_t), &tx_cfg),
        TAG, "rmt_transmit failed");

    /* Note: this API takes milliseconds, not FreeRTOS ticks. */
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_tx_channel, 100),
                        TAG, "rmt_tx_wait_all_done failed");
    return ESP_OK;
}