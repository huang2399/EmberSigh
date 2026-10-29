/*
 * DreamState 音频播放器
 * 实时白噪音生成 → I2S_NUM_1 → MAX98357
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 启动白噪音播放任务（FreeRTOS，循环播放） */
void audio_player_start(void);

#ifdef __cplusplus
}
#endif