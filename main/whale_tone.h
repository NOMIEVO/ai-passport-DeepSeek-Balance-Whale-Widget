// main/whale_tone.h —— 提示音的 PCM 合成(纯计算,可在主机上验证)。
//
// ESP32-C3 没有硬件浮点,也不带音频解码器,所以音效一律用代码合成:正弦查表 +
// 线性滑音 + 包络,不引入任何音频文件。方波分支给复古的"金币"声用。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WHALE_TONE_SAMPLE_RATE 16000   // 16 kHz 单声道,与 ES8311 播放格式一致

// 音色取自开源挂件 DeepSeek-Balance-Whale-Widget(见 docs/development/applications/)。
// 挂件的音效是 wav/mp3 文件,名片上没有文件系统也不带解码器,所以全部用合成复刻:
// 用 tools/analyze_wav.py 量出原音的包络与基频轨迹,再落到下面的音符表里。
// 「经验球」「钟声」这类带噪声的取样音色只能复刻到"听感同族",不是逐采样还原。
typedef enum {
    WHALE_SND_NONE = 0,
    WHALE_SND_PRESS,     // 按压:小黄鸭「呀」上滑(挂件预置音效组「小黄鸭·按下」)
    WHALE_SND_RELEASE,   // 松开:小黄鸭「呀」下滑(同组「松开」)
    WHALE_SND_COIN,      // 消费:金币掉落(挂件 bubble-money1 配图对应的消费提示)
    WHALE_SND_ORB,       // 任务结束音:Minecraft 经验球(挂件内置默认)
    WHALE_SND_BELL,      // 任务结束音 A(挂件内置第二预设,钟声)
    WHALE_SND_CONNECT,   // 联网 / 连接成功音
    WHALE_SND_CLICK,     // 界面切换的轻提示
    WHALE_SND_WARN,      // 余额 / 预算预警的两声提示
    WHALE_SND_ERROR,     // 取余额失败的低提示
} whale_sound_t;

typedef struct {
    uint16_t freq_start_hz;   // 起始频率;0 表示这一段是静音(用作音符间隙)
    uint16_t freq_end_hz;     // 结束频率,与起始不同即为滑音
    uint16_t duration_ms;
    uint8_t amplitude;        // 0..100,100 = 满量程
    uint8_t attack_ms;        // 起振时间,避免爆音
    bool square;              // true = 方波(复古),false = 正弦
} whale_tone_step_t;

// 取某个音效的音符序列。找不到时 count 置 0。
const whale_tone_step_t *whale_tone_steps(whale_sound_t sound, size_t *out_count);

// 按序列渲染 PCM(单声道 int16)。返回实际写入的采样数(受 out_samples 限制)。
// out 为 NULL 时只计算长度,方便调用方先算缓冲区再分配。
size_t whale_tone_render(const whale_tone_step_t *steps, size_t step_count,
                         uint32_t sample_rate, int16_t *out, size_t out_samples);

// 只渲染从 offset_samples 开始的一段。播放端用它把整段音效切成小块送进 I2S,
// 无需在内存里铺开整条音轨(ESP32-C3 没有 PSRAM)。
size_t whale_tone_render_offset(const whale_tone_step_t *steps, size_t step_count,
                                uint32_t sample_rate, size_t offset_samples,
                                int16_t *out, size_t out_samples);

// 某个音效的总采样数(用于估算播放时长与主机测试的断言)。
size_t whale_tone_total_samples(whale_sound_t sound, uint32_t sample_rate);

#ifdef __cplusplus
}
#endif
