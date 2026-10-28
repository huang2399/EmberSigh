/*
 * ESP32-S3-EYE LCD RGB Color Test v5
 * Fix: RGB element order + byte-swap in fill_screen (endian fix)
 * ESP32 is little-endian; ST7789 expects big-endian 16-bit pixels
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "lcd_rgb";

#define LCD_HOST       SPI3_HOST
#define LCD_PIXEL_CLOCK (80 * 1000 * 1000)
#define LCD_MOSI       GPIO_NUM_47
#define LCD_SCLK       GPIO_NUM_21
#define LCD_CS         GPIO_NUM_44
#define LCD_DC         GPIO_NUM_43
#define LCD_RST        GPIO_NUM_45
#define LCD_BACKLIGHT  GPIO_NUM_48

/* byte-swap: ESP32 little-endian -> ST7789 big-endian */
static inline uint16_t swap16(uint16_t v) { return (v >> 8) | (v << 8); }

static void fill_screen(esp_lcd_panel_io_handle_t io, uint16_t color)
{
    uint8_t col[4] = {0, 0, 0, 239};
    esp_lcd_panel_io_tx_param(io, 0x2A, col, 4);
    uint8_t row[4] = {0, 0, 0, 239};
    esp_lcd_panel_io_tx_param(io, 0x2B, row, 4);
    esp_lcd_panel_io_tx_param(io, 0x2C, NULL, 0);

    uint16_t swapped = swap16(color);
    uint16_t line[240];
    for (int i = 0; i < 240; i++) line[i] = swapped;
    for (int y = 0; y < 240; y++) {
        esp_lcd_panel_io_tx_color(io, -1, (uint8_t *)line, 240 * 2);
    }
}

void app_main(void)
{
    printf("\n=== ESP32-S3-EYE LCD RGB TEST v5 (byte-swap fix) ===\n");
    printf("SPI3 47/21/44/43/45/48  |  rgb_ele_order=RGB + swap16\n");
    printf("RED(2s) -> GREEN(2s) -> BLUE(2s) x3\n\n");
    fflush(stdout);

    /* Backlight constant HIGH */
    ESP_LOGI(TAG, "Backlight GPIO48 HIGH");
    gpio_config_t bl = { .pin_bit_mask = 1ULL << LCD_BACKLIGHT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl); gpio_set_level(LCD_BACKLIGHT, 1);

    /* HW reset GPIO45 */
    ESP_LOGI(TAG, "HW reset GPIO45: LOW 20ms -> HIGH 130ms");
    gpio_config_t rst = { .pin_bit_mask = 1ULL << LCD_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(130));

    /* SPI3 */
    ESP_LOGI(TAG, "SPI3: MOSI=%d SCLK=%d", LCD_MOSI, LCD_SCLK);
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI, .miso_io_num = -1, .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 240 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    /* Panel IO */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS, .dc_gpio_num = LCD_DC,
        .spi_mode = 0, .pclk_hz = LCD_PIXEL_CLOCK,
        .trans_queue_depth = 10, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    /* ST7789 - RGB order + byte-swap in data */
    ESP_LOGI(TAG, "ST7789: rgb_ele_order=RGB, data byte-swapped");
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_mirror(panel, false, false);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_LOGI(TAG, "Display ON - RGB cycle start");

    /* RGB cycle x3 */
    for (int loop = 0; loop < 3; loop++) {
        ESP_LOGI(TAG, "CYCLE %d/3: RED (0xF800)", loop + 1);
        fill_screen(io_handle, 0xF800);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "CYCLE %d/3: GREEN (0x07E0)", loop + 1);
        fill_screen(io_handle, 0x07E0);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "CYCLE %d/3: BLUE (0x001F)", loop + 1);
        fill_screen(io_handle, 0x001F);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(TAG, "RGB test done!");
    esp_lcd_panel_disp_on_off(panel, false);
    gpio_set_level(LCD_BACKLIGHT, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}