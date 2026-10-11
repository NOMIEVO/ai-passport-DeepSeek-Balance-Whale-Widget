// main/whale_prov.c —— 名片自己开热点 + 起一个网页,让手机/电脑填 Wi-Fi 与 API Key。
//
// 这是本应用唯一的"输入长文本"通道:三颗按键没法输入 API Key,所以配网用网页。
// 页面本身中英文都行(浏览器渲染),但设备屏幕上只显示 ASCII 预览 —— 见 whale_form 的说明。
#include "whale_prov.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "whale_form.h"
#include "whale_net.h"
#include "whale_store.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "whale_prov";
#define AP_PASSWORD     "12345678"
#define POST_BODY_MAX   1024
#define PAGE_MAX        8192
#define SCAN_MAX_RAW    48      // 原始结果最多看这么多条
#define SCAN_MAX_SHOWN  12      // 网页下拉里最多给这么多条

static char s_ap_ssid[20];
static httpd_handle_t s_server;
static whale_prov_cb_t s_cb;
static void *s_user;
static whale_config_t s_config;      // 当前配置(表单提交后会更新)
static whale_ap_option_t s_aps[SCAN_MAX_SHOWN];
static size_t s_ap_count;

// ---- 页面 ------------------------------------------------------------------

// 表单页。占位符依次:标题、标题、副标题、当前 SSID 预览、Key 提示、
// 扫到的热点数量、Wi-Fi 下拉框的全部 <option>、30/60/120/300 的 selected 标记、
// 页脚说明。下拉框内容一变,%s 与实参的顺序就要一起改。
//
// 这里刻意用原生 <select> 而不是 <input list="...">(datalist):datalist 在
// 手机浏览器上表现极不一致 —— iOS Safari 与各家 App 内置浏览器常常压根不弹候
// 选,用户就看到一个空输入框,以为没法选。select 由系统自己画选择器,一定能点开。
// 代价是不能边输边过滤,所以再给一个"手写名称"输入框兜隐藏网络。
static const char *PAGE_FORM =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>%s</title><style>"
    "*{box-sizing:border-box}body{margin:0;padding:16px;background:#0e1420;color:#e6ecf8;"
    "font:16px/1.5 -apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,\"Helvetica Neue\","
    "\"PingFang SC\",\"Microsoft YaHei\",sans-serif}"
    ".card{max-width:420px;margin:0 auto;background:#16203a;border:1px solid #26324f;"
    "border-radius:18px;padding:20px}"
    "h1{margin:0 0 4px;font-size:20px}p.sub{margin:0 0 18px;color:#8fa0c0;font-size:13px}"
    "label{display:block;margin-bottom:14px;font-size:13px;color:#a9b8d6}"
    "input,select{width:100%%;margin-top:6px;padding:11px 12px;border-radius:10px;"
    "border:1px solid #2c3a5c;background:#0f1626;color:#e6ecf8;font-size:15px}"
    "button{width:100%%;margin-top:6px;padding:12px;border:0;border-radius:10px;"
    "background:#3d7bff;color:#fff;font-size:16px;font-weight:600}"
    ".re{display:block;margin-top:10px;padding:11px;border-radius:10px;text-align:center;"
    "border:1px solid #2c3a5c;color:#a9b8d6;text-decoration:none;font-size:14px}"
    ".tip{margin:14px 0 0;color:#7f8fae;font-size:12px}"
    ".now{margin:0 0 14px;font-size:12px;color:#8fa0c0}"
    // 下拉框保留浏览器原生外观:系统会自己画那个小箭头,用户一眼就知道能点开。
    // font-size 给到 16px 是硬性要求 —— 再小 iOS Safari 会在聚焦时自动放大整页。
    "select{background-color:#0f1626;color:#e6ecf8;font-size:16px}"
    ".hint{margin:-8px 0 14px;color:#7f8fae;font-size:12px}"
    "</style></head><body><div class=\"card\">"
    "<h1>%s</h1><p class=\"sub\">%s</p>"
    "<p class=\"now\">当前 Wi-Fi:%s · API Key:%s · 扫到 %d 个热点</p>"
    "<form method=\"post\" action=\"/save\">"
    "<label>附近的 Wi-Fi(2.4GHz)<select name=\"ssid\">%s</select></label>"
    "<p class=\"hint\">没扫到或是不广播名称的网络(隐藏网络),用下面这个框手写。</p>"
    "<label>手写 Wi-Fi 名称<input name=\"ssid_manual\" maxlength=\"32\""
    " autocomplete=\"off\" placeholder=\"留空表示用上面选的那个\"></label>"
    "<label>Wi-Fi 密码<input name=\"pass\" type=\"password\" maxlength=\"63\""
    " autocomplete=\"new-password\" placeholder=\"留空表示开放网络\"></label>"
    "<label>DeepSeek API Key<input name=\"key\" type=\"password\" maxlength=\"128\""
    " autocomplete=\"off\" placeholder=\"sk-...\"></label>"
    "<label>余额刷新间隔<select name=\"refresh\">"
    "<option value=\"30\"%s>30 秒</option><option value=\"60\"%s>60 秒</option>"
    "<option value=\"120\"%s>120 秒</option><option value=\"300\"%s>300 秒</option>"
    "</select></label>"
    "<button type=\"submit\">保存并连接</button></form>"
    "<a class=\"re\" href=\"/rescan\">重新扫描附近热点</a>"
    "<p class=\"tip\">%s</p></div></body></html>";

