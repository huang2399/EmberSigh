/*
 * DreamState HAL - ESP32-S3 onboard WS2812 breathing demo
 *
 * Board  : ESP32-S3 (QFN56, rev v0.2), 16 MB flash
 * LED    : 1x onboard WS2812 RGB, single-wire data on GPIO48
 * Driver : native ESP-IDF RMT peripheral (see ws2812b.c)
 * Console: UART0 @ 115200
 *
 * Behaviour: the onboard LED breathes in deep blue, i.e. R=0, G=0 and
 * B=breath_brightness. Brightness follows a cosine curve over a 3000 ms
 * period, so the fade is smooth and non-linear (it lingers near the dark
 * and bright ends like a real breath) instead of a linear ramp.
 */
#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "ws2812b.h"

static const char *TAG = "DreamState";

/* ---- Breathing profile ---- */
#define BREATH_PERIOD_MS   3000                                 /* one full breath */
#define BREATH_STEP_MS     20                                   /* refresh interval */
#define BREATH_STEPS       (BREATH_PERIOD_MS / BREATH_STEP_MS)   /* 150 steps */

/* 2*pi as a local constant so we do not depend on how newlib exposes M_PI. */
#define BREATH_TWO_PI      6.283185307179586f

void app_main(void)
{
    printf("[BOOT] DreamState HAL Starting...\n");

    ESP_ERROR_CHECK(led_init());

    /* Start from a black frame so the LED has a known state. */
    ESP_ERROR_CHECK(led_set_all_color(0, 0, 0));
    ESP_ERROR_CHECK(led_refresh());

    printf("[STATUS] RGB Driver Initialized!\n");
    ESP_LOGI(TAG, "breathing deep blue on %d onboard LED(s), gpio %d, "
                  "period %d ms, step %d ms",
             WS2812B_LED_COUNT, WS2812B_GPIO, BREATH_PERIOD_MS, BREATH_STEP_MS);

    uint32_t step = 0;

    while (1) {
        /* Phase walks 0 -> 1 over exactly one 3000 ms period. */
        float phase = (float)(step % BREATH_STEPS) / (float)BREATH_STEPS;

        /* Cosine breath curve:
         *   cos(0)=+1 -> linear=0   (dark)
         *   cos(pi)=-1 -> linear=1  (full)
         *   cos(2pi)=+1 -> linear=0 (dark again)
         * (1 - cos)/2 therefore sweeps 0 -> 1 -> 0 smoothly and
         * non-linearly; no linear increment/decrement is involved. */
        float linear = (1.0f - cosf(phase * BREATH_TWO_PI)) * 0.5f;

        /* Perceptual gamma so the low end does not appear to "jump".
         * The curve is still derived entirely from the cosine above. */
        float shaped = powf(linear, 2.2f);

        uint8_t breath_brightness = (uint8_t)(shaped * 255.0f + 0.5f);

        /* Deep blue: R=0, G=0, B=brightness. */
        ESP_ERROR_CHECK(led_set_all_color(0, 0, breath_brightness));
        ESP_ERROR_CHECK(led_refresh());

        vTaskDelay(pdMS_TO_TICKS(BREATH_STEP_MS));
        step++;
    }
}