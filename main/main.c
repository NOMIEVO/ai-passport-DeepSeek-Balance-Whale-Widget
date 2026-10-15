// main/main.c —— 小鲸鱼计费名片的应用入口。
//
// 职责:BSP 初始化、三键输入分发、网络任务编排、空闲息屏/深睡。
// 界面在 whale_ui.c、金额与记账规则在 whale_model.c、联网在 whale_net.c、
// 配网网页在 whale_prov.c、持久化在 whale_store.c、声音在 whale_audio.c。
//
// 按键语义(本应用自定义,与基线 demo 无关)。交互一律以长按为主,与挂件一致:
// 挂件在手机端是「长按鲸鱼约 1.5 秒唤出菜单」,名片就把长按确定键做成唤出菜单。
//   挂件页   确定 短按   推进泡泡序列(余额 → 今日已用 → 峰谷 → 连接 → 收起)
//            上/下 短按   上一项 / 下一项(走到末项再按即收起)
//            确定 长按   唤出菜单(设置页)
//            上   长按   立即刷新余额
//            下   长按   试听当前消费音效
//   菜单页   上/下 短按   上下移动选中项
//            确定 短按   执行 / 切换选中项
//            确定 长按   返回挂件页
//            配网项 长按  清除已存凭据(危险操作,必须长按)
//   配网页   确定 短按   用已保存凭据重连一次
//            确定 长按   关闭热点并返回挂件页
//   任意键                点亮屏幕并刷新空闲计时
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "whale_audio.h"
#include "whale_form.h"
#include "whale_model.h"
#include "whale_net.h"
#include "whale_prov.h"
#include "whale_store.h"
#include "whale_ui.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "whale";

#define INPUT_QUEUE_DEPTH 8
#define CMD_QUEUE_DEPTH   8

// 熄屏时长由「熄屏时间」菜单项决定(main 层按 s_config.sleep_sec 换算),
// 这里只保留调暗到休眠的比例:半程调暗提示,到点入睡。
#define IDLE_DIM_DIVISOR    2
#define BATTERY_REFRESH_US  (10LL * 1000 * 1000)
#define KEY_GUARD_MAX_US    (10LL * 1000 * 1000)
#define CONNECT_TIMEOUT_MS  20000
#define TIME_SYNC_TIMEOUT_MS 8000

// 取数失败要连着失败这么多次,状态行才升级成「网络不可用 / 服务器繁忙」那一类严重态。
// 少于这个数一律停在 WHALE_ST_STALE("稍后重试")—— 偶发一次抖动不该让屏幕跳红字。
#define FAIL_REPORT_STREAK  2

// 失败后的退避重试间隔(秒),依次取用,到最后一项就停在那儿。
// 服务器繁忙时若仍按正常刷新间隔猛撞,只会让自己更难排上队。
static const uint32_t BACKOFF_STEPS_SEC[] = { 15, 30, 60, 120, 300 };

typedef enum {
    CMD_CONNECT = 0,      // 用当前配置连 Wi-Fi(成功后授时 + 取一次余额)
    CMD_REFRESH,          // 直接取一次余额(已连接时)
    CMD_CONFIG_SAVED,     // 网页刚保存了配置
    CMD_PROV_START,
    CMD_PROV_STOP,
    CMD_CLEAR_CREDS,
} app_cmd_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static whale_config_t s_config;
static whale_ledger_t s_ledger;
static whale_ui_state_t s_ui;
static whale_setting_t s_setting = WHALE_SET_PROVISION;

static key_guard_t s_key_guard;
static QueueHandle_t s_input_queue;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_net_task;
static volatile bool s_ready;
static volatile bool s_busy;         // 联网/配网进行中 → 不进入深睡
static volatile bool s_sleeping;
static int64_t s_last_activity_us;
static int64_t s_last_battery_us;
static int64_t s_last_refresh_us;
static bool s_dimmed;
static bool s_bubble_open = true;   // 泡泡是否展开(收起时只留小鲸鱼)
static bool s_budget_hit;           // 今日已用已触达预算(只提示一次)
static bool s_warn_hit;             // 余额已低于预警线(只提示一次)
static uint8_t s_fail_streak;       // 连续取数失败次数(成功即清零)

// 音效总开关的唯一闸口:所有发声都过这里,关掉后一律静音。
static void play_sound(whale_sound_t sound)
{
    if (!s_config.sound_all) return;
    whale_audio_play(sound);
}

