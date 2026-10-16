// 小鲸鱼计费名片模型的主机测试:金额解析/格式化、DeepSeek 余额报文抽取、峰谷时段、
// 今日已用的记账规则。不依赖 ESP-IDF/LVGL,编译方式见 tools/validate.sh。
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "whale_model.h"

// ---- 金额 ------------------------------------------------------------------

static void test_money_parse(void)
{
    int64_t cents = 0;

    assert(whale_money_parse("12.34", &cents) && cents == 1234);
    assert(whale_money_parse("0", &cents) && cents == 0);
    assert(whale_money_parse("9", &cents) && cents == 900);
    assert(whale_money_parse("-0.5", &cents) && cents == -50);
    assert(whale_money_parse("+1.25", &cents) && cents == 125);
    assert(whale_money_parse("9.935", &cents) && cents == 994);   // 四舍五入到分
    assert(whale_money_parse("9.934", &cents) && cents == 993);
    assert(whale_money_parse("  7 ", &cents) && cents == 700);
    assert(whale_money_parse("0.00", &cents) && cents == 0);

    // 非法输入必须失败,不能默默变成 0。
    assert(!whale_money_parse("", &cents));
    assert(!whale_money_parse("abc", &cents));
    assert(!whale_money_parse("12.34.5", &cents));
    assert(!whale_money_parse(".", &cents));
    assert(!whale_money_parse("1,23", &cents));
    assert(!whale_money_parse(NULL, &cents));
}

static void test_money_format(void)
{
    char buf[24];

    assert(whale_money_format(1234, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "12.34") == 0);
    assert(whale_money_format(0, buf, sizeof(buf)) == 4);
    assert(strcmp(buf, "0.00") == 0);
    assert(whale_money_format(5, buf, sizeof(buf)) == 4);
    assert(strcmp(buf, "0.05") == 0);
    assert(whale_money_format(-35, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "-0.35") == 0);

    assert(whale_money_format_signed(-35, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "-0.35") == 0);
    assert(whale_money_format_signed(100, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "+1.00") == 0);
    assert(whale_money_format_signed(0, buf, sizeof(buf)) == 4);
    assert(strcmp(buf, "0.00") == 0);

    // 缓冲区不足时不写半个数字。
    assert(whale_money_format(1234, buf, 4) == 0);
}

// ---- 余额报文 --------------------------------------------------------------

static void test_balance_extract(void)
{
    const char *payload =
        "{\"is_available\":true,"
        "\"balance_infos\":[{\"currency\":\"CNY\",\"total_balance\":\"9.93\","
        "\"granted_balance\":\"1.00\",\"topped_up_balance\":\"8.93\"}]}";

    whale_balance_t balance;
    assert(whale_balance_extract(payload, &balance));
    assert(balance.total_cents == 993);
    assert(balance.granted_cents == 100);
    assert(balance.topped_cents == 893);
    assert(balance.available);
    assert(strcmp(balance.currency, "CNY") == 0);

    // 键顺序变化、字段缺省都要能处理。
    const char *minimal = "{\"balance_infos\":[{\"total_balance\":12.5}]}";
    assert(whale_balance_extract(minimal, &balance));
    assert(balance.total_cents == 1250);
    assert(balance.granted_cents == 0);

    // 不可用账户:余额仍在,只是标记变了。
    const char *unavailable =
        "{\"is_available\":false,\"balance_infos\":[{\"total_balance\":\"0.00\"}]}";
    assert(whale_balance_extract(unavailable, &balance));
    assert(!balance.available && balance.total_cents == 0);

    // 没有余额字段必须报失败,不能当成 0 元显示。
    assert(!whale_balance_extract("{\"is_available\":true}", &balance));
    assert(!whale_balance_extract("not json", &balance));
    assert(!whale_balance_extract(NULL, &balance));

    // 只问"能不能解析"时允许不回写结果。
    assert(whale_balance_extract(payload, NULL));
}

// ---- 峰谷 ------------------------------------------------------------------

