// 配网表单解析的主机测试:URL 解码、字段覆盖语义、刷新间隔校验、ASCII 预览。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "whale_form.h"

static void test_url_decode(void)
{
    char buf[64];

    assert(whale_url_decode("MyHome", buf, sizeof(buf)) == 6);
    assert(strcmp(buf, "MyHome") == 0);

    assert(whale_url_decode("a+b", buf, sizeof(buf)) == 3);
    assert(strcmp(buf, "a b") == 0);

    // 中文 SSID:%E4%B8%AD → UTF-8 的三个字节。
    assert(whale_url_decode("%E4%B8%AD", buf, sizeof(buf)) == 3);
    assert((unsigned char)buf[0] == 0xE4 && (unsigned char)buf[1] == 0xB8 &&
           (unsigned char)buf[2] == 0xAD);

    // 非法转义原样保留,不能把整条字段吃掉。
    assert(whale_url_decode("a%zz", buf, sizeof(buf)) == 4);
    assert(strcmp(buf, "a%zz") == 0);

    // 缓冲区不足时截断但不越界。
    assert(whale_url_decode("abcdef", buf, 4) == 6);
    assert(strcmp(buf, "abc") == 0);

    assert(whale_url_decode(NULL, buf, sizeof(buf)) == 0);
}

static void test_form_parse(void)
{
    whale_form_t form;
    memset(&form, 0, sizeof(form));

    const char *body = "ssid=MyHome&pass=secret%20123&key=sk-test-key&refresh=120";
    assert(whale_form_parse(body, &form));
    assert(form.has_ssid && strcmp(form.ssid, "MyHome") == 0);
    assert(form.has_password && strcmp(form.password, "secret 123") == 0);
    assert(form.has_api_key && strcmp(form.api_key, "sk-test-key") == 0);
    assert(form.has_refresh && form.refresh_sec == 120);

    // 只提交 API Key:其余字段保持原值(局部更新的语义)。
    memset(&form, 0, sizeof(form));
    snprintf(form.ssid, sizeof(form.ssid), "KeepMe");
    assert(whale_form_parse("key=sk-only", &form));
    assert(form.has_api_key && !form.has_ssid);
    assert(strcmp(form.ssid, "KeepMe") == 0);
    assert(form.refresh_sec == 0);

    // 刷新间隔越界时不接受:整条请求没有可识别字段 → 解析失败,UI 上给出"无效"提示。
    memset(&form, 0, sizeof(form));
    assert(!whale_form_parse("refresh=5", &form));
    assert(!form.has_refresh);
    memset(&form, 0, sizeof(form));
    assert(!whale_form_parse("refresh=99999", &form));
    assert(!form.has_refresh);
    memset(&form, 0, sizeof(form));
    assert(!whale_form_parse("refresh=abc", &form));
    assert(!form.has_refresh);
    // 合法间隔仍然接受。
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse("refresh=15", &form) && form.refresh_sec == 15);
    assert(whale_form_parse("refresh=3600", &form) && form.refresh_sec == 3600);

    // 空口令是合法场景(开放网络)。
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse("ssid=OpenNet&pass=&key=sk-1", &form));
    assert(form.has_password && form.password[0] == '\0');
    assert(form.has_ssid);

    // 没有可识别字段 → 失败,不写脏数据。
    memset(&form, 0, sizeof(form));
    assert(!whale_form_parse("foo=bar", &form));
    assert(!whale_form_parse("", &form));
    assert(!whale_form_parse(NULL, &form));

    // 大段垃圾不能把栈缓冲区撑爆。
    char junk[2048];
    memset(junk, 'x', sizeof(junk) - 1);
    junk[sizeof(junk) - 1] = '\0';
    memcpy(junk, "key=", 4);
    memset(&form, 0, sizeof(form));
    assert(!whale_form_parse(junk, &form));
}

static void test_ascii_preview(void)
{
    char buf[32];

    assert(whale_text_ascii_preview("MyHome", buf, sizeof(buf)) == 6);
    assert(strcmp(buf, "MyHome") == 0);

    // 非 ASCII 字节统一显示成 '?':字体子集覆盖不了任意汉字,这是明确的降级。
    assert(whale_text_ascii_preview("\xE4\xB8\xAD\xE6\x96\x87", buf, sizeof(buf)) == 6);
    assert(strcmp(buf, "??????") == 0);

    assert(whale_text_ascii_preview("abc", buf, 3) == 2);
    assert(strcmp(buf, "ab") == 0);
    assert(whale_text_ascii_preview("", buf, sizeof(buf)) == 0);
}

