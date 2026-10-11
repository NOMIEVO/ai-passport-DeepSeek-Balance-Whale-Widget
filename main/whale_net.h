// main/whale_net.h —— Wi-Fi STA 连接、时间同步与 HTTPS 余额查询。
//
// 所有可能阻塞的操作(连接、NTP、HTTPS)都只在应用自己的 worker 任务里调用;
// 按键回调与 LVGL 任务只读取这里的状态。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WHALE_BALANCE_BODY_MAX 768   // /user/balance 的响应远小于此

typedef enum {
    WHALE_NET_OFF = 0,     // Wi-Fi 未启动或未配置
    WHALE_NET_CONNECTING,  // 正在连接已保存的 Wi-Fi
    WHALE_NET_CONNECTED,   // 已拿到 IP
    WHALE_NET_FAILED,      // 连接失败(错误码见 whale_net_last_error)
} whale_net_state_t;

typedef struct {
    int http_status;       // 200 / 401 / ...
    esp_err_t err;         // 传输层错误,ESP_OK 表示请求走到 HTTP 层
} whale_fetch_result_t;

// 初始化 NVS / esp_netif / 默认事件循环 / Wi-Fi(STA)。成功调用可重复。
esp_err_t whale_net_init(void);

// 用给定的 SSID/口令发起连接(非阻塞)。口令可为 NULL 或空串(开放网络)。
esp_err_t whale_net_sta_connect(const char *ssid, const char *password);
void whale_net_sta_disconnect(void);

// 配网页用的 SoftAP:开热点时切到 APSTA(STA 依然可用),关热点时回到纯 STA。
// 由 whale_prov 调用,不直接暴露给 UI。
esp_err_t whale_net_ap_start(const char *ssid, const char *password);
esp_err_t whale_net_ap_stop(void);
bool whale_net_ap_clients(void);

// deep sleep 前收尾:断链并停掉 Wi-Fi,避免带着 radio 入睡。
void whale_net_prepare_deep_sleep(void);

whale_net_state_t whale_net_state(void);
esp_err_t whale_net_last_error(void);
bool whale_net_has_ip(char *ip_out, size_t ip_size);

// 阻塞等待连接结果,只在 worker 任务里调用。
bool whale_net_wait_connected(uint32_t timeout_ms);

// 联网后同步北京时间(CST-8)。失败返回 false。
bool whale_net_sync_time(uint32_t timeout_ms);

// 取当前北京时间:day 形如 "2026-10-10",weekday 0=周日,minute_of_day 为当日分钟数。
// 时间未同步时返回 false(调用方按降级路径处理)。
bool whale_net_local_time(char *day, size_t day_size, int *weekday, int *minute_of_day);

// GET https://api.deepseek.com/user/balance,把响应体写进 body。
// 证书校验用 ESP-IDF 的证书包(需在 sdkconfig 里开启 MBEDTLS_CERTIFICATE_BUNDLE)。
esp_err_t whale_net_fetch_balance(const char *api_key, char *body, size_t body_size,
                                  whale_fetch_result_t *result);

#ifdef __cplusplus
}
#endif