static const char *PAGE_DONE =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>%s</title><style>body{margin:0;padding:16px;background:#0e1420;color:#e6ecf8;"
    "font:16px/1.6 -apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif}"
    ".card{max-width:420px;margin:0 auto;background:#16203a;border:1px solid #26324f;"
    "border-radius:18px;padding:20px}h1{font-size:19px;margin:0 0 10px}"
    "p{color:#a9b8d6;font-size:14px}a{display:inline-block;margin-top:14px;color:#7ea6ff}"
    "</style></head><body><div class=\"card\"><h1>%s</h1><p>%s</p>"
    "<a href=\"/\">返回继续修改</a></div></body></html>";

// 把 Key 折成 "sk-…abcd" 的形式,网页上不能回显完整密钥。
static void key_hint(const char *key, char *out, size_t out_size)
{
    if (key == NULL || key[0] == '\0') {
        snprintf(out, out_size, "未设置");
        return;
    }
    const size_t len = strlen(key);
    if (len <= 4) {
        snprintf(out, out_size, "已设置");
        return;
    }
    snprintf(out, out_size, "已设置(…%s)", key + len - 4);
}

static esp_err_t send_html(httpd_req_t *req, const char *html)
{
    esp_err_t err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) return err;
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// ---- 附近热点扫描 ----------------------------------------------------------

// 同步扫一次,结果按信号强度排序去重后放进 s_aps。
// 只在任务上下文里调用(net 任务 / httpd 任务),内部是等信号量的,不空转 CPU。
static esp_err_t scan_once(void)
{
    wifi_scan_config_t cfg = { 0 };
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.show_hidden = false;
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&cfg, true);   // true = 等扫完再返回
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t found = 0;
    const esp_err_t num_err = esp_wifi_scan_get_ap_num(&found);
    if (num_err != ESP_OK || found == 0) {
        s_ap_count = 0;
        return num_err;
    }
    if (found > SCAN_MAX_RAW) found = SCAN_MAX_RAW;

    wifi_ap_record_t *records = calloc(found, sizeof(wifi_ap_record_t));
    if (records == NULL) return ESP_ERR_NO_MEM;

    uint16_t count = found;
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        free(records);
        s_ap_count = 0;
        return err;
    }

    s_ap_count = 0;
    for (uint16_t i = 0; i < count; i++) {
        const char *name = (const char *)records[i].ssid;
        if (name[0] == '\0') continue;

        // 同名 SSID 只留信号最强的那一条。
        size_t existing = SIZE_MAX;
        for (size_t j = 0; j < s_ap_count; j++) {
            if (strncmp(s_aps[j].ssid, name, sizeof(s_aps[j].ssid)) == 0) {
                existing = j;
                break;
            }
        }

        if (existing != SIZE_MAX) {
            if (records[i].rssi <= s_aps[existing].rssi) continue;   // 已有的更强
            s_aps[existing].rssi = records[i].rssi;
            s_aps[existing].secured = (records[i].authmode != WIFI_AUTH_OPEN);
            // 变强了就往前挪到正确次序。
            while (existing > 0 && s_aps[existing].rssi > s_aps[existing - 1].rssi) {
                const whale_ap_option_t tmp = s_aps[existing];
                s_aps[existing] = s_aps[existing - 1];
                s_aps[existing - 1] = tmp;
                existing--;
            }
            continue;
        }

        if (s_ap_count == SCAN_MAX_SHOWN) continue;   // 已经列满,且这条连新的都排不进
        s_aps[s_ap_count].rssi = records[i].rssi;
        s_aps[s_ap_count].secured = (records[i].authmode != WIFI_AUTH_OPEN);
        snprintf(s_aps[s_ap_count].ssid, sizeof(s_aps[s_ap_count].ssid), "%s", name);
        s_ap_count++;
    }

    free(records);
    ESP_LOGI(TAG, "扫描完成:列出 %zu 个热点", s_ap_count);
    return ESP_OK;
}

