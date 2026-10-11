// main/whale_form.h —— 配网网页表单的解析(纯逻辑,可在主机上验证)。
//
// 浏览器把表单以 application/x-www-form-urlencoded 提交过来:
//   ssid=MyHome&pass=1234&key=sk-xxxx&refresh=60
// 解析放在这里而不是 HTTP 回调里,一是回调里不该做重活,二是这段规则能直接跑主机测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "whale_model.h"

#ifdef __cplusplus
extern "C" {
#endif

// 网页上下拉框"手写名称"那一项的哨兵值。选它表示用户要走下面的手写输入框。
// 真有一张热点叫这个名字的概率可以忽略,而且手写框永远优先,不会被误伤。
#define WHALE_FORM_SSID_MANUAL "__manual__"

typedef struct {
    char ssid[WHALE_SSID_MAX];        // 下拉框选中的 SSID
    char ssid_manual[WHALE_SSID_MAX]; // 手写输入框(优先级高于下拉框)
    char password[WHALE_PASS_MAX];
    char api_key[WHALE_KEY_MAX];
    uint32_t refresh_sec;
    bool has_ssid;
    bool has_ssid_manual;
    bool has_password;
    bool has_api_key;
    bool has_refresh;
} whale_form_t;

// URL 解码:%XX 还原成字节,+ 还原成空格。返回写入 dst 的字节数(不含结尾 NUL)。
// 缓冲区不足时按能放下的部分截断;dst 为 NULL 时只计算解码后的长度。
size_t whale_url_decode(const char *src, char *dst, size_t dst_size);

// 扫描到的一个热点(给网页下拉候选用)。
typedef struct {
    char ssid[WHALE_SSID_MAX];
    int rssi;          // dBm,负数,越大越强
    bool secured;      // 是否需要口令
} whale_ap_option_t;

// HTML 转义:把 & < > " ' 换成实体。
// SSID 是用户文本,可能带这些字符;不转义就会把表单页面搞坏(甚至注入)。
// 返回写入 dst 的字节数(不含结尾 NUL),dst 为 NULL 时只计算需要的长度。
size_t whale_html_escape(const char *src, char *dst, size_t dst_size);

// 把热点列表渲染成一组 <option value="SSID">SSID · 强度 · 加密</option>。
// 注意 label 里必须带 SSID 本身:原生 <select> 收起后只显示 label,
// 光写 "-48dBm" 用户根本看不出自己在选哪张网(datalist 那版就是这个毛病)。
// 返回应该写入的总长度(不含结尾 NUL);大于等于 dst_size 表示装不下,
// 调用方据此少列几条。
size_t whale_form_ap_options(const whale_ap_option_t *aps, size_t count, char *dst,
                             size_t dst_size);

// 从一次提交里挑出最终要用的 SSID:手写框优先,其次是下拉框,哨兵值与空串都算"没选"。
// 返回 false 表示这次提交没给出 SSID —— 调用方应保留原名,别把已保存的配置冲掉。
bool whale_form_resolve_ssid(const whale_form_t *form, char *out, size_t out_size);

// 解析整个表单体。out 里的字段只在表单里出现时才被改写,便于"只改 API Key"这类提交。
// 返回 false 表示没有解析出任何可识别字段。
bool whale_form_parse(const char *body, whale_form_t *out);

// 把任意字节串压成可显示的 ASCII 预览:非 ASCII 字节替换成 '?'。
// 界面字体是按固定文案做的子集,覆盖不了任意汉字,所以 SSID 这类"用户自带文本"
// 只做 ASCII 预览 —— 这是明确的取舍,不是悄悄丢字(见应用文档的字体章节)。
size_t whale_text_ascii_preview(const char *in, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
