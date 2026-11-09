/*
 * DreamState 硬件抽象层 (HAL)
 * 真实外设驱动：MAX98357 I2S 功放 / MOS 管加湿器 / WS2812 灯带
 */
#pragma once
#include "esp_err.h"
#include "driver/i2s_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化所有硬件外设（I2S、GPIO、LED灯带），app_main 早期调用一次 */
esp_err_t hal_hardware_init(void);

/** 香氛 MOS 管 GPIO 初始化（GPIO1 输出，默认低电平=关闭，并启动后台香氛任务） */
esp_err_t fragrance_init(void);

/**
 * 设置香氛释放占空比 0~100（时间切片 PWM，非 LEDC 高频斩波）
 * 0=关闭，100=常开，1~99=在 2000ms 窗口内按比例间歇工作
 * @note 非阻塞：只写入目标值立即返回，电平由 fragrance_task 每 10ms 步进翻转，
 *       可在麦克风采集任务中安全调用
 */
void set_fragrance_duty(uint8_t duty);

/** 读取当前目标占空比（0~100），调试/UI 显示用 */
uint8_t fragrance_get_duty(void);

/** 音频音量控制（0.0 = 静音, 1.0 = 满音量），供 audio_player 任务实时读取 */
extern volatile float hal_audio_volume;
extern i2s_chan_handle_t hal_i2s_tx;

/** 降低喇叭音量（MAX98357 I2S 静音） */
void hal_lower_volume(void);

/** 设置音量（0.0 ~ 1.0），供主程序做「渐入」斜坡 */
void hal_set_audio_volume(float vol);

/** 停止香氛释放 + 灯带变暗红防守色 */
void hal_stop_fragrance(void);

/** 仅恢复香氛 + 灯带（不动音量，配合渐入使用） */
void hal_restore_fragrance_led(void);

/** 恢复白噪音音量 + 开启香氛 + 灯带恢复深蓝暖光 */
void hal_restore_environment(void);

/** 待机模式：关灯、断香、静音 */
void hal_enter_standby(void);

#ifdef __cplusplus
}
#endif