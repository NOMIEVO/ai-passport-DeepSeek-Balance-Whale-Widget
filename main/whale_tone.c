// main/whale_tone.c —— 提示音合成的实现。
//
// 合成方式刻意保持简单:一个相位累加器 + 256 点正弦表 + 分段线性包络。没有浮点、
// 没有 malloc、没有依赖,因此这份代码可以直接在主机上编译验证(见 tests/test_whale_tone.c)。
#include "whale_tone.h"

#include <string.h>

#define SINE_TABLE_SIZE 256

// 256 点正弦表,int16 量化。避免在没有 FPU 的 C3 上调用 sinf。
static const int16_t SINE_TABLE[SINE_TABLE_SIZE] = {
    0,      804,    1608,   2410,   3212,   4011,   4808,   5602,
    6393,   7179,   7962,   8739,   9512,   10278,  11039,  11792,
    12539,  13279,  14009,  14732,  15446,  16151,  16845,  17530,
    18204,  18867,  19519,  20159,  20787,  21402,  22005,  22594,
    23169,  23731,  24278,  24811,  25329,  25832,  26318,  26789,
    27245,  27683,  28105,  28510,  28897,  29268,  29621,  29956,
    30272,  30571,  30851,  31113,  31356,  31580,  31785,  31971,
    32137,  32284,  32412,  32520,  32609,  32678,  32728,  32757,
    32767,  32757,  32728,  32678,  32609,  32520,  32412,  32284,
    32137,  31971,  31785,  31580,  31356,  31113,  30851,  30571,
    30272,  29956,  29621,  29268,  28897,  28510,  28105,  27683,
    27245,  26789,  26318,  25832,  25329,  24811,  24278,  23731,
    23169,  22594,  22005,  21402,  20787,  20159,  19519,  18867,
    18204,  17530,  16845,  16151,  15446,  14732,  14009,  13279,
    12539,  11792,  11039,  10278,  9512,    8739,   7962,   7179,
    6393,   5602,   4808,   4011,   3212,   2410,   1608,   804,
    0,      -804,   -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
    -6393,  -7179,  -7962,  -8739,  -9512,  -10278, -11039, -11792,
    -12539, -13279, -14009, -14732, -15446, -16151, -16845, -17530,
    -18204, -18867, -19519, -20159, -20787, -21402, -22005, -22594,
    -23169, -23731, -24278, -24811, -25329, -25832, -26318, -26789,
    -27245, -27683, -28105, -28510, -28897, -29268, -29621, -29956,
    -30272, -30571, -30851, -31113, -31356, -31580, -31785, -31971,
    -32137, -32284, -32412, -32520, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32520, -32412, -32284,
    -32137, -31971, -31785, -31580, -31356, -31113, -30851, -30571,
    -30272, -29956, -29621, -29268, -28897, -28510, -28105, -27683,
    -27245, -26789, -26318, -25832, -25329, -24811, -24278, -23731,
    -23169, -22594, -22005, -21402, -20787, -20159, -19519, -18867,
    -18204, -17530, -16845, -16151, -15446, -14732, -14009, -13279,
    -12539, -11792, -11039, -10278, -9512,   -8739,  -7962,  -7179,
    -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,  -804,
};

// 按压「呀」:方波快速上滑。对应挂件 preload 的 Ya1.mp3(小黄鸭按下)——
// 实测那一段是 60~90 ms 的上扬滑音,尾部带一点回落,这里用两段方波近似。
static const whale_tone_step_t PRESS_STEPS[] = {
    { 560,  1240, 60,  90, 8, true  },
    { 1240, 900,  95,  72, 8, true  },
};

// 松开:同一支音色的下行版(Ya2.mp3)。
static const whale_tone_step_t RELEASE_STEPS[] = {
    { 900,  520,  120, 74, 8, true  },
};

// 金币掉落:短促的 B5 → 延音的 E6,方波+快速衰减,就是经典电子硬币声。
static const whale_tone_step_t COIN_STEPS[] = {
    { 988,  988,  70,  85, 2, true  },
    { 1319, 1319, 420, 78, 2, true  },
};

