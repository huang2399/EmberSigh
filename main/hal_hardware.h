/*
 * DreamState 硬件抽象层 (HAL)
 * 真实外设驱动：MAX98357 I2S 功放 / MOS 管加湿器 / WS2812 灯带
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化所有硬件外设（I2S、GPIO、LED灯带），app_main 早期调用一次 */
esp_err_t hal_hardware_init(void);

/** 降低喇叭音量（MAX98357 I2S 静音） */
void hal_lower_volume(void);

/** 停止香氛释放 + 灯带变暗红防守色 */
void hal_stop_fragrance(void);

/** 恢复白噪音音量 + 开启香氛 + 灯带恢复深蓝暖光 */
void hal_restore_environment(void);

#ifdef __cplusplus
}
#endif