static void test_tariff(void)
{
    // 周一(weekday=1)。
    assert(whale_tariff_at(1, 8 * 60) == WHALE_TARIFF_VALLEY);
    assert(whale_tariff_at(1, 9 * 60) == WHALE_TARIFF_PEAK);
    assert(whale_tariff_at(1, 11 * 60 + 59) == WHALE_TARIFF_PEAK);
    assert(whale_tariff_at(1, 12 * 60) == WHALE_TARIFF_VALLEY);
    assert(whale_tariff_at(1, 14 * 60) == WHALE_TARIFF_PEAK);
    assert(whale_tariff_at(1, 17 * 60 + 59) == WHALE_TARIFF_PEAK);
    assert(whale_tariff_at(1, 18 * 60) == WHALE_TARIFF_VALLEY);

    // 周末全天谷时。
    assert(whale_tariff_at(0, 10 * 60) == WHALE_TARIFF_VALLEY);
    assert(whale_tariff_at(6, 10 * 60) == WHALE_TARIFF_VALLEY);
    assert(whale_tariff_minutes_to_change(0, 10 * 60) == -1);

    // 倒计时。
    assert(whale_tariff_minutes_to_change(1, 8 * 60) == 60);     // 距 09:00
    assert(whale_tariff_minutes_to_change(1, 11 * 60) == 60);    // 距 12:00
    assert(whale_tariff_minutes_to_change(1, 12 * 60 + 30) == 90); // 距 14:00
    assert(whale_tariff_minutes_to_change(1, 19 * 60) == -1);    // 今天不再切换
    assert(whale_tariff_minutes_to_change(1, 0) == 540);
}

// ---- 记账 ------------------------------------------------------------------

// 造一份余额报文截面。真实报文满足 total = 赠金 + 充值,这里也照这个构;
// 只有 test_ledger_mixed 的最后一例会故意破坏它,用来模拟报文自相矛盾。
static whale_balance_t mk_balance(int64_t total, int64_t granted, int64_t topped)
{
    whale_balance_t b;
    memset(&b, 0, sizeof(b));
    b.total_cents = total;
    b.granted_cents = granted;
    b.topped_cents = topped;
    b.available = true;
    memcpy(b.currency, "CNY", 4);
    return b;
}

static void test_ledger(void)
{
    whale_ledger_t ledger;
    whale_balance_t b;
    int64_t delta = 0;

    whale_ledger_reset(&ledger, "2026-10-10");
    assert(!ledger.has_observation);

    // 当日首次观测只建基线,不算消费;两个分项的基准也一并记下。
    b = mk_balance(1000, 0, 1000);
    assert(whale_ledger_observe(&ledger, "2026-10-10", &b, &delta) == WHALE_LEDGER_FIRST);
    assert(delta == 0);
    assert(ledger.opening_cents == 1000 && ledger.used_cents == 0);
    assert(ledger.last_granted_cents == 0 && ledger.last_topped_cents == 1000);

    // 余额下降 → 记消费。
    b = mk_balance(965, 0, 965);
    assert(whale_ledger_observe(&ledger, "2026-10-10", &b, &delta) == WHALE_LEDGER_SPEND);
    assert(delta == -35 && ledger.used_cents == 35);

    // 再降一次累计。
    b = mk_balance(900, 0, 900);
    assert(whale_ledger_observe(&ledger, "2026-10-10", &b, &delta) == WHALE_LEDGER_SPEND);
    assert(delta == -65 && ledger.used_cents == 100);

    // 余额上升、涨的是充值余额 → 记充值,不冲抵已记录的消费。
    b = mk_balance(1100, 0, 1100);
    assert(whale_ledger_observe(&ledger, "2026-10-10", &b, &delta) == WHALE_LEDGER_TOPUP);
    assert(delta == 200 && ledger.topup_cents == 200 && ledger.used_cents == 100);

    // 没变化。
    assert(whale_ledger_observe(&ledger, "2026-10-10", &b, &delta) == WHALE_LEDGER_UNCHANGED);
    assert(ledger.used_cents == 100 && ledger.topup_cents == 200);

    // 跨日:以新余额重建基线,当日累计清零。
    b = mk_balance(1080, 0, 1080);
    assert(whale_ledger_observe(&ledger, "2026-10-11", &b, &delta) == WHALE_LEDGER_FIRST);
    assert(ledger.used_cents == 0 && ledger.topup_cents == 0 && ledger.granted_in_cents == 0);
    assert(ledger.opening_cents == 1080);
    assert(strcmp(ledger.day, "2026-10-11") == 0);

    // day 为空串时不触发跨日(时间还没同步出来的降级路径)。
    b = mk_balance(1000, 0, 1000);
    assert(whale_ledger_observe(&ledger, "", &b, &delta) == WHALE_LEDGER_SPEND);
    assert(ledger.used_cents == 80);

    // 扣的是赠金余额(不是充值余额)时,一样算消费。
    whale_ledger_reset(&ledger, "2026-10-12");
    b = mk_balance(500, 500, 0);
    assert(whale_ledger_observe(&ledger, "2026-10-12", &b, &delta) == WHALE_LEDGER_FIRST);
    b = mk_balance(470, 470, 0);
    assert(whale_ledger_observe(&ledger, "2026-10-12", &b, &delta) == WHALE_LEDGER_SPEND);
    assert(delta == -30 && ledger.used_cents == 30);
}