// 任务结束音(挂件默认):Minecraft 经验球 minecraft-exp-orb.wav。
// 实测 48 kHz/0.62 s,RMS 从 33% 单调衰减到 2%(约 250 ms 后基本听不见),
// 零穿越率起始 ~6300 Hz —— 是三记极短的高频 pluck 依次上行,不是单音。
// 所以这里用三段方波(方波的谐波把听感推亮),时长 40/40/210 ms。
static const whale_tone_step_t ORB_STEPS[] = {
    { 1760, 1760, 40,  92, 2, true  },
    { 2349, 2349, 40,  88, 2, true  },
    { 3136, 3136, 210, 84, 2, true  },
};

// 任务结束音 A:task-end-a.wav。实测 1.66 s,能量集中在 70~200 ms,
// 零穿越率 2050 → 1350 Hz 递减 —— 是一记钟声:起音高、随后落回基频并长时间延音。
// 复刻成 E6 起音 + B5 主体(正弦,长衰减)。
static const whale_tone_step_t BELL_STEPS[] = {
    { 1319, 1319, 70,  66, 4, false },
    { 988,  988,  300, 70, 4, false },
};

// 余额 / 预算预警:两声等高的中音「嘟 · 嘟」,与金币声拉开区分度。
static const whale_tone_step_t WARN_STEPS[] = {
    { 880, 880, 110, 62, 4, false },
    { 0,   0,   60,  0,  0, false },
    { 880, 880, 110, 62, 4, false },
};

// 连接成功:C5-E5-G5-C6 上行琶音,正弦,音符之间留 15 ms 空隙。
static const whale_tone_step_t CONNECT_STEPS[] = {
    { 523,  523,  95,  72, 6, false },
    { 0,    0,    15,  0,  0, false },
    { 659,  659,  95,  72, 6, false },
    { 0,    0,    15,  0,  0, false },
    { 784,  784,  95,  72, 6, false },
    { 0,    0,    15,  0,  0, false },
    { 1047, 1047, 210, 72, 6, false },
};

// 界面切换:一记短促的高音。
static const whale_tone_step_t CLICK_STEPS[] = {
    { 880, 880, 45, 45, 2, false },
};

// 取余额失败:两声下行低音,和金币声拉开区分度。
static const whale_tone_step_t ERROR_STEPS[] = {
    { 233, 233, 130, 60, 4, false },
    { 0,   0,   30,  0,  0, false },
    { 175, 175, 190, 60, 4, false },
};

const whale_tone_step_t *whale_tone_steps(whale_sound_t sound, size_t *out_count)
{
    size_t count = 0;
    const whale_tone_step_t *steps = NULL;

    switch (sound) {
    case WHALE_SND_PRESS:
        steps = PRESS_STEPS;
        count = sizeof(PRESS_STEPS) / sizeof(PRESS_STEPS[0]);
        break;
    case WHALE_SND_RELEASE:
        steps = RELEASE_STEPS;
        count = sizeof(RELEASE_STEPS) / sizeof(RELEASE_STEPS[0]);
        break;
    case WHALE_SND_COIN:
        steps = COIN_STEPS;
        count = sizeof(COIN_STEPS) / sizeof(COIN_STEPS[0]);
        break;
    case WHALE_SND_ORB:
        steps = ORB_STEPS;
        count = sizeof(ORB_STEPS) / sizeof(ORB_STEPS[0]);
        break;
    case WHALE_SND_BELL:
        steps = BELL_STEPS;
        count = sizeof(BELL_STEPS) / sizeof(BELL_STEPS[0]);
        break;
    case WHALE_SND_WARN:
        steps = WARN_STEPS;
        count = sizeof(WARN_STEPS) / sizeof(WARN_STEPS[0]);
        break;
    case WHALE_SND_CONNECT:
        steps = CONNECT_STEPS;
        count = sizeof(CONNECT_STEPS) / sizeof(CONNECT_STEPS[0]);
        break;
    case WHALE_SND_CLICK:
        steps = CLICK_STEPS;
        count = sizeof(CLICK_STEPS) / sizeof(CLICK_STEPS[0]);
        break;
    case WHALE_SND_ERROR:
        steps = ERROR_STEPS;
        count = sizeof(ERROR_STEPS) / sizeof(ERROR_STEPS[0]);
        break;
    default:
        break;
    }

    if (out_count != NULL) *out_count = count;
    return count == 0 ? NULL : steps;
}