// 按压音:短按与长按都算"按压",统一受「按压音效」开关管,
// 否则会出现"关了按压音,长按却还在响"这种说不通的行为。
// 同时触发一次 Q 弹 —— 参考挂件里按压音与 Q 弹是同一个动作的两面(只在挂件页弹,
// 别的页面没有舞台)。
static void press_cue(void)
{
    if (s_config.sound_press) play_sound(WHALE_SND_PRESS);
    if (whale_ui_current_page() == WHALE_PAGE_HOME) whale_ui_boing();
}

// ---- 挂件页状态组装 --------------------------------------------------------

static void refresh_time_fields(void)
{
    char day[WHALE_DAY_LEN];
    int weekday = 0;
    int minute_of_day = 0;
    if (whale_net_local_time(day, sizeof(day), &weekday, &minute_of_day)) {
        s_ui.tariff_known = true;
        s_ui.tariff = whale_tariff_at(weekday, minute_of_day);
        s_ui.minutes_to_change = whale_tariff_minutes_to_change(weekday, minute_of_day);
        // 夹一次范围:既挡住异常值,也让编译器能确认格式化后一定放得下。
        int hour = minute_of_day / 60;
        int minute = minute_of_day % 60;
        if (hour < 0) hour = 0;
        if (hour > 23) hour = 23;
        if (minute < 0) minute = 0;
        if (minute > 59) minute = 59;
        snprintf(s_ui.updated, sizeof(s_ui.updated), "%02d:%02d", hour, minute);
    } else {
        s_ui.tariff_known = false;
        snprintf(s_ui.updated, sizeof(s_ui.updated), "--:--");
    }
}

static void publish_ui(void)
{
    char ip[18] = "";
    whale_net_has_ip(ip, sizeof(ip));
    snprintf(s_ui.ip, sizeof(s_ui.ip), "%s", ip);
    s_ui.bubble_open = s_bubble_open;
    // 样式是枚举,但 NVS 里存的是 int:这里夹一次,脏值退回默认而不是渲染到越界分支。
    s_ui.peak_style = whale_peak_style_clamp(s_config.peak_style);
    refresh_time_fields();
    whale_ui_update(&s_ui);
}

static void push_cmd(app_cmd_t cmd)
{
    if (s_cmd_queue == NULL) return;
    (void)xQueueSend(s_cmd_queue, &cmd, 0);
}

// 网页表单保存后的回调(运行在 httpd 任务里):只更新内存里的配置并投递命令。
static void on_form_saved(const whale_config_t *saved, void *user);

static void sync_credential_preview(void)
{
    whale_text_ascii_preview(s_config.ssid, s_ui.ssid_preview, sizeof(s_ui.ssid_preview));
    if (s_config.api_key[0] != '\0') {
        const size_t len = strlen(s_config.api_key);
        snprintf(s_ui.key_hint, sizeof(s_ui.key_hint), len > 4 ? "…%s" : "set",
                 len > 4 ? s_config.api_key + len - 4 : "");
    } else {
        snprintf(s_ui.key_hint, sizeof(s_ui.key_hint), "none");
    }
    s_ui.refresh_sec = s_config.refresh_sec;
}

// ---- 余额刷新 --------------------------------------------------------------

static void on_balance_event(whale_ledger_event_t event, int64_t delta_cents)
{
    if (event == WHALE_LEDGER_SPEND) {
        // 消费:金币声 + 掉落动画。声音与界面各走各的通道,互不加锁。
        if (s_config.sound_spend) play_sound(WHALE_SND_COIN);
        whale_ui_coin_drop();
        ESP_LOGI(TAG, "检测到消费 %lld 分,今日累计 %lld 分",
                 (long long)-delta_cents, (long long)s_ledger.used_cents);
    } else if (event == WHALE_LEDGER_TOPUP) {
        ESP_LOGI(TAG, "检测到充值 %lld 分(不冲抵消费)", (long long)delta_cents);
    }
    whale_store_save_ledger(&s_ledger);
}