// 赠金到账:必须和"充值"分开认,还要把当日已用冲抵回来。
// 客户报"余额怎么还越来越多了"就是这条规则缺失造成的 —— 平台把故障期的消费以赠金
// 形式返还,总余额涨回去了,界面上却一律显示"本次充值",已用额一分不减,两个数字打架。
static void test_ledger_grant(void)
{
    whale_ledger_t ledger;
    whale_balance_t b;
    int64_t delta = 0;

    // 起点:充值余额 1000,当天先花掉 300。
    whale_ledger_reset(&ledger, "2026-10-15");
    b = mk_balance(1000, 0, 1000);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_FIRST);
    b = mk_balance(700, 0, 700);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_SPEND);
    assert(ledger.used_cents == 300 && ledger.topup_cents == 0);

    // 平台把这 300 全额返成赠金:总余额回到 1000,涨的那 300 全落在赠金上。
    b = mk_balance(1000, 300, 700);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_GRANT);
    assert(delta == 300);
    assert(ledger.granted_in_cents == 300);
    assert(ledger.topup_cents == 0);   // 一分钱都不许记成充值
    assert(ledger.used_cents == 0);    // 已用被冲抵干净

    // 到账额超过当日已用时,已用归零,不记成负数。
    b = mk_balance(1300, 600, 700);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_GRANT);
    assert(ledger.granted_in_cents == 600);
    assert(ledger.used_cents == 0);

    // 对照:涨的是充值余额时只记充值,绝不碰当日已用。
    whale_ledger_reset(&ledger, "2026-10-15");
    b = mk_balance(1100, 0, 1100);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_FIRST);
    b = mk_balance(1050, 0, 1050);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_SPEND);
    assert(ledger.used_cents == 50);
    b = mk_balance(1150, 0, 1150);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_TOPUP);
    assert(ledger.topup_cents == 100 && ledger.used_cents == 50);
}

// 混合变化:两个分项朝相反方向走的时候,只看总额会得出错误结论。
static void test_ledger_mixed(void)
{
    whale_ledger_t ledger;
    whale_balance_t b;
    int64_t delta = 0;

    // 起点:赠金 200 + 充值 100 = 300。
    whale_ledger_reset(&ledger, "2026-10-15");
    b = mk_balance(300, 200, 100);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_FIRST);

    // 赠金 200 花光,同时自己充进来 400 → 总余额 500,净涨 200。
    // 只看总额会得出"充了 200"的错觉,实际上是花了 200 又充了 400。
    b = mk_balance(500, 0, 500);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_TOPUP);
    assert(delta == 200);
    assert(ledger.used_cents == 200);
    assert(ledger.topup_cents == 400);

    // "充 100 送 20":赠金是随充值附赠的,本质上是买来的,不能拿来冲抵当日已用。
    whale_ledger_reset(&ledger, "2026-10-15");
    b = mk_balance(1000, 0, 1000);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_FIRST);
    b = mk_balance(800, 0, 800);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_SPEND);
    assert(ledger.used_cents == 200);
    b = mk_balance(920, 20, 900);   // 充值 +100、赠金 +20,发生在同一轮
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_TOPUP);
    assert(delta == 120);
    assert(ledger.topup_cents == 100);
    assert(ledger.granted_in_cents == 20);
    assert(ledger.used_cents == 200);   // 已用不受影响

    // 分项之和与总额对不上(报文被截断 / 换了币种):退回只看总额的老口径 ——
    // 宁可粗一点,也不能拿一份错的拆分去记错账。
    whale_ledger_reset(&ledger, "2026-10-15");
    b = mk_balance(1000, 0, 1000);
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_FIRST);
    b = mk_balance(1200, 999, 500);   // 总额涨 200,两个分项加起来却差了 499
    assert(whale_ledger_observe(&ledger, "2026-10-15", &b, &delta) == WHALE_LEDGER_TOPUP);
    assert(delta == 200);
    assert(ledger.topup_cents == 200);      // 按总额记
    assert(ledger.granted_in_cents == 0);   // 分项整份丢弃
    assert(ledger.used_cents == 0);         // 没有被误冲抵
}

