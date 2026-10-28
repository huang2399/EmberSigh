/*
 * ESP32-S3-EYE DreamState Monitor
 * Uses Official BSP (Board Support Package)
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp/esp32_s3_eye.h"

static const char *TAG = "dreamstate";

void app_main(void)
{
    printf("\n=== DreamState BSP Color Test ===\n\n");
    fflush(stdout);

    /* 1. Init display + LVGL via BSP (auto pin/wiring) */
    ESP_LOGI(TAG, "Calling bsp_display_start()...");
    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start() FAILED!");
        return;
    }
    ESP_LOGI(TAG, "Display initialized OK");

    /* 2. Turn on backlight */
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "Backlight ON");

    /* 3. Get default screen */
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    /* 4. Color cycle loop */
    lv_color_t colors[] = {
        lv_color_black(),
        lv_color_hex(0xFF0000),  /* Red   */
        lv_color_hex(0x00FF00),  /* Green */
        lv_color_hex(0x0000FF),  /* Blue  */
        lv_color_white(),
    };
    const char *names[] = {"BLACK", "RED", "GREEN", "BLUE", "WHITE"};
    int ci = 0;

    while (1) {
        ESP_LOGI(TAG, ">>> Fill %s", names[ci]);

        bsp_display_lock(0);
        lv_obj_set_style_bg_color(scr, colors[ci], LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
        bsp_display_unlock();

        vTaskDelay(pdMS_TO_TICKS(2000));

        ci = (ci + 1) % 5;
    }
}