// ---- 页面 ------------------------------------------------------------------

// 把一段 HTML 追加到缓冲区末尾,绝不越界、保证结尾 NUL。返回新的长度。
static size_t append_html(char *dst, size_t dst_size, size_t len, const char *text)
{
    if (dst == NULL || dst_size == 0 || len + 1 >= dst_size) return len;
    const size_t n = strlen(text);
    const size_t room = dst_size - len - 1;
    const size_t cpy = n < room ? n : room;
    memcpy(dst + len, text, cpy);
    len += cpy;
    dst[len] = '\0';
    return len;
}

// 拼出 Wi-Fi 下拉框的全部 <option>:
//   占位项 → 当前已保存的 SSID(选中) → 扫到的热点(按信号强弱) → 手写兜底项
// 装不下时从后往前少列几条,不吐残缺标签。
static size_t build_ssid_options(char *dst, size_t dst_size)
{
    if (dst == NULL || dst_size == 0) return 0;
    dst[0] = '\0';
    size_t len = 0;

    len = append_html(dst, dst_size, len,
                      s_ap_count > 0 ? "<option value=\"\">-- 点开,选附近的一个热点 --</option>"
                                     : "<option value=\"\">-- 没扫到热点,请在下方手写 --</option>");

    if (s_config.ssid[0] != '\0') {
        char esc[WHALE_SSID_MAX * 6];
        whale_html_escape(s_config.ssid, esc, sizeof(esc));
        // esc 会写两遍(value + label),缓冲区按两份转义长度给足。
        char item[sizeof(esc) * 2 + 64];
        snprintf(item, sizeof(item),
                 "<option value=\"%s\" selected>%s · 当前已保存</option>", esc, esc);
        len = append_html(dst, dst_size, len, item);
    }

    for (size_t use = s_ap_count; use >= 1; use--) {
        const size_t need = whale_form_ap_options(s_aps, use, NULL, 0);
        if (need == 0 || need + len + 1 > dst_size) continue;
        char *tmp = calloc(need + 1, 1);
        if (tmp == NULL) break;
        whale_form_ap_options(s_aps, use, tmp, need + 1);
        len = append_html(dst, dst_size, len, tmp);
        free(tmp);
        break;
    }

    len = append_html(dst, dst_size, len,
                      "<option value=\"" WHALE_FORM_SSID_MANUAL "\">"
                      "手写名称(隐藏网络,填下面那个框)</option>");
    return len;
}