// 预算与预警:越过阈值时响一次,并占用状态行。
// 用 s_budget_hit / s_warn_hit 记"已经提示过",否则每次自动刷新都要重响一遍。
static void check_thresholds(void)
{
    const bool budget = whale_budget_exceeded(s_ledger.used_cents, s_config.budget_cents);
    const bool low = whale_balance_low(s_ui.balance_cents, s_config.warn_cents);

    if (budget && !s_budget_hit) {
        s_budget_hit = true;
        play_sound(WHALE_SND_WARN);
        ESP_LOGW(TAG, "今日已用 %lld 分,触达预算 %lld 分", (long long)s_ledger.used_cents,
                 (long long)s_config.budget_cents);
    }
    if (!budget) s_budget_hit = false;

    if (low && !s_warn_hit) {
        s_warn_hit = true;
        play_sound(WHALE_SND_WARN);
        ESP_LOGW(TAG, "余额 %lld 分,低于预警线 %lld 分", (long long)s_ui.balance_cents,
                 (long long)s_config.warn_cents);
    }
    if (!low) s_warn_hit = false;

    // 预警优先于"已连接":这两条才是用户需要立刻看见的。
    if (budget) {
        s_ui.status = WHALE_ST_BUDGET_HIT;
    } else if (low) {
        s_ui.status = WHALE_ST_BALANCE_LOW;
    }
}

// 失败登记:连续失败次数决定状态行怎么说。
//
// 前 FAIL_REPORT_STREAK-1 次只走到 WHALE_ST_STALE("稍后重试 · 上次成功时刻"),
// 余额继续显示上一次取到的值;到了阈值才升到 severe 那一档严重态。
// 返回 true 表示这次真的报错了(调用方据此决定要不要响错误音)。
static bool note_failure(whale_status_t severe)
{
    if (s_fail_streak < 255) s_fail_streak++;

    if (s_fail_streak >= FAIL_REPORT_STREAK) {
        s_ui.status = severe;
        ESP_LOGW(TAG, "取数连续失败 %u 次 → 状态升级", (unsigned)s_fail_streak);
        return true;
    }
    s_ui.status = WHALE_ST_STALE;
    return false;
}

// 按连续失败次数算下一次重试该等多久:失败越多间隔越长,但不超过最后一档。
static int64_t backoff_us(void)
{
    if (s_fail_streak == 0) return 0;
    const size_t idx = (s_fail_streak - 1) < (sizeof(BACKOFF_STEPS_SEC) / sizeof(BACKOFF_STEPS_SEC[0]))
                           ? (s_fail_streak - 1)
                           : (sizeof(BACKOFF_STEPS_SEC) / sizeof(BACKOFF_STEPS_SEC[0]) - 1);
    return (int64_t)BACKOFF_STEPS_SEC[idx] * 1000 * 1000;
}