// ---- 配置 ------------------------------------------------------------------

static void test_config(void)
{
    whale_config_t cfg;
    whale_config_init(&cfg);
    assert(cfg.refresh_sec == 60);
    assert(cfg.sound_all && cfg.sound_press && cfg.sound_spend && cfg.sound_connect);
    assert(cfg.budget_cents == 0 && cfg.warn_cents == 0);   // 出厂不设预算/预警
    assert(!whale_config_ready(&cfg));

    snprintf(cfg.ssid, sizeof(cfg.ssid), "MyHome");
    assert(!whale_config_ready(&cfg));           // 只有 SSID 还不能联网
    snprintf(cfg.api_key, sizeof(cfg.api_key), "sk-test");
    assert(whale_config_ready(&cfg));

    assert(whale_refresh_next(30) == 60);
    assert(whale_refresh_next(60) == 120);
    assert(whale_refresh_next(120) == 300);
    assert(whale_refresh_next(300) == 30);
    assert(whale_refresh_next(7) == 60);          // 不在档位上 → 回到默认
}

static void test_thresholds(void)
{
    // 档位循环:关 → ¥5 → ¥10 → ¥20 → ¥50 → 关。
    assert(whale_budget_next(0) == 500);
    assert(whale_budget_next(500) == 1000);
    assert(whale_budget_next(1000) == 2000);
    assert(whale_budget_next(2000) == 5000);
    assert(whale_budget_next(5000) == 0);
    assert(whale_budget_next(777) == 500);        // 不在档位上 → 最小非零档
    assert(whale_warn_next(5000) == 0);

    // 0 表示未设置,一律不触发(否则"没设预算"会被当成"预算 0 已超")。
    assert(!whale_budget_exceeded(9999, 0));
    assert(!whale_balance_low(-100, 0));
    assert(whale_budget_exceeded(1000, 1000));    // 正好到阈值也算触达
    assert(!whale_budget_exceeded(999, 1000));
    assert(whale_balance_low(500, 1000));
    assert(!whale_balance_low(1001, 1000));
}