// 包络:起振线性上升,之后按 (1-t)^2 衰减到 0 —— 收尾一定是静音,不会爆音。
static int32_t envelope_at(uint32_t index, uint32_t total, uint32_t attack)
{
    if (total == 0) return 0;
    if (attack >= total) attack = total / 2;
    if (attack == 0) attack = 1;

    if (index < attack) {
        return (int32_t)(((uint64_t)index * 65536u) / attack);
    }
    const uint32_t tail = total - attack;
    if (tail == 0) return 65536;
    const uint32_t elapsed = index - attack;
    if (elapsed >= tail) return 0;

    // (1 - t)^2,用 16.16 定点算,避免浮点。
    const uint32_t remaining = tail - elapsed;
    const uint64_t ratio = ((uint64_t)remaining * 65536u) / tail;
    const uint64_t squared = (ratio * ratio) >> 16;
    return (int32_t)(squared > 65536u ? 65536u : squared);
}

size_t whale_tone_render_offset(const whale_tone_step_t *steps, size_t step_count,
                                uint32_t sample_rate, size_t offset_samples,
                                int16_t *out, size_t out_samples)
{
    if (steps == NULL || sample_rate == 0) return 0;

    size_t written = 0;          // 已写入 out 的采样数
    size_t skipped = 0;          // offset 之前被跳过的采样数
    for (size_t s = 0; s < step_count; s++) {
        const whale_tone_step_t step = steps[s];
        const uint32_t total = (uint32_t)sample_rate * step.duration_ms / 1000u;
        const uint32_t attack = (uint32_t)sample_rate * step.attack_ms / 1000u;

        uint32_t phase = 0;                      // 0..65535 对应一个完整周期
        uint32_t freq_q16 = 0;                   // 当前频率的 16.16 定点值
        const uint32_t freq_start_q16 = ((uint32_t)step.freq_start_hz << 16) / sample_rate;
        const uint32_t freq_end_q16 = ((uint32_t)step.freq_end_hz << 16) / sample_rate;

        for (uint32_t i = 0; i < total; i++) {
            int16_t value = 0;
            if (step.freq_start_hz != 0 && step.duration_ms != 0) {
                // 段内线性滑音。
                const uint32_t progress = (total > 1) ? ((uint64_t)i * 65536u) / (total - 1) : 0;
                freq_q16 = freq_start_q16 +
                           (uint32_t)(((uint64_t)(freq_end_q16 - freq_start_q16) * progress) >> 16);
                phase = (phase + freq_q16) & 0xFFFFu;   // 相位累加,自然回绕

                int32_t wave;
                if (step.square) {
                    wave = (phase < 0x8000u) ? 32767 : -32767;
                } else {
                    wave = SINE_TABLE[(phase >> 8) & 0xFFu];
                }

                const int32_t env = envelope_at(i, total, attack);
                const int32_t amplitude = (int32_t)step.amplitude;
                // value = wave * amplitude/100 * env/65536
                int64_t scaled = (int64_t)wave * amplitude;
                scaled = (scaled / 100) * env;
                scaled >>= 16;
                if (scaled > 32767) scaled = 32767;
                if (scaled < -32767) scaled = -32767;
                value = (int16_t)scaled;
            }

            if (skipped < offset_samples) {
                skipped++;
                continue;
            }
            if (out == NULL) {
                written++;          // 只问长度:统计 offset 之后还剩多少采样
                continue;
            }
            if (written < out_samples) {
                out[written] = value;
                written++;
            }
        }
    }
    return written;
}

size_t whale_tone_render(const whale_tone_step_t *steps, size_t step_count,
                         uint32_t sample_rate, int16_t *out, size_t out_samples)
{
    return whale_tone_render_offset(steps, step_count, sample_rate, 0, out, out_samples);
}

size_t whale_tone_total_samples(whale_sound_t sound, uint32_t sample_rate)
{
    size_t count = 0;
    const whale_tone_step_t *steps = whale_tone_steps(sound, &count);
    if (steps == NULL) return 0;
    return whale_tone_render(steps, count, sample_rate, NULL, 0);
}
