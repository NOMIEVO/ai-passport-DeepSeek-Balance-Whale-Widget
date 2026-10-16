// main/whale_model.h —— 小鲸鱼计费名片的纯逻辑层。
//
// 这里放的是可以脱离 ESP-IDF 与 LVGL、直接在主机上跑测试的东西:金额解析、DeepSeek
// 余额报文抽取、今日已用的记账规则、峰谷时段判定、配置项。界面(whale_ui.c)、网络
// (whale_net.c)与存储(whale_store.c)都只调用本文件的函数,不重复实现规则。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 金额统一用「分」做整数单位(1 元 = 100 分),避免浮点累计误差。
// DeepSeek 的余额报文给的是两位小数字符串,分是够用的最小粒度。
#define WHALE_DAY_LEN       11   // "YYYY-MM-DD" + 结尾 NUL
#define WHALE_SSID_MAX      33   // Wi-Fi SSID 上限 32 字节 + NUL
#define WHALE_PASS_MAX      65   // WPA2 口令上限 63 字节 + NUL
#define WHALE_KEY_MAX       129  // API Key 上限 128 字节 + NUL
#define WHALE_CURRENCY_LEN  8

// ---- 金额 ------------------------------------------------------------------

// 解析 "12.34" / "-0.5" / "12" 这样的十进制字符串。空串、非法字符、超出范围都算失败。
// 小数超过两位时四舍五入到分(余额报文只给两位,这里只是兜底)。
bool whale_money_parse(const char *text, int64_t *out_cents);

// 格式化为不带符号的 "12.34"。缓冲区不足时返回 0。
size_t whale_money_format(int64_t cents, char *out, size_t out_size);

// 格式化为带符号的 "-0.35" / "+1.00" / "0.00"。
size_t whale_money_format_signed(int64_t cents, char *out, size_t out_size);

// ---- 余额报文 --------------------------------------------------------------

typedef struct {
    int64_t total_cents;      // balance_infos[0].total_balance
    int64_t granted_cents;    // balance_infos[0].granted_balance(赠金,没有则 0)
    int64_t topped_cents;     // balance_infos[0].topped_up_balance(充值,没有则 0)
    bool available;           // is_available
    char currency[WHALE_CURRENCY_LEN];
} whale_balance_t;

// 从 https://api.deepseek.com/user/balance 的 JSON 响应里取值。
//
// 不引第三方 JSON 库:这份报文结构固定,用一个只认键名的最小扫描器即可,
// 好处是这段抽取逻辑能在主机测试里直接喂字符串验证(见 tests/test_whale_model.c)。
// out 为 NULL 时只做"能否解析出余额"的判断。
bool whale_balance_extract(const char *json, whale_balance_t *out);

// ---- 峰谷时段 --------------------------------------------------------------

typedef enum {
    WHALE_TARIFF_VALLEY = 0,  // 谷时(错峰优惠)
    WHALE_TARIFF_PEAK,        // 峰时
} whale_tariff_t;

// weekday 沿用 C 的 tm_wday:0=周日,1..5=周一..周五,6=周六。
// minute_of_day 为 0..1439。
//
// 时段口径与开源挂件 DeepSeek-Balance-Whale-Widget 一致:
// 工作日 09:00-12:00、14:00-18:00 为峰时,其余为谷时;周末全天谷时。
whale_tariff_t whale_tariff_at(int weekday, int minute_of_day);

// 距下一次峰谷切换还有多少分钟;当天不再切换(周末)返回 -1。
// 现在是峰时 → 返回到本时段结束的分钟数;谷时 → 返回到下一个峰时开始的分钟数。
int whale_tariff_minutes_to_change(int weekday, int minute_of_day);

// ---- 峰谷显示样式 ----------------------------------------------------------
//
// 对齐开源挂件峰谷模块的「显示样式」下拉(BUBBLE_PEAK_STYLE_OPTS):同一份峰谷判定,
// 换一套称呼。文案常量放在本文件(而不是界面)里,主机测试才能直接断言。
typedef enum {
    WHALE_PEAK_DEFAULT = 0,   // 高峰时段 / 空闲时段
    WHALE_PEAK_LIANGWEN,      // 梁文峰 / 梁文谷
    WHALE_PEAK_QIANGQIANG,    // !?峰峰?! / !?谷谷?!
    WHALE_PEAK_COUNT,         // 主数值改显倒计时(挂件的 nextpeak 模块)
    WHALE_PEAK_MINI,          // 峰 / 谷
    WHALE_PEAK_STYLE_COUNT,
} whale_peak_style_t;

// 菜单里显示的样式名。
const char *whale_peak_style_name(whale_peak_style_t style);

// 泡泡主数值的状态文字。**倒计时样式返回 NULL** —— 那种样式的数值是倒计时本身,
// 由界面自己拼,这里不参与。
const char *whale_peak_status_text(whale_peak_style_t style, bool is_peak);

// 在档位之间循环:默认 → 梁文峰谷 → !?强强?! → 倒计时 → 简洁 → 默认。
// 越界值回到默认,保证 NVS 里存到脏数据也能自愈。
whale_peak_style_t whale_peak_style_next(whale_peak_style_t style);

// 配置里样式存的是 int(NVS blob 直读),夹回合法档位再用,免得渲染走到越界分支。
whale_peak_style_t whale_peak_style_clamp(int value);

