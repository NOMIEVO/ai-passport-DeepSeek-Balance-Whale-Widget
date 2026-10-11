// main/whale_ui.h —— 小鲸鱼计费名片的界面。
//
// 这是应用自己画的界面:挂件页(余额泡泡 + 代码绘制的小鲸鱼)、设置页、配网页三屏,
// 与基线 demo 的测试菜单/像素外壳没有任何共用代码(不 include ui_pixel.h)。
//
// 所有中文文案都以字面量写在本文件里(字模生成脚本只扫这里),外部只传枚举、数值和
// ASCII 文本 —— 动态文本(SSID、IP、时间)一律按 ASCII 预览显示,字体子集才盖得住。
#pragma once

#include "whale_model.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WHALE_PAGE_HOME = 0,
    WHALE_PAGE_SETTINGS,
    WHALE_PAGE_PROVISION,
} whale_page_t;

// 挂件页底部状态行。UI 负责把它翻成中文。
typedef enum {
    WHALE_ST_UNCONFIGURED = 0,   // 还没配网
    WHALE_ST_CONNECTING,         // 正在连 Wi-Fi
    WHALE_ST_ONLINE,             // 已连接
    WHALE_ST_REFRESHING,         // 正在取余额
    WHALE_ST_AUTH_FAILED,        // API Key 无效(401/403)
    WHALE_ST_NETWORK_FAILED,     // 网络不通 / 超时
    WHALE_ST_NO_TIME,            // 时间没同步出来(峰谷降级)
    WHALE_ST_BUDGET_HIT,         // 今日已用触达预算
    WHALE_ST_BALANCE_LOW,        // 余额低于预警阈值
} whale_status_t;

// 配网页状态行。
typedef enum {
    WHALE_PV_WAITING = 0,
    WHALE_PV_SAVED,
    WHALE_PV_CONNECTING,
    WHALE_PV_ONLINE,
    WHALE_PV_FAILED,
} whale_prov_status_t;

// 菜单项。对照开源挂件的主菜单:配网、用量设置(预算 / 预警)、音效与提示。
typedef enum {
    WHALE_SET_PROVISION = 0,   // 配网与 API Key(长按 = 清除已存凭据)
    WHALE_SET_REFRESH,         // 余额刷新间隔
    WHALE_SET_BUDGET,          // 今日预算
    WHALE_SET_WARN,            // 余额预警
    WHALE_SET_PEAK_STYLE,      // 峰谷显示样式
    WHALE_SET_SOUND_ALL,       // 音效总开关
    WHALE_SET_SOUND_PRESS,     // 按压音效(小黄鸭)
    WHALE_SET_SOUND_SPEND,     // 消费金币声
    WHALE_SET_SOUND_CONNECT,   // 连接提示音
    WHALE_SET_SLEEP,           // 熄屏时间(0 = 一直亮)
    WHALE_SET_LEDGER,          // 今日记账清零
    WHALE_SET_BACK,            // 返回挂件页
    WHALE_SET_COUNT,
} whale_setting_t;

// 泡泡序列:挂件里「点按角色推进队列」,走到最后一项再按一次收起泡泡。
#define WHALE_VIEW_COUNT 4     // 0 余额 / 1 今日已用 / 2 峰谷 / 3 连接

typedef struct {
    int battery;                 // -1 表示电量不可用
    whale_status_t status;
    bool has_balance;
    int64_t balance_cents;
    int64_t today_used_cents;
    int64_t last_delta_cents;
    bool tariff_known;
    whale_tariff_t tariff;
    int minutes_to_change;       // -1 = 当天不再切换
    whale_peak_style_t peak_style;  // 峰谷显示样式
    int view;                    // 0..WHALE_VIEW_COUNT-1
    char ssid_preview[34];       // ASCII 预览
    char key_hint[24];           // ASCII,形如 "…1a2b" 或 "none"
    char ip[18];                 // ASCII,形如 "192.168.1.5"
    char updated[12];            // ASCII,形如 "12:34" 或 "--:--"
    uint32_t refresh_sec;        // 自动刷新间隔(秒)
    bool bubble_open;            // false = 泡泡收起,只留小鲸鱼
} whale_ui_state_t;

// 创建三个界面。失败返回 false(调用方应停止启动)。
bool whale_ui_init(void);

void whale_ui_show_page(whale_page_t page);
whale_page_t whale_ui_current_page(void);

// 刷新挂件页内容。可从任意任务调用(内部持有 LVGL 锁)。
void whale_ui_update(const whale_ui_state_t *state);

// 设置页:选中项 + 各项当前值。
void whale_ui_settings_update(whale_setting_t selected, uint32_t refresh_sec,
                              int64_t budget_cents, int64_t warn_cents,
                              whale_peak_style_t peak_style, uint32_t sleep_sec,
                              bool sound_all, bool sound_press, bool sound_spend,
                              bool sound_connect);

// 配网页:状态 + 热点信息(热点名/口令均为 ASCII)。
void whale_ui_provision_update(whale_prov_status_t status, const char *ap_ssid,
                               const char *ap_password, const char *ip);

// 消费动画:一枚金币从泡泡下方掉到小鲸鱼身上。
void whale_ui_coin_drop(void);

// Q 弹:按参考挂件的「按压 Q 弹」参数走一次压扁 → 回弹(角色图 + 气泡 + 泡泡文本
// 整块一起动)。按键反馈时调用;拿不到 LVGL 锁就直接跳过,不影响别的功能。
void whale_ui_boing(void);

// 读电量计并重画右上角;读不到时显示 "--"。
void whale_ui_battery_refresh(void);

// 深睡前在挂件页状态行给出提示(深睡后屏幕是黑的,提示只在入睡瞬间可见)。
void whale_ui_sleep_notice(void);

#ifdef __cplusplus
}
#endif