// 峰谷显示样式:文案与挂件的 BUBBLE_PEAK_STYLE_OPTS 一一对应。
static void test_peak_style(void)
{
    // 出厂是默认样式。
    whale_config_t cfg;
    whale_config_init(&cfg);
    assert(cfg.peak_style == WHALE_PEAK_DEFAULT);

    // 档位循环:默认 → 梁文峰谷 → !?强强?! → 倒计时 → 简洁 → 默认。
    assert(whale_peak_style_next(WHALE_PEAK_DEFAULT) == WHALE_PEAK_LIANGWEN);
    assert(whale_peak_style_next(WHALE_PEAK_LIANGWEN) == WHALE_PEAK_QIANGQIANG);
    assert(whale_peak_style_next(WHALE_PEAK_QIANGQIANG) == WHALE_PEAK_COUNT);
    assert(whale_peak_style_next(WHALE_PEAK_COUNT) == WHALE_PEAK_MINI);
    assert(whale_peak_style_next(WHALE_PEAK_MINI) == WHALE_PEAK_DEFAULT);

    // 夹取:NVS 里的脏值不能渲染到越界分支。
    assert(whale_peak_style_clamp(0) == WHALE_PEAK_DEFAULT);
    assert(whale_peak_style_clamp(4) == WHALE_PEAK_MINI);
    assert(whale_peak_style_clamp(-1) == WHALE_PEAK_DEFAULT);
    assert(whale_peak_style_clamp(99) == WHALE_PEAK_DEFAULT);
    assert(whale_peak_style_next((whale_peak_style_t)99) == WHALE_PEAK_LIANGWEN);

    // 状态文字:峰 / 谷两套。
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_DEFAULT, true), "高峰时段") == 0);
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_DEFAULT, false), "空闲时段") == 0);
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_LIANGWEN, true), "梁文峰") == 0);
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_LIANGWEN, false), "梁文谷") == 0);
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_QIANGQIANG, true), "!?峰峰?!") == 0);
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_QIANGQIANG, false), "!?谷谷?!") == 0);
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_MINI, true), "峰") == 0);
    assert(strcmp(whale_peak_status_text(WHALE_PEAK_MINI, false), "谷") == 0);
    // 倒计时样式没有状态文字:数值是倒计时本身,界面自己拼。
    assert(whale_peak_status_text(WHALE_PEAK_COUNT, true) == NULL);
    assert(whale_peak_status_text(WHALE_PEAK_COUNT, false) == NULL);

    // 菜单里的样式名。
    assert(strcmp(whale_peak_style_name(WHALE_PEAK_DEFAULT), "默认") == 0);
    assert(strcmp(whale_peak_style_name(WHALE_PEAK_LIANGWEN), "梁文峰谷") == 0);
    assert(strcmp(whale_peak_style_name(WHALE_PEAK_QIANGQIANG), "!?强强?!") == 0);
    assert(strcmp(whale_peak_style_name(WHALE_PEAK_COUNT), "倒计时") == 0);
    assert(strcmp(whale_peak_style_name(WHALE_PEAK_MINI), "简洁") == 0);
    // 越界值也要有名字可显示,不能返回 NULL 让界面拿到空标签。
    assert(strcmp(whale_peak_style_name((whale_peak_style_t)42), "默认") == 0);
}

// 熄屏档位:关闭 → 100 秒 → 300 秒 → 30 分钟。
static void test_sleep(void)
{
    whale_config_t cfg;
    whale_config_init(&cfg);
    assert(cfg.sleep_sec == WHALE_SLEEP_DEFAULT);
    assert(cfg.sleep_sec == 300);

    assert(whale_sleep_next(WHALE_SLEEP_OFF) == 100);
    assert(whale_sleep_next(100) == 300);
    assert(whale_sleep_next(300) == 1800);
    assert(whale_sleep_next(1800) == WHALE_SLEEP_OFF);

    // 夹取最要紧:NVS 里若出现脏值,必须退回一个会熄屏的档位,
    // 绝不能落到 0(= 永不休眠)上去。
    assert(whale_sleep_clamp(0) == 0);      // 用户显式选的「关闭」要保住
    assert(whale_sleep_clamp(100) == 100);
    assert(whale_sleep_clamp(300) == 300);
    assert(whale_sleep_clamp(1800) == 1800);
    assert(whale_sleep_clamp(150) == WHALE_SLEEP_DEFAULT);
    assert(whale_sleep_clamp(1) == WHALE_SLEEP_DEFAULT);
    assert(whale_sleep_clamp(UINT32_MAX) == WHALE_SLEEP_DEFAULT);
    assert(whale_sleep_next(150) == WHALE_SLEEP_DEFAULT);   // 不在档位 → 回默认

    assert(strcmp(whale_sleep_name(0), "关闭") == 0);
    assert(strcmp(whale_sleep_name(100), "100秒") == 0);
    assert(strcmp(whale_sleep_name(300), "300秒") == 0);
    assert(strcmp(whale_sleep_name(1800), "30分钟") == 0);
    assert(whale_sleep_name(42) != NULL);
}

int main(void)
{
    test_money_parse();
    test_money_format();
    test_balance_extract();
    test_tariff();
    test_ledger();
    test_ledger_grant();
    test_ledger_mixed();
    test_config();
    test_thresholds();
    test_peak_style();
    test_sleep();
    printf("test_whale_model: PASS\n");
    return 0;
}
