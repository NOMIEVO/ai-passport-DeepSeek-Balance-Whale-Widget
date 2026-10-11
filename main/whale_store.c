// main/whale_store.c —— 配置与账本存 NVS。
//
// 两个对象都以 blob 整体存取:结构里全是定长数组,没有指针,直接落盘/回读即可。
// 长度对不上(比如换了结构)时按"读不到"处理,绝不拿半截数据当配置用。
//
// 注意:API Key 和 Wi-Fi 口令存在 flash 里。这是配网功能的固有取舍,文档里写明了
// 「恢复出厂/清除凭据」入口,日志里一律不打印这两项。
#include "whale_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "whale_store";
static const char *NVS_NAMESPACE = "whale";
static const char *KEY_CONFIG = "cfg";
static const char *KEY_LEDGER = "ledger";

// 加「今日预算 / 余额预警 / 按压音效 / 音效总开关」之前落盘的配置结构。
// 结构一变长,NVS 里的旧 blob 长度就对不上,直接判"读不到"会逼用户重新配网;
// 所以这里认一次旧长度并按字段搬过来,只让新增字段取默认值。
typedef struct {
    char ssid[WHALE_SSID_MAX];
    char password[WHALE_PASS_MAX];
    char api_key[WHALE_KEY_MAX];
    uint32_t refresh_sec;
    bool sound_spend;
    bool sound_connect;
} whale_config_legacy_t;

// 加「峰谷显示样式」之前落盘的配置结构:比现在的少一个 peak_style,其余字段同序同名。
typedef struct {
    char ssid[WHALE_SSID_MAX];
    char password[WHALE_PASS_MAX];
    char api_key[WHALE_KEY_MAX];
    uint32_t refresh_sec;
    bool sound_all;
    bool sound_press;
    bool sound_spend;
    bool sound_connect;
    int64_t budget_cents;
    int64_t warn_cents;
} whale_config_prev_t;

// 加「熄屏时间」之前落盘的配置结构:只比现在的少一个 sleep_sec,其余字段同序同名。
typedef struct {
    char ssid[WHALE_SSID_MAX];
    char password[WHALE_PASS_MAX];
    char api_key[WHALE_KEY_MAX];
    uint32_t refresh_sec;
    bool sound_all;
    bool sound_press;
    bool sound_spend;
    bool sound_connect;
    int64_t budget_cents;
    int64_t warn_cents;
    int peak_style;
} whale_config_v3_t;

// 四档长度必须互不相同,否则"按长度认档"会认错版本。
_Static_assert(sizeof(whale_config_prev_t) != sizeof(whale_config_legacy_t),
               "两版旧配置长度相同,无法按长度区分");
_Static_assert(sizeof(whale_config_prev_t) != sizeof(whale_config_t),
               "旧配置长度与当前结构相同,迁移分支永远走不到");
_Static_assert(sizeof(whale_config_v3_t) != sizeof(whale_config_prev_t),
               "v3 与 v2 配置长度相同,无法按长度区分");
_Static_assert(sizeof(whale_config_v3_t) != sizeof(whale_config_legacy_t),
               "v3 与 legacy 配置长度相同,无法按长度区分");
_Static_assert(sizeof(whale_config_v3_t) != sizeof(whale_config_t),
               "v3 配置长度与当前结构相同,迁移分支永远走不到");

// 只比当前结构少 sleep_sec,直接按字段搬,最后一个字段取默认值。
static bool load_v3_config(nvs_handle_t handle, whale_config_t *out)
{
    whale_config_v3_t old;

    size_t length = 0;
    if (nvs_get_blob(handle, KEY_CONFIG, NULL, &length) != ESP_OK) return false;
    if (length != sizeof(old)) return false;

    if (nvs_get_blob(handle, KEY_CONFIG, &old, &length) != ESP_OK) return false;
    if (length != sizeof(old)) return false;

    whale_config_t migrated;
    whale_config_init(&migrated);
    snprintf(migrated.ssid, sizeof(migrated.ssid), "%s", old.ssid);
    snprintf(migrated.password, sizeof(migrated.password), "%s", old.password);
    snprintf(migrated.api_key, sizeof(migrated.api_key), "%s", old.api_key);
    migrated.refresh_sec = old.refresh_sec;
    migrated.sound_all = old.sound_all;
    migrated.sound_press = old.sound_press;
    migrated.sound_spend = old.sound_spend;
    migrated.sound_connect = old.sound_connect;
    migrated.budget_cents = old.budget_cents;
    migrated.warn_cents = old.warn_cents;
    migrated.peak_style = old.peak_style;
    // sleep_sec 取默认值(init 已给),不额外赋值,免得以后再加字段时漏搬。

    *out = migrated;
    ESP_LOGW(TAG, "发现上一版配置(%u 字节),已迁移;熄屏时间取默认", (unsigned)sizeof(old));
    return true;
}