static void test_html_escape(void)
{
    char buf[128];

    assert(whale_html_escape("MyHome", buf, sizeof(buf)) == 6);
    assert(strcmp(buf, "MyHome") == 0);

    // SSID 是用户文本;这几个字符不转义会直接破页面结构。
    assert(whale_html_escape("a&b<c>d\"e'f", buf, sizeof(buf)) ==
           strlen("a&amp;b&lt;c&gt;d&quot;e&#39;f"));
    assert(strcmp(buf, "a&amp;b&lt;c&gt;d&quot;e&#39;f") == 0);

    // dst=NULL 只算长度,与实际写入长度一致(调用方据此预留缓冲)。
    const size_t need = whale_html_escape("&<>\"'", NULL, 0);
    assert(need == strlen("&amp;&lt;&gt;&quot;&#39;"));
    assert(whale_html_escape("&<>\"'", buf, sizeof(buf)) == need);

    // 缓冲区不足时安全截断且一定结尾 NUL。
    whale_html_escape("&<>\"'", buf, 6);
    assert(strlen(buf) == 5);
    assert(whale_html_escape("", buf, sizeof(buf)) == 0);
    assert(buf[0] == '\0');
}

static void test_ap_options(void)
{
    whale_ap_option_t aps[3] = {
        { .ssid = "Home-2.4G", .rssi = -48, .secured = true },
        { .ssid = "Guest \"A\"<&>", .rssi = -71, .secured = false },
        { .ssid = "", .rssi = -60, .secured = true },
    };
    char buf[512];

    // dst=NULL:只报需要的长度。
    const size_t need = whale_form_ap_options(aps, 3, NULL, 0);
    assert(need > 0);

    const size_t written = whale_form_ap_options(aps, 3, buf, sizeof(buf));
    assert(written == need);
    assert(written < sizeof(buf));
    // label 里必须带上 SSID 本体:原生 <select> 收起后只看得到 label。
    assert(strstr(buf, "<option value=\"Home-2.4G\">Home-2.4G · -48dBm · 需要密码</option>") != NULL);
    // 特殊字符在 value 与 label 里都必须是实体,不能出现裸 '<' / '&'。
    assert(strstr(buf, "value=\"Guest &quot;A&quot;&lt;&amp;&gt;\"") != NULL);
    assert(strstr(buf, "&quot;A&quot;&lt;&amp;&gt; · -71dBm · 开放") != NULL);
    // 空 SSID 的记录被跳过。
    assert(strstr(buf, "<option value=\"\">") == NULL);

    // 小缓冲:整条 option 要么完整写入要么不写,绝不写出残缺标签,且结果永远是合法 C 串。
    char small[32];
    const size_t small_written = whale_form_ap_options(aps, 1, small, sizeof(small));
    assert(small_written < need);
    assert(strlen(small) == small_written || small_written == 0);
    assert(small_written == 0 || strstr(small, "</option>") != NULL);

    assert(whale_form_ap_options(NULL, 3, buf, sizeof(buf)) == 0);
    whale_ap_option_t empty[2] = { 0 };
    assert(whale_form_ap_options(empty, 2, buf, sizeof(buf)) == 0);
}

static void test_ssid_choice(void)
{
    whale_form_t form;
    char out[64];

    // 下拉框选中的 SSID 直接生效。
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse("ssid=Home-2.4G", &form));
    assert(whale_form_resolve_ssid(&form, out, sizeof(out)));
    assert(strcmp(out, "Home-2.4G") == 0);

    // 手写框优先:隐藏网络场景下用户一定是在手写框里填的。
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse("ssid=Home-2.4G&ssid_manual=HiddenNet", &form));
    assert(whale_form_resolve_ssid(&form, out, sizeof(out)));
    assert(strcmp(out, "HiddenNet") == 0);

    // 选了"手写名称"哨兵但手写框空着 → 等于没选,调用方应保留原 SSID。
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse("ssid=" WHALE_FORM_SSID_MANUAL "&ssid_manual=", &form));
    assert(!whale_form_resolve_ssid(&form, out, sizeof(out)));

    // 下拉框停在占位项(空 value)+ 手写框也空 → 同样算没选。
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse("ssid=&ssid_manual=", &form));
    assert(!whale_form_resolve_ssid(&form, out, sizeof(out)));

    // 只改 API Key 时压根不会带 SSID 字段。
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse("key=sk-only", &form));
    assert(!whale_form_resolve_ssid(&form, out, sizeof(out)));

    // 极端输入:超长手写值被截进 SSID 上限,不越界且结果仍是合法 C 串。
    char long_body[300];
    const size_t head = strlen("ssid_manual=");
    memset(long_body, 'a', sizeof(long_body) - 1);
    long_body[sizeof(long_body) - 1] = '\0';
    memcpy(long_body, "ssid_manual=", head);
    memset(&form, 0, sizeof(form));
    assert(whale_form_parse(long_body, &form));
    assert(form.has_ssid_manual);
    assert(strlen(form.ssid_manual) == WHALE_SSID_MAX - 1);
    assert(whale_form_resolve_ssid(&form, out, sizeof(out)));
    assert(strlen(out) == WHALE_SSID_MAX - 1);        // 完整的都能放下
    char tight[8];
    assert(whale_form_resolve_ssid(&form, tight, sizeof(tight)));
    assert(strlen(tight) == sizeof(tight) - 1);       // 小缓冲按可用空间截

    assert(!whale_form_resolve_ssid(NULL, out, sizeof(out)));
}

int main(void)
{
    test_url_decode();
    test_form_parse();
    test_ascii_preview();
    test_html_escape();
    test_ap_options();
    test_ssid_choice();
    printf("test_whale_form: PASS\n");
    return 0;
}
