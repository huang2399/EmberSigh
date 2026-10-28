#include "hal_hardware.h"
#include <stdio.h>

void hal_lower_volume(void)
{
    printf("[HAL] 硬件指令：降低喇叭音量...\n");
    fflush(stdout);
}

void hal_stop_fragrance(void)
{
    printf("[HAL] 硬件指令：停止香氛释放...\n");
    fflush(stdout);
}

void hal_restore_environment(void)
{
    printf("[HAL] 硬件指令：恢复白噪音与香氛...\n");
    fflush(stdout);
}