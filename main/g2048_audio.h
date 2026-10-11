// main/g2048_audio.h —— 2048 的合成音效。
//
// 声音全部由代码合成(方波),不占用 Flash 存音频文件,也不依赖任何素材。
// 播放是阻塞的 I2S 写入,所以单独放在一个 worker 任务里,按键回调只投递一个枚举。
#pragma once

#include <stdbool.h>

typedef enum {
    G2048_SND_MOVE = 0,   // 滑动成功
    G2048_SND_MERGE,      // 发生了合并
    G2048_SND_WIN,        // 首次合出 2048
    G2048_SND_OVER,       // 无路可走
    G2048_SND_BLOCKED,    // 这个方向动不了
} g2048_sound_t;

// 创建音效任务。音频 codec 不可用时会失败,调用方按"没声音也能玩"降级。
bool g2048_audio_start(void);

// 停止音效任务。删页面/进 deep sleep 前必须调用。
void g2048_audio_stop(void);

// 投递一个音效;队列满时直接丢弃,绝不阻塞调用方。
void g2048_audio_play(g2048_sound_t sound);
