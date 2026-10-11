// main/whale_form.c —— 表单解析与预览文本的实现。
#include "whale_form.h"

#include <stdio.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t whale_url_decode(const char *src, char *dst, size_t dst_size)
{
    if (src == NULL) return 0;

    size_t written = 0;
    for (const char *p = src; *p != '\0'; p++) {
        char decoded;
        if (*p == '+') {
            decoded = ' ';
        } else if (*p == '%') {
            const int high = hex_value(p[1]);
            const int low = hex_value(p[2]);
            if (high < 0 || low < 0) {
                decoded = *p;             // 非法转义原样保留,不至于整条丢掉
            } else {
                decoded = (char)((high << 4) | low);
                p += 2;
            }
        } else {
            decoded = *p;
        }

        if (dst != NULL && written + 1 < dst_size) dst[written] = decoded;
        written++;
    }
    if (dst != NULL && dst_size > 0) dst[written < dst_size - 1 ? written : dst_size - 1] = '\0';
    return written;
}

// 把 "key=value" 拆成两段:value 指向原串内部。
static bool split_pair(char *pair, const char **key, const char **value)
{
    char *sep = strchr(pair, '=');
    if (sep == NULL) return false;
    *sep = '\0';
    *key = pair;
    *value = sep + 1;
    return true;
}

static bool parse_u32(const char *text, uint32_t *out)
{
    if (text == NULL || *text == '\0') return false;
    uint32_t value = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return false;
        if (value > (4294967295u - 9u) / 10u) return false;
        value = value * 10u + (uint32_t)(*p - '0');
    }
    *out = value;
    return true;
}

bool whale_form_parse(const char *body, whale_form_t *out)
{
    if (body == NULL || out == NULL) return false;

    bool recognized = false;
    const char *cursor = body;

    while (*cursor != '\0') {
        const char *amp = strchr(cursor, '&');
        const size_t length = (amp != NULL) ? (size_t)(amp - cursor) : strlen(cursor);
        if (length == 0) {
            if (amp == NULL) break;
            cursor = amp + 1;
            continue;
        }

        char pair[512];
        if (length >= sizeof(pair)) {
            if (amp == NULL) break;
            cursor = amp + 1;
            continue;             // 单段过长,跳过而不是截断出半个字段
        }
        memcpy(pair, cursor, length);
        pair[length] = '\0';

        const char *key = NULL;
        const char *value = NULL;
        if (split_pair(pair, &key, &value)) {
            char decoded[512];
            whale_url_decode(value, decoded, sizeof(decoded));

            // 用 %.Ns 显式限长:既防止超长输入截断成半个多字节字符,
            // 也让编译器确认不会写溢出(本仓库把 -Wformat-truncation 当错误)。
            if (strcmp(key, "ssid") == 0) {
                snprintf(out->ssid, sizeof(out->ssid), "%.*s", (int)sizeof(out->ssid) - 1, decoded);
                out->has_ssid = true;
                recognized = true;
            } else if (strcmp(key, "ssid_manual") == 0) {
                snprintf(out->ssid_manual, sizeof(out->ssid_manual), "%.*s",
                         (int)sizeof(out->ssid_manual) - 1, decoded);
                out->has_ssid_manual = true;
                recognized = true;
            } else if (strcmp(key, "pass") == 0) {
                snprintf(out->password, sizeof(out->password), "%.*s",
                         (int)sizeof(out->password) - 1, decoded);
                out->has_password = true;
                recognized = true;
            } else if (strcmp(key, "key") == 0) {
                snprintf(out->api_key, sizeof(out->api_key), "%.*s",
                         (int)sizeof(out->api_key) - 1, decoded);
                out->has_api_key = true;
                recognized = true;
            } else if (strcmp(key, "refresh") == 0) {
                uint32_t seconds = 0;
                if (parse_u32(decoded, &seconds) && seconds >= 15 && seconds <= 3600) {
                    out->refresh_sec = seconds;
                    out->has_refresh = true;
                    recognized = true;
                }
            }
        }

        if (amp == NULL) break;
        cursor = amp + 1;
    }

    return recognized;
}

