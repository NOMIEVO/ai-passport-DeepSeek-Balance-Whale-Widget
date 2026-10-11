// 提示音合成的主机测试:时长、包络收尾、幅度上限、音符间隙与可复现性。
// 这些量在真机上只能"听个大概",放到主机上断言才可靠。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "whale_tone.h"

#define RATE WHALE_TONE_SAMPLE_RATE

static int16_t abs_max(const int16_t *buf, size_t count)
{
    int peak = 0;
    for (size_t i = 0; i < count; i++) {
        const int value = buf[i] < 0 ? -buf[i] : buf[i];
        if (value > peak) peak = value;
    }
    return peak;
}

static void test_lengths(void)
{
    // 按压:60 + 95 ms;松开:120 ms。
    assert(whale_tone_total_samples(WHALE_SND_PRESS, RATE) == (60 + 95) * RATE / 1000);
    assert(whale_tone_total_samples(WHALE_SND_RELEASE, RATE) == 120 * RATE / 1000);
    // 金币声:70 ms + 420 ms。
    assert(whale_tone_total_samples(WHALE_SND_COIN, RATE) == (70 + 420) * RATE / 1000);
    // 经验球:40 + 40 + 210 ms;钟声:70 + 300 ms。
    assert(whale_tone_total_samples(WHALE_SND_ORB, RATE) == (40 + 40 + 210) * RATE / 1000);
    assert(whale_tone_total_samples(WHALE_SND_BELL, RATE) == (70 + 300) * RATE / 1000);
    // 预警:110 + 60 间隙 + 110 ms。
    assert(whale_tone_total_samples(WHALE_SND_WARN, RATE) == (110 + 60 + 110) * RATE / 1000);
    // 连接音:四声 + 三个间隙。
    assert(whale_tone_total_samples(WHALE_SND_CONNECT, RATE) ==
           (95 + 15 + 95 + 15 + 95 + 15 + 210) * RATE / 1000);
    assert(whale_tone_total_samples(WHALE_SND_CLICK, RATE) == 45 * RATE / 1000);
    assert(whale_tone_total_samples(WHALE_SND_ERROR, RATE) == (130 + 30 + 190) * RATE / 1000);
    assert(whale_tone_total_samples(WHALE_SND_NONE, RATE) == 0);
}

// 每个音效都要能取到序列、有声、收尾归零且不溢出 —— 新增音效时这条会兜住漏配。
static void test_all_sounds_renderable(void)
{
    static const whale_sound_t ALL[] = {
        WHALE_SND_PRESS, WHALE_SND_RELEASE, WHALE_SND_COIN, WHALE_SND_ORB,
        WHALE_SND_BELL,  WHALE_SND_WARN,    WHALE_SND_CONNECT, WHALE_SND_CLICK,
        WHALE_SND_ERROR,
    };

    for (size_t i = 0; i < sizeof(ALL) / sizeof(ALL[0]); i++) {
        size_t count = 0;
        const whale_tone_step_t *steps = whale_tone_steps(ALL[i], &count);
        assert(steps != NULL && count > 0);

        const size_t total = whale_tone_total_samples(ALL[i], RATE);
        assert(total > 0);

        int16_t *buf = calloc(total, sizeof(int16_t));
        assert(buf != NULL);
        assert(whale_tone_render(steps, count, RATE, buf, total) == total);
        assert(abs_max(buf, total) > 3000);          // 有可闻响度
        assert(abs_max(buf, total) <= 32767);        // 不溢出
        assert(buf[total - 1] == 0);                 // 收尾静音,不爆音
        free(buf);
    }
}

