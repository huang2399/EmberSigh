/*
 * ESP32-S3-EYE DreamState Monitor
 * Audio + ES8311 Codec + LCD
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp32_s3_eye.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "dreamstate";

#define SAMPLE_RATE 16000
#define BUFFER_SAMPLES 512

#define LCD_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK (40*1000*1000)
#define LCD_MOSI GPIO_NUM_13
#define LCD_SCLK GPIO_NUM_14
#define LCD_CS GPIO_NUM_10
#define LCD_DC GPIO_NUM_5
#define LCD_RST GPIO_NUM_9
#define LCD_BACKLIGHT GPIO_NUM_15
#define LCD_H_RES 240
#define LCD_V_RES 240
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8

#define COLOR_BG 0x0010
#define COLOR_TEXT 0xFFFF
#define COLOR_ALERT_BG 0xF800
#define COLOR_ALERT_TEXT 0xFFFF
#define NOISE_THRESHOLD 500.0f

#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define SCALE 4