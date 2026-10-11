// main/whale_model.c —— 小鲸鱼计费名片的纯逻辑实现(不依赖 ESP-IDF / LVGL)。
//
// 记账规则参照开源挂件 DeepSeek-Balance-Whale-Widget:余额**下降**按观测累计为消费,
// 余额**上升**(充值 / 赠金)单独记录、不冲抵已有消费;跨日以当日首次观测重建基线。
#include "whale_model.h"

#include <stdio.h>
#include <string.h>

// 峰谷时段(北京时间,分钟计)。与挂件的口径一致:工作日两个高峰窗口,周末全天谷时。
#define PEAK_START_1 540    // 09:00
#define PEAK_END_1   720    // 12:00
#define PEAK_START_2 840    // 14:00
#define PEAK_END_2   1080   // 18:00
#define DAY_MINUTES  1440

static const uint32_t REFRESH_OPTIONS[] = { 30, 60, 120, 300 };

static bool is_weekend(int weekday)
{
    return weekday == 0 || weekday == 6;
}

static int clamp_minute(int minute_of_day)
{
    if (minute_of_day < 0) return 0;
    if (minute_of_day >= DAY_MINUTES) return DAY_MINUTES - 1;
    return minute_of_day;
}

// ---- 金额 ------------------------------------------------------------------

bool whale_money_parse(const char *text, int64_t *out_cents)
{
    if (text == NULL || out_cents == NULL) return false;

    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;

    bool negative = false;
    if (*text == '-') {
        negative = true;
        text++;
    } else if (*text == '+') {
        text++;
    }

    int64_t integer_part = 0;
    int digits = 0;
    while (*text >= '0' && *text <= '9') {
        if (integer_part > (INT64_MAX - 9) / 10) return false;   // 明显越界
        integer_part = integer_part * 10 + (*text - '0');
        text++;
        digits++;
    }
    if (digits == 0) return false;

    int64_t fraction = 0;
    int64_t fraction_scale = 1;
    if (*text == '.') {
        text++;
        int fraction_digits = 0;
        // 只取前 6 位小数:余额报文给两位,多取没有意义且会溢出。
        while (*text >= '0' && *text <= '9' && fraction_digits < 6) {
            fraction = fraction * 10 + (*text - '0');
            fraction_scale *= 10;
            text++;
            fraction_digits++;
        }
        while (*text >= '0' && *text <= '9') text++;   // 更低的位只影响四舍五入
    }

    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    if (*text != '\0') return false;

    int64_t cents = integer_part * 100;
    if (fraction_scale > 1) {
        cents += (fraction * 100 + fraction_scale / 2) / fraction_scale;   // 四舍五入到分
    }
    *out_cents = negative ? -cents : cents;
    return true;
}

static size_t format_cents(int64_t cents, char *out, size_t out_size, bool always_sign)
{
    if (out == NULL || out_size < 5) return 0;

    bool negative = cents < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(cents + 1)) + 1u : (uint64_t)cents;
    const char *sign = negative ? "-" : (always_sign ? "+" : "");

    int written = snprintf(out, out_size, "%s%llu.%02llu", sign,
                           (unsigned long long)(magnitude / 100u),
                           (unsigned long long)(magnitude % 100u));
    if (written < 0 || (size_t)written >= out_size) return 0;
    return (size_t)written;
}

size_t whale_money_format(int64_t cents, char *out, size_t out_size)
{
    return format_cents(cents, out, out_size, false);
}

size_t whale_money_format_signed(int64_t cents, char *out, size_t out_size)
{
    if (cents == 0) return format_cents(0, out, out_size, false);
    return format_cents(cents, out, out_size, true);
}

// ---- 余额报文 --------------------------------------------------------------

// 找到 "key" 后面那个值的起点。要求键名本身带引号,避免匹配到字符串值里的同名文本。
static const char *find_value_start(const char *json, const char *key)
{
    if (json == NULL) return NULL;
    const size_t key_len = strlen(key);

    for (const char *hit = json; (hit = strstr(hit, key)) != NULL; hit++) {
        if (hit > json && hit[-1] != '"') continue;
        const char *p = hit + key_len;
        if (*p != '"') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        return p;
    }
    return NULL;
}

// 把值拷进缓冲区并去掉两侧引号;返回 false 表示值缺失或过长。
static bool copy_scalar(const char *value_start, char *out, size_t out_size)
{
    if (value_start == NULL) return false;

    const char *p = value_start;
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (end == NULL) return false;
        const size_t len = (size_t)(end - p);
        if (len == 0 || len >= out_size) return false;
        memcpy(out, p, len);
        out[len] = '\0';
        return true;
    }

    // 非引号值:数字 / true / false / null,遇到分隔符或空白即结束。
    size_t len = 0;
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\n' && *p != '\r') {
        if (len + 1 >= out_size) return false;
        out[len++] = *p++;
    }
    if (len == 0) return false;
    out[len] = '\0';
    return true;
}

