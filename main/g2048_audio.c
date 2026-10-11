// main/g2048_audio.c —— 用方波合成 2048 的四种提示音。
//
// ES8311 + I2S 的写入是阻塞的,一次短音也要几十毫秒,因此只在 worker 任务里播放;
// 按键回调(运行在共享 esp_timer 任务上)只往队列里丢一个枚举立即返回。
#include "g2048_audio.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SAMPLE_RATE   16000
#define CHUNK_SAMPLES 256
#define QUEUE_DEPTH   4

static const char *TAG = "g2048_audio";

typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
} tone_step_t;

// 每种音效是一串短音。总时长控制在 250ms 以内,避免长音占住 I2S。
static const tone_step_t SOUNDS[][4] = {
    [G2048_SND_MOVE]     = { { 660, 60 }, { 0, 0 } },
    [G2048_SND_MERGE]    = { { 880, 70 }, { 1320, 80 }, { 0, 0 } },
    [G2048_SND_WIN]      = { { 784, 90 }, { 988, 90 }, { 1319, 140 }, { 0, 0 } },
    [G2048_SND_OVER]     = { { 440, 110 }, { 330, 110 }, { 247, 160 }, { 0, 0 } },
    [G2048_SND_BLOCKED]  = { { 196, 70 }, { 0, 0 } },
};
#define MAX_STEPS 4

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_cancel;

static void play_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    if (freq_hz == 0 || duration_ms == 0) return;

    int16_t buf[CHUNK_SAMPLES];
    const uint32_t period = SAMPLE_RATE / freq_hz;   // 一个周期的采样数
    if (period < 2) return;
    uint32_t phase = 0;
    uint32_t remaining = (uint32_t)SAMPLE_RATE * duration_ms / 1000u;

    while (remaining > 0 && !s_cancel) {
        const uint32_t n = remaining < CHUNK_SAMPLES ? remaining : CHUNK_SAMPLES;
        for (uint32_t i = 0; i < n; i++) {
            buf[i] = (phase < period / 2) ? 6000 : -6000;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "音频写入失败,跳过剩余音效");
            return;
        }
        remaining -= n;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    g2048_sound_t sound;
    while (!s_cancel) {
        if (xQueueReceive(s_queue, &sound, portMAX_DELAY) != pdTRUE) continue;
        if (s_cancel) break;
        if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
            ESP_LOGW(TAG, "音频格式设置失败,本次不发声");
            continue;
        }
        for (int i = 0; i < MAX_STEPS; i++) {
            const tone_step_t step = SOUNDS[sound][i];
            if (step.freq_hz == 0) break;
            play_tone(step.freq_hz, step.duration_ms);
            if (s_cancel) break;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

bool g2048_audio_start(void)
{
    if (s_task != NULL) return true;
    s_cancel = false;
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频 codec 初始化失败,游戏静音运行");
        return false;
    }
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(g2048_sound_t));
    if (s_queue == NULL) return false;
    bsp_audio_set_volume(70);
    if (xTaskCreate(audio_task, "g2048_audio", 3072, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }
    return true;
}

void g2048_audio_stop(void)
{
    if (s_task == NULL && s_queue == NULL) return;
    s_cancel = true;
    // 任务阻塞在队列上,给它一个空事件把它唤醒退出,避免永久等待。
    if (s_queue != NULL) {
        const g2048_sound_t dummy = G2048_SND_MOVE;
        (void)xQueueSend(s_queue, &dummy, 0);
    }
    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_cancel = false;
}

void g2048_audio_play(g2048_sound_t sound)
{
    if (s_queue == NULL || s_cancel) return;
    (void)xQueueSend(s_queue, &sound, 0);
}