static esp_err_t send_form_page(httpd_req_t *req)
{
    char ssid_preview[WHALE_SSID_MAX];
    whale_text_ascii_preview(s_config.ssid, ssid_preview, sizeof(ssid_preview));
    if (ssid_preview[0] == '\0') snprintf(ssid_preview, sizeof(ssid_preview), "未设置");

    char hint[48];
    key_hint(s_config.api_key, hint, sizeof(hint));

    const uint32_t refresh = s_config.refresh_sec;
    const char *sel30 = (refresh == 30) ? " selected" : "";
    const char *sel60 = (refresh == 60) ? " selected" : "";
    const char *sel120 = (refresh == 120) ? " selected" : "";
    const char *sel300 = (refresh == 300) ? " selected" : "";

    char *page = malloc(PAGE_MAX);
    if (page == NULL) return ESP_ERR_NO_MEM;

    // 下拉项单独拼:缓冲区有限,装不下就少列几条,不把 <option> 截成半个标签。
    char options[3072];
    build_ssid_options(options, sizeof(options));

    snprintf(page, PAGE_MAX, PAGE_FORM,
             "小鲸鱼计费名片 · 配置",
             "小鲸鱼计费名片 · 配置",
             "在这张名片上填好 Wi-Fi 与 DeepSeek API Key",
             ssid_preview, hint, (int)s_ap_count, options,
             sel30, sel60, sel120, sel300,
             "提交后名片会立刻尝试连网;API Key 只写进名片自身的 Flash,不会上传别处。");

    const esp_err_t err = send_html(req, page);
    free(page);
    return err;
}

static esp_err_t handler_root(httpd_req_t *req)
{
    // 第一次打开时列表可能还是空的,顺手扫一次。
    if (s_ap_count == 0) (void)scan_once();
    return send_form_page(req);
}

// 重新扫描:扫完直接把表单页吐回去,浏览器不需要 JS。
static esp_err_t handler_rescan(httpd_req_t *req)
{
    (void)scan_once();
    return send_form_page(req);
}

static esp_err_t handler_captive(httpd_req_t *req)
{
    // 各家系统的联网探测地址:一律 302 回表单页,让手机自动弹出配网窗口。
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char *body = malloc(POST_BODY_MAX + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    int remaining = req->content_len;
    if (remaining > POST_BODY_MAX) {
        free(body);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, NULL);
        return ESP_OK;
    }
    while (remaining > 0) {
        const int read = httpd_req_recv(req, body + received, remaining);
        if (read <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
            return ESP_OK;
        }
        received += (size_t)read;
        remaining -= read;
    }
    body[received] = '\0';

    whale_form_t form;
    memset(&form, 0, sizeof(form));
    const bool parsed = whale_form_parse(body, &form);
    free(body);

    const char *message;
    if (!parsed) {
        message = "没有识别到有效内容,请至少选一个 Wi-Fi、填写 Wi-Fi 密码或 API Key。";
    } else {
        // 只覆盖表单里出现的字段。SSID 走"手写框 > 下拉框"的优先级,
        // 两边都没给就保留原名(用户可能只是来改 API Key 的)。
        char chosen_ssid[WHALE_SSID_MAX];
        if (whale_form_resolve_ssid(&form, chosen_ssid, sizeof(chosen_ssid))) {
            snprintf(s_config.ssid, sizeof(s_config.ssid), "%s", chosen_ssid);
        }
        if (form.has_password) snprintf(s_config.password, sizeof(s_config.password), "%s", form.password);
        if (form.has_api_key) snprintf(s_config.api_key, sizeof(s_config.api_key), "%s", form.api_key);
        if (form.has_refresh) s_config.refresh_sec = form.refresh_sec;

        if (!whale_store_save_config(&s_config)) {
            message = "写入名片失败(NVS 不可用),请重启后再试。";
        } else if (s_config.ssid[0] == '\0') {
            message = "其余设置已保存,但还没有 Wi-Fi 名称:请在下拉框里选一个热点,"
                      "或在“手写 Wi-Fi 名称”框里填一个。";
        } else {
            message = "已保存。名片正在尝试连接,屏幕会显示连接结果。"
                      "连接成功时会响一声,之后每隔一段时间自动刷新余额。";
            ESP_LOGI(TAG, "表单已保存:SSID=%s 刷新=%lus Key=%s",
                     s_config.ssid, (unsigned long)s_config.refresh_sec,
                     s_config.api_key[0] != '\0' ? "已设置" : "空");
            if (s_cb != NULL) s_cb(&s_config, s_user);
        }
    }

    char *page = malloc(1536);
    if (page == NULL) return ESP_ERR_NO_MEM;
    snprintf(page, 1536, PAGE_DONE, "小鲸鱼计费名片", "小鲸鱼计费名片", message);
    const esp_err_t err = send_html(req, page);
    free(page);
    return err;
}