static bool fetch_balance_once(bool manual)
{
    static bool stack_reported;
    char body[WHALE_BALANCE_BODY_MAX];
    whale_fetch_result_t result = { 0 };

    if (!whale_config_ready(&s_config)) {
        s_ui.status = WHALE_ST_UNCONFIGURED;
        publish_ui();
        return false;
    }
    if (whale_net_state() != WHALE_NET_CONNECTED) {
        s_ui.status = WHALE_ST_CONNECTING;
        publish_ui();
        return false;
    }

    s_ui.status = WHALE_ST_REFRESHING;
    publish_ui();

    const esp_err_t err = whale_net_fetch_balance(s_config.api_key, body, sizeof(body), &result);
    if (!stack_reported) {
        stack_reported = true;
        ESP_LOGI(TAG, "网络任务栈水位线: %lu 字节剩余",
                 (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    }
    // 分级的关键顺序:**先看服务端给没给状态码**。
    // whale_net_fetch_balance() 即使自己返回错误,也会把收到的 HTTP 状态码填进 result。
    // 对方明明回话了(401/429/5xx)却因为流式响应中途断流导致 err != ESP_OK,若按
    // "传输层失败"一律报"网络不可用",用户就会跑去重启路由器 —— 实际上本地网络好好的。
    const int status = result.http_status;

    if (status >= 400 && status < 500 && status != 429) {
        // 请求侧 4xx:Key 写错(401/403)、参数不对(400)、账户没钱(402)。
        // 这是永久性错误,重试多少次都一样,不参与"连续失败"计数,立刻明说。
        ESP_LOGW(TAG, "余额接口拒绝请求: HTTP %d", status);
        s_ui.status = WHALE_ST_AUTH_FAILED;
        publish_ui();
        if (manual) play_sound(WHALE_SND_ERROR);
        return false;
    }
    if (status == 429 || status >= 500) {
        // 服务器侧:429 限流或 5xx。这一侧什么都改不了,别让用户去查路由器。
        if (note_failure(WHALE_ST_SERVER_BUSY) && manual) play_sound(WHALE_SND_ERROR);
        publish_ui();
        return false;
    }

    if (err != ESP_OK) {
        // 走到这儿说明对方压根没给一个完整响应(DNS/TLS/连接失败、读超时)。
        // 用 Wi-Fi 是否还在来区分"本地链路断了"和"对方不理我":
        if (whale_net_state() != WHALE_NET_CONNECTED) {
            if (note_failure(WHALE_ST_NET_UNREACHABLE) && manual) play_sound(WHALE_SND_ERROR);
        } else {
            // 链路是通的,只是这趟请求没成(多半是对方超时/断流)—— 不冤枉本地网络。
            if (note_failure(WHALE_ST_API_TIMEOUT) && manual) play_sound(WHALE_SND_ERROR);
        }
        publish_ui();
        return false;
    }

    if (status != 200) {
        // 既不是成功也不是上面那几类(3xx 之类):拿不到余额正文,按"没同步上"处理。
        if (note_failure(WHALE_ST_API_TIMEOUT) && manual) play_sound(WHALE_SND_ERROR);
        publish_ui();
        return false;
    }

    whale_balance_t balance;
    if (!whale_balance_extract(body, &balance)) {
        // 拿到了响应但读不出余额:多半是返回体被截断或字段变了。
        if (note_failure(WHALE_ST_DATA_ERROR) && manual) play_sound(WHALE_SND_ERROR);
        publish_ui();
        return false;
    }

    // 记账:跨日/首次观测由模型自己判定。
    char day[WHALE_DAY_LEN] = "";
    whale_net_local_time(day, sizeof(day), NULL, NULL);

    int64_t delta = 0;
    const whale_ledger_event_t event = whale_ledger_observe(&s_ledger, day, balance.total_cents,
                                                            &delta);

    s_ui.has_balance = true;
    s_ui.balance_cents = balance.total_cents;
    s_ui.today_used_cents = s_ledger.used_cents;
    s_ui.last_delta_cents = delta;
    s_ui.status = WHALE_ST_ONLINE;
    // 这一轮通了:失败计数清零,并把眼下这个时刻记成"数据更新于"。
    // publish_ui() 会不停刷新 s_ui.updated(那是当前时钟),所以这里必须先拷一份快照。
    s_fail_streak = 0;
    snprintf(s_ui.updated_ok, sizeof(s_ui.updated_ok), "%s", s_ui.updated);

    on_balance_event(event, delta);
    check_thresholds();
    publish_ui();
    return true;
}

// ---- 联网编排 --------------------------------------------------------------

static void connect_and_refresh(void)
{
    if (!whale_config_ready(&s_config)) {
        s_ui.status = WHALE_ST_UNCONFIGURED;
        publish_ui();
        return;
    }

    s_ui.status = WHALE_ST_CONNECTING;
    publish_ui();

    if (whale_net_sta_connect(s_config.ssid, s_config.password) != ESP_OK) {
        note_failure(WHALE_ST_NET_UNREACHABLE);
        publish_ui();
        return;
    }
    if (!whale_net_wait_connected(CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Wi-Fi 连接超时: %s", esp_err_to_name(whale_net_last_error()));
        note_failure(WHALE_ST_NET_UNREACHABLE);
        publish_ui();
        return;
    }

    // 连上了就响一声 —— 这是"名片已经上线"的听觉确认。
    if (s_config.sound_connect) play_sound(WHALE_SND_CONNECT);
    whale_net_sync_time(TIME_SYNC_TIMEOUT_MS);

    s_ui.status = WHALE_ST_ONLINE;
    publish_ui();
    fetch_balance_once(false);
}

static void net_task(void *arg)
{
    (void)arg;
    app_cmd_t cmd;

    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_busy = true;
            switch (cmd) {
            case CMD_CONNECT:
                connect_and_refresh();
                break;
            case CMD_REFRESH:
                if (whale_net_state() != WHALE_NET_CONNECTED) {
                    connect_and_refresh();
                } else {
                    fetch_balance_once(true);
                }
                break;
            case CMD_CONFIG_SAVED:
                // 走完一次表单多半换了 SSID / Key,旧的失败计数与"上次成功时刻"作废。
                s_fail_streak = 0;
                s_ui.updated_ok[0] = '\0';
                whale_ui_provision_update(WHALE_PV_CONNECTING, NULL, NULL, NULL);
                connect_and_refresh();
                whale_ui_provision_update(whale_net_state() == WHALE_NET_CONNECTED
                                              ? WHALE_PV_ONLINE : WHALE_PV_FAILED,
                                          NULL, NULL, s_ui.ip);
                break;
            case CMD_PROV_START:
                if (whale_prov_start(&s_config, on_form_saved, NULL) == ESP_OK) {
                    whale_ui_provision_update(WHALE_PV_WAITING, whale_prov_ssid(),
                                              whale_prov_password(), NULL);
                    whale_ui_show_page(WHALE_PAGE_PROVISION);
                }
                break;
            case CMD_PROV_STOP:
                whale_prov_stop();
                whale_ui_show_page(WHALE_PAGE_HOME);
                break;
            case CMD_CLEAR_CREDS:
                whale_store_clear_credentials();
                s_config.ssid[0] = '\0';
                s_config.password[0] = '\0';
                s_config.api_key[0] = '\0';
                whale_net_sta_disconnect();
                s_ui.ssid_preview[0] = '\0';
                snprintf(s_ui.key_hint, sizeof(s_ui.key_hint), "none");
                s_ui.has_balance = false;
                s_ui.status = WHALE_ST_UNCONFIGURED;
                // 凭据都没了,之前的失败计数和"上次成功时刻"也不再有意义。
                s_fail_streak = 0;
                s_ui.updated_ok[0] = '\0';
                publish_ui();
                break;
            default:
                break;
            }
            s_busy = false;
        }

        // 自动取数与失败重试:配网进行中一律不插手,把资源让给 SoftAP。
        if (!whale_prov_running() && whale_config_ready(&s_config)) {
            const bool connected = whale_net_state() == WHALE_NET_CONNECTED;
            const int64_t now = esp_timer_get_time();
            // 有失败悬着就按退避表走(失败越多等得越久);一路顺利才用设置里的刷新间隔。
            const int64_t interval = (s_fail_streak > 0) ? backoff_us()
                                                        : (int64_t)s_config.refresh_sec * 1000 * 1000;
            if (now - s_last_refresh_us >= interval) {
                s_last_refresh_us = now;
                s_busy = true;
                if (!connected) {
                    // Wi-Fi 掉线也必须肯自己重连。以前的写法要求"已连接才轮询",
                    // 于是掉线之后没有任何一条路径能把它拉回来 —— 状态行会永远
                    // 停在"网络不可用",直到用户按键或重启。
                    ESP_LOGW(TAG, "Wi-Fi 已掉线,尝试重新连接");
                    connect_and_refresh();
                } else {
                    fetch_balance_once(false);
                }
                s_busy = false;
            }
        }
    }
}

// ---- 配网表单回调 ----------------------------------------------------------

static void on_form_saved(const whale_config_t *saved, void *user)
{
    (void)user;
    if (saved == NULL) return;
    s_config = *saved;
    sync_credential_preview();
    push_cmd(CMD_CONFIG_SAVED);
}

// ---- 设置页动作 ------------------------------------------------------------

static void refresh_settings_view(void)
{
    whale_ui_settings_update(s_setting, s_config.refresh_sec, s_config.budget_cents,
                             s_config.warn_cents, whale_peak_style_clamp(s_config.peak_style),
                             whale_sleep_clamp((uint32_t)s_config.sleep_sec),
                             s_config.sound_all,
                             s_config.sound_press, s_config.sound_spend,
                             s_config.sound_connect);
}

static void settings_activate(void)
{
    switch (s_setting) {
    case WHALE_SET_PROVISION:
        play_sound(WHALE_SND_CLICK);
        push_cmd(CMD_PROV_START);
        break;
    case WHALE_SET_REFRESH:
        s_config.refresh_sec = whale_refresh_next(s_config.refresh_sec);
        whale_store_save_config(&s_config);
        s_ui.refresh_sec = s_config.refresh_sec;
        play_sound(WHALE_SND_CLICK);
        break;
    case WHALE_SET_BUDGET:
        s_config.budget_cents = whale_budget_next(s_config.budget_cents);
        whale_store_save_config(&s_config);
        s_budget_hit = false;      // 换了阈值就重新计一次"是否已经提示过"
        play_sound(WHALE_SND_CLICK);
        break;
    case WHALE_SET_WARN:
        s_config.warn_cents = whale_warn_next(s_config.warn_cents);
        whale_store_save_config(&s_config);
        s_warn_hit = false;
        play_sound(WHALE_SND_CLICK);
        break;
    case WHALE_SET_PEAK_STYLE:
        s_config.peak_style = whale_peak_style_next(whale_peak_style_clamp(s_config.peak_style));
        whale_store_save_config(&s_config);
        s_ui.peak_style = whale_peak_style_clamp(s_config.peak_style);
        play_sound(WHALE_SND_CLICK);
        break;
    case WHALE_SET_SOUND_ALL:
        s_config.sound_all = !s_config.sound_all;
        whale_store_save_config(&s_config);
        // 这里刻意不走 play_sound():总开关刚打开时应当立刻响一声确认。
        if (s_config.sound_all) whale_audio_play(WHALE_SND_CLICK);
        break;
    case WHALE_SET_SOUND_PRESS:
        s_config.sound_press = !s_config.sound_press;
        whale_store_save_config(&s_config);
        play_sound(s_config.sound_press ? WHALE_SND_PRESS : WHALE_SND_CLICK);
        break;
    case WHALE_SET_SOUND_SPEND:
        s_config.sound_spend = !s_config.sound_spend;
        whale_store_save_config(&s_config);
        play_sound(s_config.sound_spend ? WHALE_SND_COIN : WHALE_SND_CLICK);
        break;
    case WHALE_SET_SOUND_CONNECT:
        s_config.sound_connect = !s_config.sound_connect;
        whale_store_save_config(&s_config);
        play_sound(s_config.sound_connect ? WHALE_SND_CONNECT : WHALE_SND_CLICK);
        break;
    case WHALE_SET_SLEEP:
        s_config.sleep_sec =
            (int64_t)whale_sleep_next(whale_sleep_clamp((uint32_t)s_config.sleep_sec));
        whale_store_save_config(&s_config);
        // 改档位也走一次按键路径,空闲计时已经被 handle_input 清零了:
        // 否则从 100 秒切到 300 秒的瞬间可能已经"超时",当场睡过去。
        play_sound(WHALE_SND_CLICK);
        break;
    case WHALE_SET_LEDGER:
        // 只清当日累计,不动 Wi-Fi 与 API Key。
        s_ledger.used_cents = 0;
        s_ledger.topup_cents = 0;
        s_ledger.last_cents = s_ui.balance_cents;
        s_ui.today_used_cents = 0;
        s_budget_hit = false;
        whale_store_save_ledger(&s_ledger);
        play_sound(WHALE_SND_CLICK);
        break;
    case WHALE_SET_BACK:
        play_sound(WHALE_SND_CLICK);
        whale_ui_show_page(WHALE_PAGE_HOME);
        break;
    default:
        break;
    }
    refresh_settings_view();
}

// ---- 空闲与休眠 ------------------------------------------------------------

static void mark_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_dimmed) {
        bsp_display_backlight(100);
        s_dimmed = false;
    }
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "空闲超时,准备进入 deep sleep");
    whale_ui_sleep_notice();

    whale_audio_stop();     // 音频任务持有 I2S,必须先停
    s_ready = false;

    const int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGW(TAG, "按键被按住(%d mV),本次不入睡", mv);
        s_ready = true;
        s_last_activity_us = esp_timer_get_time();
        publish_ui();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(400));

    int level = 0;
    if (bsp_button_prepare_deep_sleep(&level) != ESP_OK) {
        ESP_LOGE(TAG, "按键交回失败,重启恢复外设");
        esp_restart();
    }
    // 第一个参数是位掩码,不是引脚号。
    if (esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO,
                                          ESP_GPIO_WAKEUP_GPIO_LOW) != ESP_OK) {
        ESP_LOGW(TAG, "按键唤醒源武装失败,重启恢复外设");
        esp_restart();
    }

    // Wi-Fi 与 NTP 先收尾,再关其它外设。
    whale_net_prepare_deep_sleep();
    if (bsp_battery_sleep() != ESP_OK) ESP_LOGW(TAG, "电量计 suspend 未确认");
    if (bsp_audio_sleep() != ESP_OK) ESP_LOGW(TAG, "codec suspend 未确认");
    if (bsp_audio_prepare_deep_sleep() != ESP_OK) ESP_LOGW(TAG, "I2S 引脚释放失败");
    if (bsp_i2c_prepare_deep_sleep() != ESP_OK) ESP_LOGW(TAG, "I2C 引脚释放失败");

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "无法停住 LVGL 刷屏,重启恢复外设");
        esp_restart();
    }
    if (bsp_display_prepare_deep_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "屏幕 suspend 未确认(仍会入睡)");
    }
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回,重启");
    esp_restart();
}

