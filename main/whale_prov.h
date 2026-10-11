// main/whale_prov.h —— 配网网页:SoftAP + HTTP 表单(填 Wi-Fi 与 API Key)。
#pragma once

#include "esp_err.h"
#include "whale_model.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 表单保存后回调:通知应用层配置已更新(应用层负责重连与刷新)。
// 回调运行在 httpd 任务里,只能投递事件,不能做联网或刷屏。
typedef void (*whale_prov_cb_t)(const whale_config_t *saved, void *user);

esp_err_t whale_prov_start(const whale_config_t *current, whale_prov_cb_t cb, void *user);
void whale_prov_stop(void);
bool whale_prov_running(void);

// 屏幕上要显示的 AP 信息(固定后缀 = MAC 后两位,便于分辨多张名片)。
const char *whale_prov_ssid(void);
const char *whale_prov_password(void);

#ifdef __cplusplus
}
#endif