static const httpd_uri_t URI_ROOT = {
    .uri = "/", .method = HTTP_GET, .handler = handler_root, .user_ctx = NULL
};
static const httpd_uri_t URI_SAVE = {
    .uri = "/save", .method = HTTP_POST, .handler = handler_save, .user_ctx = NULL
};
static const httpd_uri_t URI_RESCAN = {
    .uri = "/rescan", .method = HTTP_GET, .handler = handler_rescan, .user_ctx = NULL
};
static const httpd_uri_t URI_CAPTIVE_1 = {
    .uri = "/generate_204", .method = HTTP_GET, .handler = handler_captive, .user_ctx = NULL
};
static const httpd_uri_t URI_CAPTIVE_2 = {
    .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handler_captive, .user_ctx = NULL
};
static const httpd_uri_t URI_CAPTIVE_3 = {
    .uri = "/favicon.ico", .method = HTTP_GET, .handler = handler_captive, .user_ctx = NULL
};
static const httpd_uri_t URI_CAPTIVE_4 = {
    .uri = "/connecttest.txt", .method = HTTP_GET, .handler = handler_captive, .user_ctx = NULL
};

// ---- 生命周期 --------------------------------------------------------------

esp_err_t whale_prov_start(const whale_config_t *current, whale_prov_cb_t cb, void *user)
{
    if (s_server != NULL) return ESP_OK;
    if (current != NULL) s_config = *current;
    s_cb = cb;
    s_user = user;

    // AP 名称带 MAC 后两位,多张名片放在一起也不至于连错。
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "WhaleCard-%02X%02X", mac[4], mac[5]);

    esp_err_t err = whale_net_ap_start(s_ap_ssid, AP_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    // 请求头预算在 sdkconfig.defaults 里放大到 2 KB
    // (CONFIG_HTTPD_MAX_REQ_HDR_LEN):手机/PC 浏览器的 User-Agent + Accept* +
    // sec-ch-ua 一堆头,512 B 的默认预算会让 httpd 直接回 "Header fields are
    // too long"(431),页面根本打不开。
    // 8 个 handler:/、/save、/rescan + 4 个探测地址。
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.lru_purge_enable = true;

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        whale_net_ap_stop();
        return err;
    }

    httpd_register_uri_handler(s_server, &URI_ROOT);
    httpd_register_uri_handler(s_server, &URI_SAVE);
    httpd_register_uri_handler(s_server, &URI_RESCAN);
    httpd_register_uri_handler(s_server, &URI_CAPTIVE_1);
    httpd_register_uri_handler(s_server, &URI_CAPTIVE_2);
    httpd_register_uri_handler(s_server, &URI_CAPTIVE_3);
    httpd_register_uri_handler(s_server, &URI_CAPTIVE_4);

    // 先把附近热点扫一遍再挂 NVS/网页:用户第一次打开表单时列表已经在手上了。
    (void)scan_once();

    ESP_LOGI(TAG, "配网网页已就绪: http://192.168.4.1 (AP %s)", s_ap_ssid);
    return ESP_OK;
}

void whale_prov_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    whale_net_ap_stop();
    s_cb = NULL;
    s_user = NULL;
    s_ap_count = 0;   // 下次配网重新扫,不要拿旧列表糊弄人
}

bool whale_prov_running(void)
{
    return s_server != NULL;
}

const char *whale_prov_ssid(void)
{
    return s_ap_ssid[0] != '\0' ? s_ap_ssid : "WhaleCard";
}

const char *whale_prov_password(void)
{
    return AP_PASSWORD;
}