static void service_idle(void)
{
    if (s_sleeping) return;
    const int64_t now = esp_timer_get_time();

    if (now - s_last_battery_us >= BATTERY_REFRESH_US) {
        s_last_battery_us = now;
        s_ui.battery = bsp_battery_soc();
        whale_ui_battery_refresh();
    }

    // 熄屏设为「关闭」时既不调暗也不休眠:屏幕一直亮着,等用户自己按键。
    const uint32_t sleep_sec = whale_sleep_clamp((uint32_t)s_config.sleep_sec);
    if (sleep_sec == WHALE_SLEEP_OFF) return;

    const int64_t idle = now - s_last_activity_us;
    // 先把背光压到最低,再入睡 —— 给人一个"快要睡了"的提示。
    if (!s_dimmed && idle >= (int64_t)sleep_sec * 1000 * 1000 / IDLE_DIM_DIVISOR) {
        bsp_display_backlight(8);
        s_dimmed = true;
    }
    // 配网/联网进行中不睡:用户正在读屏幕上的热点信息,或正在等余额。
    if (idle >= (int64_t)sleep_sec * 1000 * 1000 && !s_busy && !whale_prov_running()) {
        s_sleeping = true;
        enter_deep_sleep();
    }
}

// ---- 输入 ------------------------------------------------------------------

