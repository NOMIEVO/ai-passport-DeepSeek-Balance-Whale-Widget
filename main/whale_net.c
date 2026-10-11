// main/whale_net.c —— Wi-Fi STA / SoftAP 切换、SNTP 授时、HTTPS 取余额。
//
// 所有会阻塞的调用(等待连接、等 NTP、HTTPS 往返)都只出现在应用 worker 任务里;
// 事件回调只改状态,不做事。
#include "whale_net.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "whale_net";
static const char *BALANCE_URL = "https://api.deepseek.com/user/balance";

#define CONNECT_ATTEMPTS  3
#define FETCH_TIMEOUT_MS  15000

static bool s_initialized;
static bool s_started;
static bool s_ap_active;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile whale_net_state_t s_state = WHALE_NET_OFF;
static volatile esp_err_t s_last_error = ESP_OK;
static volatile uint8_t s_attempts;
static volatile bool s_want_connection;
static char s_ip[16];

// ---- 事件 ------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    if (id == WIFI_EVENT_STA_START) {
        s_attempts = 0;
        if (s_want_connection) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_want_connection) {
            s_state = WHALE_NET_OFF;
            return;
        }
        if (s_attempts < CONNECT_ATTEMPTS) {
            s_attempts++;
            ESP_LOGW(TAG, "Wi-Fi 断开,第 %d 次重连", s_attempts);
            esp_wifi_connect();
            return;
        }
        s_state = WHALE_NET_FAILED;
        s_last_error = ESP_ERR_WIFI_NOT_CONNECT;
        ESP_LOGE(TAG, "Wi-Fi 连接失败,已重试 %d 次", CONNECT_ATTEMPTS);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_attempts = 0;
    s_last_error = ESP_OK;
    s_state = WHALE_NET_CONNECTED;
    ESP_LOGI(TAG, "已连接 Wi-Fi,IP=%s", s_ip);
}

// ---- 初始化 ----------------------------------------------------------------

esp_err_t whale_net_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;

    if (s_sta_netif == NULL) s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) return ESP_ERR_NO_MEM;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    // 凭据由 whale_store 自己存,不让 esp_wifi 再往 NVS 里写一份。
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi 就绪(STA)");
    return ESP_OK;
}

static esp_err_t ensure_started(void)
{
    if (!s_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_started = true;
    }
    return ESP_OK;
}

