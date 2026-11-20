/*
 * ws2812b.h - WS2812B single-wire RGB driver for ESP-IDF
 *
 * Implemented on top of the native ESP-IDF RMT TX peripheral.
 * No third-party libraries are used.
 *
 * Hardware: 1x onboard WS2812 RGB LED, data line on GPIO48.
 * Protocol: GRB byte order, 800 kHz, bit-encoded as:
 *   '0' -> T0H high, T0L low
 *   '1' -> T1H high, T1L low
 *   frame latch -> line held low for >= 50 us
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board wiring ----
 * The board carries a single onboard WS2812 RGB LED on GPIO48 (not an
 * external strip). LED_COUNT sizes the frame buffer inside the driver,
 * so it must match the real number of pixels on the wire: 1.
 */
#define WS2812B_GPIO       48
#define WS2812B_LED_COUNT  1

/**
 * @brief Initialize the RMT TX channel and encoder used to drive the strip.
 *        Must be called once before any other led_* function.
 */
esp_err_t led_init(void);

/**
 * @brief Set every LED to the same colour.
 *        Only updates the internal frame buffer; call led_refresh() to push
 *        the frame out on the wire.
 *
 * @param r  red   0..255
 * @param g  green 0..255
 * @param b  blue  0..255
 */
esp_err_t led_set_all_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Encode the current frame buffer into RMT symbols and transmit it,
 *        including the trailing reset/latch pulse. Blocks until the transfer
 *        has completed.
 */
esp_err_t led_refresh(void);

#ifdef __cplusplus
}
#endif