// 泡泡序列推进:走到最后一项后再按一次就收起泡泡(挂件的"最后一项再点收起")。
// 收起状态下再按一次 → 从第一项重新开始。
static void bubble_advance(void)
{
    if (!s_bubble_open) {
        s_bubble_open = true;
        s_ui.view = 0;
    } else if (s_ui.view >= WHALE_VIEW_COUNT - 1) {
        s_bubble_open = false;
    } else {
        s_ui.view = (s_ui.view + 1) % WHALE_VIEW_COUNT;
    }
    s_ui.bubble_open = s_bubble_open;
}

static void bubble_step(int direction)
{
    if (!s_bubble_open) {
        s_bubble_open = true;
        s_ui.view = 0;
    } else {
        s_ui.view = (s_ui.view + direction + WHALE_VIEW_COUNT) % WHALE_VIEW_COUNT;
    }
    s_ui.bubble_open = s_bubble_open;
}

static void handle_input(const input_event_t *input)
{
    mark_activity();
    if (input->event != BSP_BTN_CLICK && input->event != BSP_BTN_LONG) return;

    const whale_page_t page = whale_ui_current_page();

    // 长按是主交互通道:唤菜单、返回、刷新、试听、清除凭据都由长按承担。
    if (input->event == BSP_BTN_LONG) {
        if (page == WHALE_PAGE_HOME) {
            if (input->btn == BSP_BTN_OK) {
                press_cue();
                refresh_settings_view();
                whale_ui_show_page(WHALE_PAGE_SETTINGS);
            } else if (input->btn == BSP_BTN_UP) {
                press_cue();
                push_cmd(CMD_REFRESH);              // 长按上 = 立刻取一次余额
            } else {
                press_cue();
                // 长按下 = 试听消费音效(挂件每个音效行都带「试听」)
                play_sound(s_config.sound_spend ? WHALE_SND_COIN : WHALE_SND_BELL);
                whale_ui_coin_drop();
            }
            return;
        }

        if (page == WHALE_PAGE_SETTINGS) {
            if (input->btn == BSP_BTN_OK) {
                press_cue();
                whale_ui_show_page(WHALE_PAGE_HOME);
            } else if (input->btn == BSP_BTN_UP && s_setting == WHALE_SET_PROVISION) {
                // 危险操作必须长按:清掉已存的 Wi-Fi 与 API Key。
                press_cue();
                push_cmd(CMD_CLEAR_CREDS);
            } else {
                press_cue();
            }
            return;
        }

        if (input->btn == BSP_BTN_OK) {
            press_cue();
            push_cmd(CMD_PROV_STOP);
        }
        return;
    }

    if (page == WHALE_PAGE_HOME) {
        // 挂件页短按 = 按压鲸鱼,先给「小黄鸭·按下」再推进泡泡序列。
        press_cue();
        if (input->btn == BSP_BTN_OK) {
            bubble_advance();
        } else if (input->btn == BSP_BTN_UP) {
            bubble_step(-1);
        } else {
            bubble_step(1);
        }
        publish_ui();
        return;
    }

    if (page == WHALE_PAGE_SETTINGS) {
        if (input->btn == BSP_BTN_OK) {
            settings_activate();       // 各菜单项自己决定响什么音
            return;
        }
        play_sound(WHALE_SND_CLICK);
        if (input->btn == BSP_BTN_UP) {
            s_setting = (whale_setting_t)((s_setting + WHALE_SET_COUNT - 1) % WHALE_SET_COUNT);
        } else {
            s_setting = (whale_setting_t)((s_setting + 1) % WHALE_SET_COUNT);
        }
        refresh_settings_view();
        return;
    }

    // 配网页:短按确定 = 用已保存凭据再连一次。
    play_sound(WHALE_SND_CLICK);
    if (input->btn == BSP_BTN_OK) push_cmd(CMD_CONNECT);
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle_input(&input);
        }
        service_idle();
    }
}