static bool load_prev_config(nvs_handle_t handle, whale_config_t *out)
{
    whale_config_prev_t old;

    size_t length = 0;
    if (nvs_get_blob(handle, KEY_CONFIG, NULL, &length) != ESP_OK) return false;
    if (length != sizeof(old)) return false;

    if (nvs_get_blob(handle, KEY_CONFIG, &old, &length) != ESP_OK) return false;
    if (length != sizeof(old)) return false;

    whale_config_t migrated;
    whale_config_init(&migrated);
    snprintf(migrated.ssid, sizeof(migrated.ssid), "%s", old.ssid);
    snprintf(migrated.password, sizeof(migrated.password), "%s", old.password);
    snprintf(migrated.api_key, sizeof(migrated.api_key), "%s", old.api_key);
    migrated.refresh_sec = old.refresh_sec;
    migrated.sound_all = old.sound_all;
    migrated.sound_press = old.sound_press;
    migrated.sound_spend = old.sound_spend;
    migrated.sound_connect = old.sound_connect;
    migrated.budget_cents = old.budget_cents;
    migrated.warn_cents = old.warn_cents;
    // peak_style 取默认值(init 已给),不额外赋值,免得以后再加字段时漏搬。

    *out = migrated;
    ESP_LOGW(TAG, "发现上一版配置(%u 字节),已迁移;峰谷样式取默认", (unsigned)sizeof(old));
    return true;
}

static bool load_legacy_config(nvs_handle_t handle, whale_config_t *out)
{
    whale_config_legacy_t old;

    size_t length = 0;
    if (nvs_get_blob(handle, KEY_CONFIG, NULL, &length) != ESP_OK) return false;
    if (length != sizeof(old)) return false;

    if (nvs_get_blob(handle, KEY_CONFIG, &old, &length) != ESP_OK) return false;
    if (length != sizeof(old)) return false;

    whale_config_t migrated;
    whale_config_init(&migrated);
    snprintf(migrated.ssid, sizeof(migrated.ssid), "%s", old.ssid);
    snprintf(migrated.password, sizeof(migrated.password), "%s", old.password);
    snprintf(migrated.api_key, sizeof(migrated.api_key), "%s", old.api_key);
    migrated.refresh_sec = old.refresh_sec;
    migrated.sound_spend = old.sound_spend;
    migrated.sound_connect = old.sound_connect;

    *out = migrated;
    ESP_LOGW(TAG, "发现旧版配置(%u 字节),已迁移;新增项取默认值", (unsigned)sizeof(old));
    return true;
}

bool whale_store_load_config(whale_config_t *cfg)
{
    if (cfg == NULL) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t length = 0;
    esp_err_t err = nvs_get_blob(handle, KEY_CONFIG, NULL, &length);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    bool ok = false;
    if (length == sizeof(whale_config_t)) {
        whale_config_t stored;
        err = nvs_get_blob(handle, KEY_CONFIG, &stored, &length);
        if (err == ESP_OK && length == sizeof(whale_config_t)) {
            *cfg = stored;
            ok = true;
        }
    } else if (load_v3_config(handle, cfg)) {
        ok = true;
    } else if (load_prev_config(handle, cfg)) {
        ok = true;
    } else if (load_legacy_config(handle, cfg)) {
        ok = true;
    }

    nvs_close(handle);
    return ok;
}

bool whale_store_save_config(const whale_config_t *cfg)
{
    if (cfg == NULL) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;

    esp_err_t err = nvs_set_blob(handle, KEY_CONFIG, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "配置写入失败: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool whale_store_load_ledger(whale_ledger_t *ledger)
{
    if (ledger == NULL) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t length = 0;
    esp_err_t err = nvs_get_blob(handle, KEY_LEDGER, NULL, &length);
    if (err != ESP_OK || length != sizeof(whale_ledger_t)) {
        nvs_close(handle);
        return false;
    }

    whale_ledger_t stored;
    err = nvs_get_blob(handle, KEY_LEDGER, &stored, &length);
    nvs_close(handle);
    if (err != ESP_OK) return false;

    *ledger = stored;
    return true;
}

bool whale_store_save_ledger(const whale_ledger_t *ledger)
{
    if (ledger == NULL) return false;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;

    esp_err_t err = nvs_set_blob(handle, KEY_LEDGER, ledger, sizeof(*ledger));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "账本写入失败: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool whale_store_clear_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;

    whale_config_t cfg;
    size_t length = sizeof(cfg);
    if (nvs_get_blob(handle, KEY_CONFIG, &cfg, &length) != ESP_OK ||
        length != sizeof(cfg)) {
        whale_config_init(&cfg);
    }
    cfg.ssid[0] = '\0';
    cfg.password[0] = '\0';
    cfg.api_key[0] = '\0';

    esp_err_t err = nvs_set_blob(handle, KEY_CONFIG, &cfg, sizeof(cfg));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}