bool whale_balance_extract(const char *json, whale_balance_t *out)
{
    if (json == NULL) return false;

    whale_balance_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.available = true;

    // 只在 balance_infos 数组之后找字段,避免同名的其它节点。
    const char *infos = strstr(json, "balance_infos");
    if (infos == NULL) return false;

    char buffer[64];
    if (copy_scalar(find_value_start(infos, "total_balance"), buffer, sizeof(buffer))) {
        if (!whale_money_parse(buffer, &parsed.total_cents)) return false;
    } else {
        return false;   // 没有余额就没有"今日已用",宁可报错也不要显示 0
    }

    if (copy_scalar(find_value_start(infos, "granted_balance"), buffer, sizeof(buffer))) {
        (void)whale_money_parse(buffer, &parsed.granted_cents);
    }
    if (copy_scalar(find_value_start(infos, "topped_up_balance"), buffer, sizeof(buffer))) {
        (void)whale_money_parse(buffer, &parsed.topped_cents);
    }
    if (copy_scalar(find_value_start(infos, "currency"), buffer, sizeof(buffer))) {
        // 显式限长:货币码只有三四个字符,超出的直接丢掉。
        snprintf(parsed.currency, sizeof(parsed.currency), "%.*s",
                 (int)sizeof(parsed.currency) - 1, buffer);
    }

    if (copy_scalar(find_value_start(json, "is_available"), buffer, sizeof(buffer))) {
        parsed.available = !(strcmp(buffer, "false") == 0 || strcmp(buffer, "0") == 0);
    }

    if (out != NULL) *out = parsed;
    return true;
}

// ---- 峰谷 ------------------------------------------------------------------

whale_tariff_t whale_tariff_at(int weekday, int minute_of_day)
{
    if (is_weekend(weekday)) return WHALE_TARIFF_VALLEY;

    const int minute = clamp_minute(minute_of_day);
    const bool peak = (minute >= PEAK_START_1 && minute < PEAK_END_1) ||
                      (minute >= PEAK_START_2 && minute < PEAK_END_2);
    return peak ? WHALE_TARIFF_PEAK : WHALE_TARIFF_VALLEY;
}

int whale_tariff_minutes_to_change(int weekday, int minute_of_day)
{
    if (is_weekend(weekday)) return -1;

    const int minute = clamp_minute(minute_of_day);
    if (whale_tariff_at(weekday, minute) == WHALE_TARIFF_PEAK) {
        const int end = (minute < PEAK_END_1) ? PEAK_END_1 : PEAK_END_2;
        return end - minute;
    }
    if (minute < PEAK_START_1) return PEAK_START_1 - minute;
    if (minute < PEAK_START_2) return PEAK_START_2 - minute;
    return -1;   // 18:00 之后到当天结束都是谷时
}

const char *whale_peak_style_name(whale_peak_style_t style)
{
    switch (style) {
    case WHALE_PEAK_LIANGWEN:   return "梁文峰谷";
    case WHALE_PEAK_QIANGQIANG: return "!?强强?!";
    case WHALE_PEAK_COUNT:      return "倒计时";
    case WHALE_PEAK_MINI:       return "简洁";
    default:                    return "默认";
    }
}

const char *whale_peak_status_text(whale_peak_style_t style, bool is_peak)
{
    switch (style) {
    case WHALE_PEAK_LIANGWEN:   return is_peak ? "梁文峰" : "梁文谷";
    case WHALE_PEAK_QIANGQIANG: return is_peak ? "!?峰峰?!" : "!?谷谷?!";
    case WHALE_PEAK_MINI:       return is_peak ? "峰" : "谷";
    case WHALE_PEAK_COUNT:      return NULL;   // 数值是倒计时,界面自己拼
    default:                    return is_peak ? "高峰时段" : "空闲时段";
    }
}

whale_peak_style_t whale_peak_style_clamp(int value)
{
    if (value < 0 || value >= WHALE_PEAK_STYLE_COUNT) return WHALE_PEAK_DEFAULT;
    return (whale_peak_style_t)value;
}

whale_peak_style_t whale_peak_style_next(whale_peak_style_t style)
{
    return (whale_peak_style_t)(((int)whale_peak_style_clamp((int)style) + 1) %
                                WHALE_PEAK_STYLE_COUNT);
}

// ---- 记账 ------------------------------------------------------------------

void whale_ledger_reset(whale_ledger_t *ledger, const char *day)
{
    if (ledger == NULL) return;
    memset(ledger, 0, sizeof(*ledger));
    if (day != NULL) snprintf(ledger->day, sizeof(ledger->day), "%.10s", day);
}

