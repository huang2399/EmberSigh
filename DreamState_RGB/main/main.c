/*
 * DreamState HAL - ESP32-S3 onboard WS2812 + digital-twin serial link
 *
 * Board  : ESP32-S3 (QFN56, rev v0.2), 16 MB flash
 * LED    : 1x onboard WS2812 RGB, single-wire data on GPIO48
 * Driver : native ESP-IDF RMT peripheral (see ws2812b.c)
 * Console: UART0 @ 115200 on GPIO43(TX)/GPIO44(RX)
 *
 * Serial protocol with the EmberSigh web host page (网页版/index(1).html):
 *
 *   host -> device   "CALM\n" | "CALMING\n" | "NOISE\n"
 *   device -> host   "DATA | RMS: <n> | STATE: <CALM|CALMING|NOISE>\n"  (500 ms)
 *   device -> host   "ACK <CMD>\n"                                        (on command)
 *
 * The host drops any line that does not match its regex, so unknown commands
 * only produce a diagnostic line and never change state.
 *
 * IMPORTANT / honesty note:
 *   This board has NO microphone. The RMS value in the DATA frames is a
 *   synthetic, per-state constant used so the web UI has a numeric field to
 *   display. It is NOT a measured acoustic level and must not be presented as
 *   a real sensor reading.
 *
 * LED behaviour:
 *   CALM     deep blue (0,0,255), cosine breathing over 3000 ms
 *   CALMING  orange   (255,165,0), cosine breathing over 3000 ms
 *   NOISE    red      (255,0,0),   steady at full level for legibility
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"

#include "ws2812b.h"

static const char *TAG = "DreamState";

/* ---- Breathing profile ---- */
#define BREATH_PERIOD_MS   3000                                 /* one full breath */
#define BREATH_STEP_MS     20                                   /* refresh interval */
#define BREATH_STEPS       (BREATH_PERIOD_MS / BREATH_STEP_MS)   /* 150 steps */
#define BREATH_TWO_PI      6.283185307179586f

/* ---- Telemetry cadence (host expects ~500 ms) ---- */
#define TELEMETRY_PERIOD_MS   500
#define TELEMETRY_EVERY_STEPS (TELEMETRY_PERIOD_MS / BREATH_STEP_MS)

/* ---- Serial link ---- */
#define LINK_UART       UART_NUM_0
#define LINK_TX_GPIO    43
#define LINK_RX_GPIO    44
#define LINK_BAUD       115200
#define CMD_MAX_LEN     16

typedef enum {
    DS_CALM = 0,
    DS_CALMING,
    DS_NOISE,
} ds_state_t;

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static ds_state_t s_state = DS_CALM;

static const char *state_name(ds_state_t st)
{
    switch (st) {
    case DS_CALMING: return "CALMING";
    case DS_NOISE:   return "NOISE";
    default:         return "CALM";
    }
}

static rgb_t state_base_color(ds_state_t st)
{
    switch (st) {
    case DS_CALMING: return (rgb_t){ 255, 165, 0 };   /* orange */
    case DS_NOISE:   return (rgb_t){ 255, 0, 0 };     /* red    */
    default:         return (rgb_t){ 0, 0, 255 };     /* deep blue */
    }
}

/* Synthetic per-state level for the UI. See the honesty note above: this is
 * not a microphone measurement. */
static unsigned state_demo_rms(ds_state_t st)
{
    switch (st) {
    case DS_CALMING: return 62;
    case DS_NOISE:   return 178;
    default:         return 12;
    }
}

static void emit_data_frame(void)
{
    printf("DATA | RMS: %u | STATE: %s\n",
           state_demo_rms(s_state), state_name(s_state));
}

static void apply_command(const char *cmd)
{
    ds_state_t next;

    if (strcmp(cmd, "CALM") == 0) {
        next = DS_CALM;
    } else if (strcmp(cmd, "CALMING") == 0) {
        next = DS_CALMING;
    } else if (strcmp(cmd, "NOISE") == 0) {
        next = DS_NOISE;
    } else {
        printf("[CMD] ignored unknown command: %s\n", cmd);
        return;
    }

    s_state = next;
    printf("[CMD] %s -> state %s\n", cmd, state_name(next));
    printf("ACK %s\n", cmd);          /* host logs any line containing ACK */
    emit_data_frame();                /* immediate reply, no 500 ms wait */
}

