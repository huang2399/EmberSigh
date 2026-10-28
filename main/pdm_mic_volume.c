#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

void app_main(void)
{
    gpio_set_direction(48, GPIO_MODE_OUTPUT);

    while (1) {
        gpio_set_level(48, 1);
        printf("GPIO48 = HIGH\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));

        gpio_set_level(48, 0);
        printf("GPIO48 = LOW\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}