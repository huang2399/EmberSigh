/*
 * ESP32-S3-EYE BSP Display Test
 * Uses official BSP: bsp_display_new() + esp_lcd_panel_draw_bitmap()
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp32_s3_eye.h"
#include "bsp/display.h"

static const char *TAG = "bsp_lcd";

void app_main(void)
{
    printf("\n=== ESP32-S3-EYE BSP WHITE SCREEN ===\n\n");
    fflush(stdout);

    /* Init display via BSP (handles SPI, ST7789, HW reset, LEDC backlight) */
    ESP_LOGI(TAG, "Init via bsp_display_new...");
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    bsp_display_config_t cfg = { .max_transfer_sz = 240 * 240 * 2 };
    ESP_ERROR_CHECK(bsp_display_new(&cfg, &panel, &io));
    ESP_LOGI(TAG, "Display init OK");

    /* Turn on display */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* Turn on backlight (BSP LEDC PWM on GPIO48) */
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_LOGI(TAG, "Backlight ON");

    /* Fill screen with white (0xFFFF) via BSP draw_bitmap */
    ESP_LOGI(TAG, "Filling WHITE...");
    uint16_t *buf = heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 240 * 240; i++) buf[i] = 0xFFFF;
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, 240, 240, buf));
    free(buf);
    ESP_LOGI(TAG, "DONE - white screen running forever");

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}