esp_err_t whale_net_sta_connect(const char *ssid, const char *password)
{
    if (!s_initialized || ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    wifi_config_t cfg = { 0 };
    const size_t ssid_len = strlen(ssid);
    memcpy(cfg.sta.ssid, ssid, ssid_len < sizeof(cfg.sta.ssid) ? ssid_len : sizeof(cfg.sta.ssid));
    if (password != NULL) {
        const size_t pass_len = strlen(password);
        memcpy(cfg.sta.password, password,
               pass_len < sizeof(cfg.sta.password) ? pass_len : sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = (password != NULL && password[0] != '\0')
                                     ? WIFI_AUTH_WPA2_PSK
                                     : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;
    err = ensure_started();
    if (err != ESP_OK) return err;

    s_want_connection = true;
    s_attempts = 0;
    s_state = WHALE_NET_CONNECTING;
    s_last_error = ESP_OK;
    s_ip[0] = '\0';

    // 已经连上的先断开,否则不会重新走一遍握手。
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_state = WHALE_NET_FAILED;
        s_last_error = err;
    }
    return err;
}

void whale_net_sta_disconnect(void)
{
    s_want_connection = false;
    if (s_initialized) esp_wifi_disconnect();
    s_state = WHALE_NET_OFF;
    s_ip[0] = '\0';
}

whale_net_state_t whale_net_state(void)
{
    return s_state;
}

esp_err_t whale_net_last_error(void)
{
    return s_last_error;
}

bool whale_net_has_ip(char *ip_out, size_t ip_size)
{
    if (s_state != WHALE_NET_CONNECTED) return false;
    if (ip_out != NULL && ip_size > 0) snprintf(ip_out, ip_size, "%s", s_ip);
    return true;
}

bool whale_net_wait_connected(uint32_t timeout_ms)
{
    const uint32_t step = 100;
    for (uint32_t waited = 0; waited < timeout_ms; waited += step) {
        if (s_state == WHALE_NET_CONNECTED) return true;
        if (s_state == WHALE_NET_FAILED) return false;
        vTaskDelay(pdMS_TO_TICKS(step));
    }
    return s_state == WHALE_NET_CONNECTED;
}

// ---- SoftAP(配网用) --------------------------------------------------------

esp_err_t whale_net_ap_start(const char *ssid, const char *password)
{
    if (!s_initialized || ssid == NULL) return ESP_ERR_INVALID_ARG;

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) return ESP_ERR_NO_MEM;
    }

    wifi_config_t cfg = { 0 };
    const size_t ssid_len = strlen(ssid);
    memcpy(cfg.ap.ssid, ssid, ssid_len < sizeof(cfg.ap.ssid) ? ssid_len : sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = (uint8_t)(ssid_len < sizeof(cfg.ap.ssid) ? ssid_len : sizeof(cfg.ap.ssid));
    cfg.ap.max_connection = 4;
    if (password != NULL && password[0] != '\0') {
        const size_t pass_len = strlen(password);
        memcpy(cfg.ap.password, password,
               pass_len < sizeof(cfg.ap.password) ? pass_len : sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    // 换模式必须先停:运行中改模式会被驱动拒绝。
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) return err;
    err = ensure_started();
    if (err != ESP_OK) return err;

    s_ap_active = true;
    ESP_LOGI(TAG, "SoftAP 已开启: %s", ssid);
    return ESP_OK;
}

esp_err_t whale_net_ap_stop(void)
{
    if (!s_initialized || !s_ap_active) return ESP_OK;

    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = ensure_started();
    s_ap_active = false;
    return err;
}

bool whale_net_ap_clients(void)
{
    if (!s_ap_active || s_ap_netif == NULL) return false;
    wifi_sta_list_t stations = { 0 };
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return false;
    return stations.num > 0;
}

void whale_net_prepare_deep_sleep(void)
{
    if (!s_initialized) return;
    s_want_connection = false;
    esp_wifi_disconnect();
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }
    esp_netif_sntp_deinit();
    s_state = WHALE_NET_OFF;
}

// ---- 时间 ------------------------------------------------------------------

bool whale_net_sync_time(uint32_t timeout_ms)
{
    // 北京时间:CST-8 就是 UTC+8(POSIX 偏移方向相反)。
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.wait_for_sync = true;
    config.renew_servers_after_new_IP = true;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) return false;

    const bool synced = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
    if (!synced) ESP_LOGW(TAG, "NTP 同步超时(峰谷与跨日判定将降级)");
    return synced;
}

bool whale_net_local_time(char *day, size_t day_size, int *weekday, int *minute_of_day)
{
    time_t now = 0;
    time(&now);
    if (now < 1700000000) return false;   // 1970 附近 → 时间还没同步出来

    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (day != NULL && day_size >= 11) {
        snprintf(day, day_size, "%04d-%02d-%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                 tm_now.tm_mday);
    }
    if (weekday != NULL) *weekday = tm_now.tm_wday;
    if (minute_of_day != NULL) *minute_of_day = tm_now.tm_hour * 60 + tm_now.tm_min;
    return true;
}

// ---- HTTPS -----------------------------------------------------------------

typedef struct {
    char *body;
    size_t capacity;
    size_t length;
} fetch_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    fetch_ctx_t *ctx = (fetch_ctx_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx == NULL || ctx->body == NULL || evt->data_len <= 0) break;
        {
            const size_t room = ctx->capacity - ctx->length - 1;
            const size_t take = ((size_t)evt->data_len < room) ? (size_t)evt->data_len : room;
            if (take > 0) {
                memcpy(ctx->body + ctx->length, evt->data, take);
                ctx->length += take;
                ctx->body[ctx->length] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t whale_net_fetch_balance(const char *api_key, char *body, size_t body_size,
                                  whale_fetch_result_t *result)
{
    if (api_key == NULL || api_key[0] == '\0' || body == NULL || body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    fetch_ctx_t ctx = { .body = body, .capacity = body_size, .length = 0 };
    body[0] = '\0';

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %.120s", api_key);

    esp_http_client_config_t config = {
        .url = BALANCE_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = on_http_event,
        .user_data = &ctx,
        .timeout_ms = FETCH_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);

    if (result != NULL) {
        result->http_status = status;
        result->err = err;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "余额请求失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "余额请求完成: HTTP %d, %u 字节", status, (unsigned)ctx.length);
    return ESP_OK;
}
