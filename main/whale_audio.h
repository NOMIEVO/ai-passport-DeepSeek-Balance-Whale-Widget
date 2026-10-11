// main/whale_audio.h —— 提示音播放(worker 任务,不阻塞按键回调)。
#pragma once

#include "whale_tone.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动播放任务并初始化 codec。失败返回 false(应用应降级为静音,而不是退出)。
bool whale_audio_start(void);

// 停止播放任务并释放队列。深睡前必须调用:bsp_audio_sleep 要求先停下 PCM 读写。
void whale_audio_stop(void);

// 投递一个音效。可在按键回调里调用(只入队);队列满时丢弃,不阻塞。
void whale_audio_play(whale_sound_t sound);

#ifdef __cplusplus
}
#endif