whale_ledger_event_t whale_ledger_observe(whale_ledger_t *ledger, const char *day,
                                          int64_t balance_cents, int64_t *delta_cents)
{
    if (ledger == NULL) return WHALE_LEDGER_UNCHANGED;

    const bool new_day = day != NULL && day[0] != '\0' &&
                         strncmp(day, ledger->day, WHALE_DAY_LEN - 1) != 0;

    if (!ledger->has_observation || new_day) {
        whale_ledger_reset(ledger, day);
        ledger->opening_cents = balance_cents;
        ledger->last_cents = balance_cents;
        ledger->has_observation = true;
        if (delta_cents != NULL) *delta_cents = 0;
        return WHALE_LEDGER_FIRST;
    }

    const int64_t delta = balance_cents - ledger->last_cents;
    ledger->last_cents = balance_cents;
    if (delta_cents != NULL) *delta_cents = delta;

    if (delta < 0) {
        ledger->used_cents += -delta;
        return WHALE_LEDGER_SPEND;
    }
    if (delta > 0) {
        ledger->topup_cents += delta;
        return WHALE_LEDGER_TOPUP;
    }
    return WHALE_LEDGER_UNCHANGED;
}

// ---- 配置 ------------------------------------------------------------------

void whale_config_init(whale_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->refresh_sec = 60;
    cfg->sound_all = true;
    cfg->sound_press = true;
    cfg->sound_spend = true;
    cfg->sound_connect = true;
    cfg->budget_cents = 0;      // 默认不设预算 / 不预警,与挂件的出厂行为一致
    cfg->warn_cents = 0;
    cfg->peak_style = WHALE_PEAK_DEFAULT;
    cfg->sleep_sec = WHALE_SLEEP_DEFAULT;
}

bool whale_config_ready(const whale_config_t *cfg)
{
    if (cfg == NULL) return false;
    return cfg->ssid[0] != '\0' && cfg->api_key[0] != '\0';
}

uint32_t whale_refresh_next(uint32_t current_sec)
{
    for (size_t i = 0; i < sizeof(REFRESH_OPTIONS) / sizeof(REFRESH_OPTIONS[0]); i++) {
        if (current_sec == REFRESH_OPTIONS[i]) {
            return REFRESH_OPTIONS[(i + 1) % (sizeof(REFRESH_OPTIONS) / sizeof(REFRESH_OPTIONS[0]))];
        }
    }
    return REFRESH_OPTIONS[1];   // 不在预设档位上时回到默认 60 秒
}

// 预算与预警共用档位(分)。0 放最前也放最后,循环含义是「关 → 开 → … → 关」。
static const int64_t THRESHOLD_OPTIONS[] = { 0, 500, 1000, 2000, 5000 };

static int64_t threshold_next(int64_t current_cents)
{
    const size_t count = sizeof(THRESHOLD_OPTIONS) / sizeof(THRESHOLD_OPTIONS[0]);
    for (size_t i = 0; i < count; i++) {
        if (current_cents == THRESHOLD_OPTIONS[i]) {
            return THRESHOLD_OPTIONS[(i + 1) % count];
        }
    }
    return THRESHOLD_OPTIONS[1];   // 不在档位上时给一个最小的非零档
}

int64_t whale_budget_next(int64_t current_cents)
{
    return threshold_next(current_cents);
}

int64_t whale_warn_next(int64_t current_cents)
{
    return threshold_next(current_cents);
}

bool whale_budget_exceeded(int64_t used_cents, int64_t budget_cents)
{
    if (budget_cents <= 0) return false;
    return used_cents >= budget_cents;
}

bool whale_balance_low(int64_t balance_cents, int64_t warn_cents)
{
    if (warn_cents <= 0) return false;
    return balance_cents <= warn_cents;
}

// ---- 熄屏档位 --------------------------------------------------------------

// 0 放在首尾两头:循环含义是「关闭 → 开一个档 → … → 再关掉」。
static const uint32_t SLEEP_OPTIONS[] = { 0, 100, 300, 1800 };

uint32_t whale_sleep_clamp(uint32_t sec)
{
    const size_t count = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);
    for (size_t i = 0; i < count; i++) {
        if (sec == SLEEP_OPTIONS[i]) return sec;
    }
    return WHALE_SLEEP_DEFAULT;
}

uint32_t whale_sleep_next(uint32_t current_sec)
{
    const size_t count = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);
    for (size_t i = 0; i < count; i++) {
        if (current_sec == SLEEP_OPTIONS[i]) {
            return SLEEP_OPTIONS[(i + 1) % count];
        }
    }
    return WHALE_SLEEP_DEFAULT;   // 不在档位上时先回到出厂默认,好过越界值一路亮到没电
}

const char *whale_sleep_name(uint32_t sec)
{
    switch (whale_sleep_clamp(sec)) {
    case 100:  return "100秒";
    case 300:  return "300秒";
    case 1800: return "30分钟";
    default:   return "关闭";
    }
}