// 按键回调运行在共享 esp_timer 任务上:只入队。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_ready || s_input_queue == NULL) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

// ---- 启动 ------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy AI Passport · 小鲸鱼计费名片 启动");

    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法运行。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (whale_net_init() != ESP_OK) {
        ESP_LOGE(TAG, "网络栈初始化失败,仅离线显示");
    }

    whale_config_init(&s_config);
    if (!whale_store_load_config(&s_config)) {
        ESP_LOGI(TAG, "没有已保存的配置,等待配网");
    } else {
        ESP_LOGI(TAG, "读到配置:SSID=%s 刷新=%lus Key=%s", s_config.ssid,
                 (unsigned long)s_config.refresh_sec,
                 s_config.api_key[0] != '\0' ? "已设置" : "空");
    }
    if (!whale_store_load_ledger(&s_ledger)) {
        whale_ledger_reset(&s_ledger, "");
        ESP_LOGI(TAG, "没有账本,从本次观测开始记账");
    } else {
        ESP_LOGI(TAG, "读到账本:%s 已用 %lld 分", s_ledger.day, (long long)s_ledger.used_cents);
    }

    // 界面状态初值。
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.battery = -1;
    s_ui.status = whale_config_ready(&s_config) ? WHALE_ST_CONNECTING : WHALE_ST_UNCONFIGURED;
    s_ui.today_used_cents = s_ledger.used_cents;
    s_ui.refresh_sec = s_config.refresh_sec;
    sync_credential_preview();
    snprintf(s_ui.updated, sizeof(s_ui.updated), "--:--");

    if (!whale_ui_init()) {
        ESP_LOGE(TAG, "界面创建失败");
        return;
    }
    publish_ui();

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(app_cmd_t));
    if (s_input_queue == NULL || s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "whale_input", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        return;
    }
    // HTTPS(mbedTLS + 证书包)握手很吃栈:这个任务按 12 KB 开,并在首次请求后
    // 打一次水位线,便于真机核对余量。
    if (xTaskCreate(net_task, "whale_net", 12288, NULL, 5, &s_net_task) != pdPASS) {
        ESP_LOGE(TAG, "网络任务创建失败");
        return;
    }

    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,右上角显示 --");
    }
    s_ui.battery = bsp_battery_soc();
    whale_ui_battery_refresh();

    if (!whale_audio_start()) ESP_LOGW(TAG, "音效不可用,静音运行");

    const int64_t now = esp_timer_get_time();
    s_last_activity_us = now;
    s_last_battery_us = now;
    s_last_refresh_us = now;
    s_ready = true;

    // 有配置就自动连一次;没配置停在"未配网",等用户长按进设置。
    if (whale_config_ready(&s_config)) {
        push_cmd(CMD_CONNECT);
    } else {
        ESP_LOGI(TAG, "等待配网:长按确定键进设置 → 配网与 API Key");
    }
}
