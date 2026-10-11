// main/whale_audio.c —— 把 whale_tone 合成的 PCM 送进 ES8311。
//
// bsp_audio_write 是阻塞的,一次短音也要几十毫秒,所以播放只在独立的 worker 任务里做;
// 按键回调(运行在共享 esp_timer 任务上)只往队列里丢一个枚举就返回。
#include "whale_audio.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CHUNK_SAMPLES 256
#define QUEUE_DEPTH   4
#define PLAY_VOLUME   70

static const char *TAG = "whale_audio";

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_stop;

// 整段音效按 256 采样一块地渲染并写出,避免在内存里铺开整条音轨。
static void play_sound(whale_sound_t sound)
{
    size_t step_count = 0;
    const whale_tone_step_t *steps = whale_tone_steps(sound, &step_count);
    if (steps == NULL) return;

    const size_t total = whale_tone_total_samples(sound, WHALE_TONE_SAMPLE_RATE);
    int16_t chunk[CHUNK_SAMPLES];

    for (size_t offset = 0; offset < total && !s_stop; offset += CHUNK_SAMPLES) {
        const size_t want = (total - offset < CHUNK_SAMPLES) ? (total - offset) : CHUNK_SAMPLES;
        const size_t got = whale_tone_render_offset(steps, step_count, WHALE_TONE_SAMPLE_RATE,
                                                    offset, chunk, want);
        if (got == 0) break;
        if (bsp_audio_write(chunk, got * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "音频写入失败,放弃剩余部分");
            return;
        }
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    whale_sound_t sound;

    while (!s_stop) {
        if (xQueueReceive(s_queue, &sound, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (s_stop) break;

        // 每次播放前对齐格式:格式变化会 close/open codec,这里保持单一格式故开销很小。
        if (bsp_audio_set_format(WHALE_TONE_SAMPLE_RATE, 16, 1) != ESP_OK) {
            ESP_LOGW(TAG, "音频格式设置失败,本次不发声");
            continue;
        }
        play_sound(sound);
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

bool whale_audio_start(void)
{
    if (s_task != NULL) return true;

    s_stop = false;
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "codec 初始化失败,静音运行");
        return false;
    }
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(whale_sound_t));
    if (s_queue == NULL) return false;

    bsp_audio_set_volume(PLAY_VOLUME);
    if (xTaskCreate(audio_task, "whale_audio", 3072, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }
    return true;
}

void whale_audio_stop(void)
{
    if (s_task == NULL && s_queue == NULL) return;

    s_stop = true;
    if (s_queue != NULL) {
        const whale_sound_t wake = WHALE_SND_NONE;   // 唤醒阻塞在队列上的任务
        (void)xQueueSend(s_queue, &wake, 0);
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
    s_stop = false;
}

void whale_audio_play(whale_sound_t sound)
{
    if (s_queue == NULL || s_stop) return;
    (void)xQueueSend(s_queue, &sound, 0);
}