static void poll_link(void)
{
    static char line[CMD_MAX_LEN + 1];
    static int len = 0;
    uint8_t chunk[32];

    int n = uart_read_bytes(LINK_UART, chunk, sizeof(chunk), 0);
    for (int i = 0; i < n; i++) {
        uint8_t ch = chunk[i];

        if (ch == '\n' || ch == '\r') {
            if (len > 0) {
                line[len] = '\0';
                apply_command(line);
                len = 0;
            }
            continue;
        }

        if (len < CMD_MAX_LEN) {
            line[len++] = (char)toupper((int)ch);
        } else {
            len = 0;   /* overlong token: drop it and resync on next newline */
        }
    }
}

static esp_err_t link_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(LINK_UART, &cfg),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");
    /* RX ring buffer sized for a burst of commands; TX is driven by printf. */
    ESP_RETURN_ON_ERROR(uart_driver_install(LINK_UART, 2048, 0, 0, NULL, 0),
                        TAG, "uart_driver_install failed");
    return ESP_OK;
}

void app_main(void)
{
    /* Configure UART0 *before* the first printf.
     * uart_driver_install() resets the UART0 TX FIFO, so any console bytes still
     * queued from an earlier printf get flushed and arrive as garbage. Doing the
     * link setup first keeps the boot banner intact. */
    esp_err_t err = link_init();

    printf("[BOOT] DreamState HAL Starting...\n");

    ESP_ERROR_CHECK(led_init());

    /* Start from a black frame so the LED has a known state. */
    ESP_ERROR_CHECK(led_set_all_color(0, 0, 0));
    ESP_ERROR_CHECK(led_refresh());

    printf("[STATUS] RGB Driver Initialized!\n");

    if (err == ESP_OK) {
        printf("[LINK] UART0 @ %d bps (TX%d/RX%d) | commands: CALM CALMING NOISE\n",
               LINK_BAUD, LINK_TX_GPIO, LINK_RX_GPIO);
    } else {
        printf("[LINK] ** UART setup failed (0x%x) | running local-only **\n", err);
    }

    ESP_LOGI(TAG, "breathing deep blue on %d onboard LED(s), gpio %d, "
                  "period %d ms, step %d ms",
             WS2812B_LED_COUNT, WS2812B_GPIO, BREATH_PERIOD_MS, BREATH_STEP_MS);

    uint32_t step = 0;

    while (1) {
        poll_link();

        /* Phase walks 0 -> 1 over exactly one 3000 ms period. */
        float phase = (float)(step % BREATH_STEPS) / (float)BREATH_STEPS;

        /* Cosine breath curve: cos(0)=+1 -> 0 (dark), cos(pi)=-1 -> 1 (full),
         * cos(2pi)=+1 -> 0. (1-cos)/2 gives 0 -> 1 -> 0, smooth and
         * non-linear; no linear increment/decrement anywhere. */
        float linear = (1.0f - cosf(phase * BREATH_TWO_PI)) * 0.5f;

        /* Perceptual gamma so the low end does not appear to "jump". */
        float shaped = powf(linear, 2.2f);

        rgb_t base = state_base_color(s_state);

        if (s_state == DS_NOISE) {
            /* Alarm state: steady full red reads better than a slow breath. */
            ESP_ERROR_CHECK(led_set_all_color(base.r, base.g, base.b));
        } else {
            uint16_t k = (uint16_t)(shaped * 255.0f + 0.5f);
            ESP_ERROR_CHECK(led_set_all_color(
                (uint8_t)((uint32_t)base.r * k / 255u),
                (uint8_t)((uint32_t)base.g * k / 255u),
                (uint8_t)((uint32_t)base.b * k / 255u)));
        }
        ESP_ERROR_CHECK(led_refresh());

        vTaskDelay(pdMS_TO_TICKS(BREATH_STEP_MS));
        step++;

        if ((step % TELEMETRY_EVERY_STEPS) == 0) {
            emit_data_frame();
        }
    }
}