bool whale_form_resolve_ssid(const whale_form_t *form, char *out, size_t out_size)
{
    if (form == NULL) return false;

    const char *chosen = NULL;
    if (form->has_ssid_manual && form->ssid_manual[0] != '\0') {
        chosen = form->ssid_manual;                      // 手写框最优先
    } else if (form->has_ssid && form->ssid[0] != '\0' &&
               strcmp(form->ssid, WHALE_FORM_SSID_MANUAL) != 0) {
        chosen = form->ssid;                             // 下拉框里挑的那一张
    }
    if (chosen == NULL) return false;

    if (out != NULL && out_size > 0) {
        snprintf(out, out_size, "%.*s", (int)out_size - 1, chosen);
    }
    return true;
}

size_t whale_html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t written = 0;
    const char *entity = NULL;

    // 通用的一次写 n 字节助手:dst 为 NULL 时只累计长度。
#define EMIT(str, len)                                                          \
    do {                                                                        \
        const size_t n = (len);                                                 \
        if (dst != NULL) {                                                      \
            size_t room = dst_size - written;                                   \
            if (room > 0) {                                                     \
                const size_t cpy = n < room - 1 ? n : room - 1;                  \
                memcpy(dst + written, (str), cpy);                               \
            }                                                                    \
        }                                                                        \
        written += n;                                                            \
    } while (0)

    if (src == NULL) src = "";
    for (const char *p = src; *p != '\0'; p++) {
        switch (*p) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        case '"': entity = "&quot;"; break;
        case '\'': entity = "&#39;"; break;
        default: entity = NULL; break;
        }
        if (entity != NULL) {
            EMIT(entity, strlen(entity));
        } else {
            EMIT(p, 1);
        }
    }
    if (dst != NULL && dst_size > 0) {
        size_t term = written < dst_size - 1 ? written : dst_size - 1;
        dst[term] = '\0';
    }
    return written;
#undef EMIT
}

size_t whale_form_ap_options(const whale_ap_option_t *aps, size_t count, char *dst,
                             size_t dst_size)
{
    size_t written = 0;

    if (aps == NULL || count == 0) return 0;
    // 任何出口都保证 dst 是合法 C 串,调用方可以直接 strlen/拼接。
    if (dst != NULL && dst_size > 0) dst[0] = '\0';

    for (size_t i = 0; i < count; i++) {
        if (aps[i].ssid[0] == '\0') continue;

        // <option value="SSID">SSID · RSSI dBm · 加密/开放</option>
        // label 里必须重复一遍 SSID:原生下拉收起后只看得到 label。
        const char *prefix = "<option value=\"";
        const char *mid = "\">";
        const char *sep = " · ";
        const char *tail = "</option>";

        char escaped[WHALE_SSID_MAX * 6];     // 最坏情况每个字节都转义成 6 个字符
        char intensity[32];
        snprintf(intensity, sizeof(intensity), "%ddBm%s%s", aps[i].rssi, sep,
                 aps[i].secured ? "需要密码" : "开放");

        const size_t escaped_len = whale_html_escape(aps[i].ssid, NULL, 0);

        size_t need = strlen(prefix);
        need += escaped_len;                  // value
        need += strlen(mid);
        need += escaped_len;                  // label 里的 SSID
        need += strlen(sep) + strlen(intensity) + strlen(tail);

        if (dst == NULL) {
            written += need;
            continue;
        }
        if (written + need + 1 > dst_size) break;    // 放不下就整条放弃,不写半个 option

        memcpy(dst + written, prefix, strlen(prefix));
        written += strlen(prefix);
        whale_html_escape(aps[i].ssid, escaped, sizeof(escaped));
        memcpy(dst + written, escaped, strlen(escaped));
        written += strlen(escaped);
        memcpy(dst + written, mid, strlen(mid));
        written += strlen(mid);
        memcpy(dst + written, escaped, strlen(escaped));      // label:SSID 本体
        written += strlen(escaped);
        memcpy(dst + written, sep, strlen(sep));
        written += strlen(sep);
        memcpy(dst + written, intensity, strlen(intensity));
        written += strlen(intensity);
        memcpy(dst + written, tail, strlen(tail));
        written += strlen(tail);

        dst[written] = '\0';
    }

    return written;
}

size_t whale_text_ascii_preview(const char *in, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return 0;

    size_t written = 0;
    for (const char *p = in; p != NULL && *p != '\0' && written + 1 < out_size; p++) {
        const unsigned char c = (unsigned char)*p;
        out[written++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[written] = '\0';
    return written;
}