// ---- 今日已用记账 ----------------------------------------------------------
//
// 余额报文同时给了「赠金」和「充值」两个分项,而且 total = 赠金 + 充值 是接口自带的
// 恒等式。所以记账不能只看总额涨跌 —— 那样平台补偿到账的赠金会被当成用户自己充值,
// 而用户明明没掏钱。这里每次观测都留一份分项快照,用两个分项各自的增减判断发生了什么:
//   分项下降 → 花掉的钱   充值分项上升 → 自己充的钱   赠金分项上升 → 平台送的钱
typedef struct {
    char day[WHALE_DAY_LEN];   // 记账日(北京时间)
    int64_t opening_cents;     // 当日首次观测到的余额
    int64_t last_cents;        // 最近一次观测到的总余额
    int64_t last_granted_cents;// 最近一次观测到的赠金余额(下次判定消费/到账的基准)
    int64_t last_topped_cents; // 最近一次观测到的充值余额
    int64_t used_cents;        // 当日净消费(已扣掉平台返还的赠金,不会为负)
    int64_t topup_cents;       // 当日累计充值(只算自己掏的钱)
    int64_t granted_in_cents;  // 当日累计到账的赠金(平台返还 / 活动赠送)
    bool has_observation;      // 当日是否已有观测
} whale_ledger_t;

typedef enum {
    WHALE_LEDGER_FIRST = 0,    // 当日(或首次)建立基线,不计消费
    WHALE_LEDGER_UNCHANGED,    // 余额没变
    WHALE_LEDGER_SPEND,        // 余额净下降 → 记消费
    WHALE_LEDGER_TOPUP,        // 余额净上升,涨的是充值余额 → 自己充的钱
    WHALE_LEDGER_GRANT,        // 余额净上升,涨的是赠金 → 平台送的钱,冲抵当日已用
} whale_ledger_event_t;

void whale_ledger_reset(whale_ledger_t *ledger, const char *day);

// 观测一次余额。跨日(day 与账本里记录的不同)时自动以新余额重建基线。
// balance 必须带 granted/topped 分项 —— 少了它们就只能退回「只看总额」的粗口径。
// delta_cents 可为 NULL;它返回本次总余额变化量(负=花掉,正=多出来)。
whale_ledger_event_t whale_ledger_observe(whale_ledger_t *ledger, const char *day,
                                          const whale_balance_t *balance,
                                          int64_t *delta_cents);

// ---- 配置 ------------------------------------------------------------------

typedef struct {
    char ssid[WHALE_SSID_MAX];
    char password[WHALE_PASS_MAX];
    char api_key[WHALE_KEY_MAX];
    uint32_t refresh_sec;   // 自动刷新间隔(秒)
    bool sound_all;         // 音效总开关(关掉后任何提示音都不响)
    bool sound_press;       // 按键的「小黄鸭」按压音
    bool sound_spend;       // 消费时的金币声
    bool sound_connect;     // 联网/连接成功音
    int64_t budget_cents;   // 今日预算(分);0 = 不设预算
    int64_t warn_cents;     // 余额预警阈值(分);0 = 不预警
    int peak_style;         // 峰谷显示样式(whale_peak_style_t)
    // 空闲多久转入休眠(秒);0 = 不休眠。
    //
    // 刻意用 64 位:NVS 是按 blob 长度认版本的,而这个字段正好落在旧结构的尾部
    // padding 里(旧结构 264 字节),写成 32 位的话 sizeof 仍是 264,旧 blob 会被
    // 当成新结构读出一个随机熄屏值 —— 最坏的情况是永不休眠、把电放光。
    int64_t sleep_sec;
} whale_config_t;

void whale_config_init(whale_config_t *cfg);
// 有 SSID 也有 API Key 才算可联网。
bool whale_config_ready(const whale_config_t *cfg);
// 刷新间隔在预设档位之间循环:30 → 60 → 120 → 300 → 30。
uint32_t whale_refresh_next(uint32_t current_sec);

// 今日预算与余额预警共用一套档位:0(关) → ¥5 → ¥10 → ¥20 → ¥50 → 关。
// 单位是「分」,与余额一致,避免两套单位来回换算。
int64_t whale_budget_next(int64_t current_cents);
int64_t whale_warn_next(int64_t current_cents);

// 阈值生效的判定:阈值 <= 0 视为未设置,一律不触发。
bool whale_budget_exceeded(int64_t used_cents, int64_t budget_cents);
bool whale_balance_low(int64_t balance_cents, int64_t warn_cents);

// 空闲多久熄屏。0 = 一直亮着(不调暗也不休眠),其余按档位。
// 默认 300 秒:名片休眠后要按键唤醒并重连 Wi-Fi,太短的超时反而难用。
#define WHALE_SLEEP_OFF      0u      // 不熄屏
#define WHALE_SLEEP_DEFAULT  300u    // 出厂默认(5 分钟)

// 在档位之间循环:关闭 → 100 秒 → 300 秒 → 30 分钟 → 关闭。
uint32_t whale_sleep_next(uint32_t current_sec);

// 夹回合法档位:NVS 里若存了脏数据(比如手改过),退回出厂默认而不是把
// 它当成「永不休眠」把电放光 —— 越界值一律按无效处理。
uint32_t whale_sleep_clamp(uint32_t sec);

// 菜单里显示的档位名("关闭" / "100秒" / "300秒" / "30分钟")。
const char *whale_sleep_name(uint32_t sec);

#ifdef __cplusplus
}
#endif
