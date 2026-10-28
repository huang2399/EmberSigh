/*
 * DreamState 硬件抽象层 (HAL)
 * 预留外设控制接口，硬件到货后填入真实驱动代码
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 降低喇叭音量（MAX98357 I2S 功放） */
void hal_lower_volume(void);

/** 停止香氛释放（MOS 管关闭加湿器） */
void hal_stop_fragrance(void);

/** 恢复白噪音音量和香氛 */
void hal_restore_environment(void);

#ifdef __cplusplus
}
#endif