static void test_coin_envelope(void)
{
    const size_t total = whale_tone_total_samples(WHALE_SND_COIN, RATE);
    int16_t *buf = calloc(total, sizeof(int16_t));
    assert(buf != NULL);

    size_t count = 0;
    const whale_tone_step_t *steps = whale_tone_steps(WHALE_SND_COIN, &count);
    assert(steps != NULL && count == 2);
    assert(whale_tone_render(steps, count, RATE, buf, total) == total);

    // 起振后要达到可闻的响度(85% 幅度 ≈ 27852)。
    assert(abs_max(buf, total) > 20000);
    // 不超过该段设定的幅度上限。
    assert(abs_max(buf, total) <= 27852);
    // 收尾必须是静音:包络按 (1-t)^2 收敛,末尾采样为 0,不会有爆音。
    assert(buf[total - 1] == 0);

    // 第二段(E6 延音)应当明显衰减。
    const size_t tail_start = 70 * RATE / 1000;
    assert(abs_max(buf + tail_start + 400, 200) < abs_max(buf + tail_start, 200));

    free(buf);
}

static void test_connect_gaps(void)
{
    const size_t total = whale_tone_total_samples(WHALE_SND_CONNECT, RATE);
    int16_t *buf = calloc(total, sizeof(int16_t));
    assert(buf != NULL);

    size_t count = 0;
    const whale_tone_step_t *steps = whale_tone_steps(WHALE_SND_CONNECT, &count);
    assert(whale_tone_render(steps, count, RATE, buf, total) == total);

    // 第一声结束后的间隙必须是全零。
    const size_t gap_start = 95 * RATE / 1000;
    const size_t gap_samples = 15 * RATE / 1000;
    for (size_t i = 0; i < gap_samples; i++) assert(buf[gap_start + i] == 0);

    // 最后一声音量不能溢出。
    assert(abs_max(buf, total) <= 32767);
    free(buf);
}

static void test_deterministic_and_bounded(void)
{
    const size_t total = whale_tone_total_samples(WHALE_SND_ERROR, RATE);
    int16_t *first = calloc(total, sizeof(int16_t));
    int16_t *second = calloc(total, sizeof(int16_t));
    assert(first != NULL && second != NULL);

    size_t count = 0;
    const whale_tone_step_t *steps = whale_tone_steps(WHALE_SND_ERROR, &count);
    assert(whale_tone_render(steps, count, RATE, first, total) == total);
    assert(whale_tone_render(steps, count, RATE, second, total) == total);
    assert(memcmp(first, second, total * sizeof(int16_t)) == 0);

    // 输出缓冲区偏小时只截断,不越界写。
    int16_t small[8];
    assert(whale_tone_render(steps, count, RATE, small, 8) == 8);
    assert(memcmp(small, first, sizeof(small)) == 0);

    free(first);
    free(second);
}

static void test_chunked_render_matches_whole(void)
{
    // 播放端按 256 采样一块地喂 I2S,分块结果必须与一次性渲染完全一致。
    const size_t total = whale_tone_total_samples(WHALE_SND_COIN, RATE);
    int16_t *whole = calloc(total, sizeof(int16_t));
    int16_t *chunked = calloc(total, sizeof(int16_t));
    int16_t block[256];
    assert(whole != NULL && chunked != NULL);

    size_t count = 0;
    const whale_tone_step_t *steps = whale_tone_steps(WHALE_SND_COIN, &count);
    assert(whale_tone_render(steps, count, RATE, whole, total) == total);

    size_t offset = 0;
    while (offset < total) {
        const size_t want = (total - offset < 256) ? (total - offset) : 256;
        const size_t got = whale_tone_render_offset(steps, count, RATE, offset, block, want);
        assert(got == want);
        memcpy(chunked + offset, block, want * sizeof(int16_t));
        offset += want;
    }
    assert(memcmp(whole, chunked, total * sizeof(int16_t)) == 0);

    // offset 超出总长后不再产出采样。
    assert(whale_tone_render_offset(steps, count, RATE, total + 10, block, 256) == 0);

    free(whole);
    free(chunked);
}

int main(void)
{
    test_lengths();
    test_all_sounds_renderable();
    test_coin_envelope();
    test_connect_gaps();
    test_deterministic_and_bounded();
    test_chunked_render_matches_whole();
    printf("test_whale_tone: PASS\n");
    return 0;
}
