// main/whale_ui.c —— 三屏界面的实现(挂件 / 菜单 / 配网)。
//
// 视觉照抄开源挂件 DeepSeek-Balance-Whale-Widget 的泡泡:
//   · 泡泡是白底 + #203170 粗描边的圆润色块,底下挂两个依次变小的圆点当"尾巴";
//   · 泡泡内是居中三行:标签(小)、主数值(大)、补充(浅灰蓝);
//   · 主色 #203170 是挂件自己的品牌蓝,文字用 #536BA9 / #9FB0D9 两档;
//   · 泡泡下面是角色图:直接嵌入参考挂件的原版资源 assets/img/DSniang1.png
//     (610x610 透明底 cut-out,经 tools/gen_whale_img.py 缩成 116x116 的
//     RGB565A8 C 数组),水平居中放在角色区,底边与状态行留 4 px 净空。
//
// 竖屏 240x320 的纵向分配(改布局只改这一组常量):
//   6..24     顶栏:左标题,右电量
//   26..120   泡泡主体(白底蓝边)
//   121..156  泡泡尾巴:两个递减圆点
//   123..262  角色图(带轻微上下浮动;上沿会探进泡泡尾巴区域,透明区不挡字)
//   266..284  状态行(左侧挂峰谷徽章)
//   288..306  操作提示
// 屏幕四角被 BSP 裁成半径 30 的圆角,故内容横向保持在 18..222 以内。
#include "whale_ui.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

// 应用自带的中文字模(assets/fonts/whale_ui_font_*.c),含界面用到的全部汉字与符号。
LV_FONT_DECLARE(whale_ui_font_16);
LV_FONT_DECLARE(whale_ui_font_24);
// 角色图(assets/img/whale_dsnang_img.c,由 tools/gen_whale_img.py 生成)。
LV_IMAGE_DECLARE(whale_dsnang_img);
// 气泡整条图(assets/img/whale_bubble_img.c,由 tools/gen_whale_bubble.py 生成)。
LV_IMAGE_DECLARE(whale_bubble_img);

#define SCREEN_W 240
#define SCREEN_H 320

#define MARGIN      18
#define CONTENT_W   (SCREEN_W - MARGIN * 2)

#define TOP_Y       6
#define TOP_H       18
#define BATT_W      58

// 顶栏右侧的电量块是一个宽 58 的小容器:左边 32 px 放百分比数字,右边画一枚
// 电池(外框 18x11 + 2x5 的正极帽,内部按容量填色)。容器坐标是相对的,
// 改这里就能整体挪位置,不用去动 make_label 的 x。
#define BATT_NUM_W      32
#define BATT_ICON_X     36
#define BATT_ICON_W     18
#define BATT_ICON_H     11
#define BATT_ICON_Y     ((TOP_H - BATT_ICON_H) / 2)   // 垂直居中于顶栏
#define BATT_CAP_W      2
#define BATT_CAP_H      5
// 填色条相对外框内缩 1 px 描边 + 1 px 间隙,按百分比线性占满。
#define BATT_FILL_X     (BATT_ICON_X + 2)
#define BATT_FILL_Y     (BATT_ICON_Y + 2)
#define BATT_FILL_W     (BATT_ICON_W - 4)
#define BATT_FILL_H     (BATT_ICON_H - 4)

// ---- 气泡:照参考挂件的 SVG 几何整条生成,不再用圆角矩形拼 ----------------
//
// 参考挂件的泡泡是**真椭圆**(中心 454,247 / rx373 ry232)+ 一条斜 10° 的半椭圆
// 尾巴 + 两个小椭圆泡泡,统一白底 + #203170 描边 18 —— 之前用圆角矩形拼出来的是
// "胶囊",跟 assets/DSniang02.png 里的样子对不上。LVGL 这边没开矢量引擎,画不出
// 椭圆,所以把整条按同样几何 raster 成 RGB565A8 图片(tools/gen_whale_bubble.py),
// 屏幕端只做贴图 + 叠label。改宽度/等比缩放都重跑那个脚本。
#define BUB_IMG_W   194        // 大椭圆在屏幕上的宽度(同时也是整条图的缩放基准)
#define BUB_IMG_H   174        // 整条图(含尾巴与两个小泡泡)的内容框高
#define BUB_IMG_X   16         // 内容框左上角;大椭圆本体落在 18..212
#define BUB_IMG_Y   28         // 内容框左上角 y;大椭圆本体落在 30..151

// 大椭圆的几何:三行文本要落在白底里,扁椭圆越靠上下越窄,行位由中心推算。
#define BUB_ELL_CX  115        // 大椭圆中心 x
#define BUB_ELL_CY  91         // 大椭圆中心 y
#define BUB_ELL_RX  97
#define BUB_ELL_RY  60
// 行心相对椭圆中心 ±32:再往下到 ±41(行底边缘)时白底只剩 142 px,
// 140 是这个 width 下不会压到描边的最大文本宽度。
#define BUB_TEXT_W  140
#define BUB_TEXT_X  (BUB_ELL_CX - BUB_TEXT_W / 2)
#define BUB_ROW1_Y  50         // 标签行(font16,高 18)
#define BUB_ROW2_Y  76         // 主数值(font24,高 30)
#define BUB_ROW3_Y  114        // 补充行(font16,高 18)

#define BUB_LINE    3          // 配网卡沿用圆角矩形,描边还是 3

// 角色图的底边固定在 262,给下面的状态行(266 起)留 4 px 净空。
// 纵向空间有限:图变高只能往上顶,顶到泡泡尾巴区域时靠截图预览确认构图。
#define WHALE_BOTTOM 262

#define WHALE_X     0
#define WHALE_Y     (WHALE_BOTTOM - WHALE_IMG_S)
#define WHALE_W     240
#define WHALE_H     WHALE_IMG_S

// 角色图尺寸,与 assets/img/whale_dsnang_img.c 里的图一致(改图就要改这里)。
#define WHALE_IMG_S 139
// 离右边框留一点距离,别贴在屏幕边缘上。
#define WHALE_IMG_RIGHT_GAP 6
#define WHALE_IMG_X (WHALE_W - WHALE_IMG_S - WHALE_IMG_RIGHT_GAP)
// 消费时金币从泡泡尾巴方向掉下来;角色已挪到中间,金币改落在左侧空档,
// 免得掉在角色脸上。
#define COIN_X      64

// ---- Q 弹(squash & stretch):照参考挂件的「按压 Q 弹」权威参数 --------------
//
// 参考挂件:`.dshwv-body{transform-origin:50% 100%;transition:transform .22s
// cubic-bezier(.34,1.56,.64,1)}`,按压态 `scaleY(.88) scaleX(1.05)`。
//
// 舞台里**只有角色**:气泡是另一层,不跟着 Q 弹(实测气泡跟着缩会晃得厉害,
// 而且舞台一变大,每帧重绘面积翻倍,屏刷新跟不上直接卡成幻灯片)。
// 现在缩放的只有 240 × 139 的角色区,刷新压力小一半。
#define STAGE_Y0        WHALE_Y              // 舞台上边 = 角色图顶(123)
#define STAGE_W         240
#define STAGE_H         WHALE_IMG_S          // 139:下边压在角色脚底 262
#define STAGE_PIVOT_X   (STAGE_W / 2)        // 对应 transform-origin 的 50%
#define STAGE_PIVOT_Y   STAGE_H              // 对应 100%(屏幕 y = 262)

// 气泡单独一层(图像 + 三行文本),自己有进出场动画,不参与 Q 弹。
#define BUB_LAYER_X     0
#define BUB_LAYER_Y     BUB_IMG_Y            // 28:与气泡图内容框对齐
#define BUB_LAYER_W     240
#define BUB_LAYER_H     BUB_IMG_H            // 174 → 28..202,盖住三行文本(最底 132)

// 气泡进出场:一律缓进缓出,方向是「下进下出」——
//   消失:由上至下,边往下沉边淡出(走到 +DY);
//   出现:由下至上,从下方 +DY 浮到位边淡入。
// 切换内容时这两段串起来:旧的先沉下去消失,换好字,新的再从下面浮上来。
#define BUB_OUT_MS      300                  // 向下消失(慢一点,看得清)
#define BUB_IN_MS       340                  // 由下至上浮现
#define BUB_SLIDE_DY    32                   // 位移量

// LVGL 的 transform_scale 以 1/256 为单位:256 = 1.0。
#define SQ_SCALE_1      256
#define SQ_PRESS_Y      225                  // scaleY(.88) → 0.88 × 256 ≈ 225
#define SQ_IDLE_Y       248                  // 常驻呼吸只压到 .97,免得整屏晃
// scaleX 与 scaleY 互补(参考 .88 ↔ 1.05):sx - 1 = 5/12 × (1 - sy),压得越扁越往外撑。
#define SQ_X_OF(sy)     (SQ_SCALE_1 + ((SQ_SCALE_1 - (sy)) * 5) / 12)

// 参考给的 .22s 是按 60 fps 网页定的;名片这块 SPI 屏刷新跟不上,同样的曲线
// 放到 110 ms 上只能出两三帧,看着就是「闪一下」+ 卡顿。所以整体放慢到
// 压 180 + 回弹 420(约 0.6 s),每帧位移更小,视觉上才连贯。
#define SQ_PRESS_MS     180                  // 压下去(半程)
#define SQ_BACK_MS      420                  // 回弹,仍是 cubic-bezier(.34,1.56,.64,1)
#define SQ_IDLE_MS      2400                 // 常驻呼吸的半周期(放慢,少占刷新)

#define BADGE_W     44
#define STATUS_Y    266
#define HINT_Y      288
#define LINE_H      18

// 菜单行:11 项要在 320 px 高的屏上排开(顶栏 24 起,底部留给提示行),
// 所以行高/行距比挂件那种网页菜单紧 —— 24 px 行 + 24 px 行距,末行落在 264..286。
// 12 项要塞进 320 px:22 px 行高 + 22 px 行距,最后一行底边落在 284,给底部提示行留够。
#define ROW_X       14
#define ROW_W       212
#define ROW_H       20
#define ROW_GAP     22
#define ROW_Y0      22

// 配色:主色 #203170 / 文字 #536BA9 / 浅文字 #9FB0D9 直接取自挂件样式表。
// 屏幕底色用深蓝而不是挂件的透明:名片没有网页做背景,深色既省电也让白泡泡跳出来。
#define C_BG            0x0B1330
#define C_INK           0x203170
#define C_BUBBLE        0xFFFFFF
#define C_BUBBLE_LINE   0x203170
#define C_LABEL         0x536BA9
#define C_HINT          0x9FB0D9
#define C_TOP           0x8FA0C0
#define C_TEXT          0xE6ECF8
#define C_DIM           0x7C8CB0
#define C_PUPIL         0x203170
#define C_PEAK          0xE8834A
#define C_VALLEY        0x2ECC9B
#define C_GOLD          0xFFC93C
#define C_WARN          0xE8834A
#define C_ROW           0x121C3C
#define C_ROW_SEL       0x203170
#define C_ROW_LINE      0x2B3A5E

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *battery;      // 百分比数字(soc < 0 时显示 "--")
    lv_obj_t *batt_shell;   // 电池外框
    lv_obj_t *batt_cap;     // 正极帽
    lv_obj_t *batt_fill;    // 按容量伸缩的填充条
} page_base_t;

typedef struct {
    page_base_t base;
    lv_obj_t *stage;       // Q 弹舞台:只有角色图,整块以脚底为原点缩放
    lv_obj_t *bubble_layer;// 气泡层(图 + 三行文本),不参与 Q 弹,自己有进出场
    lv_obj_t *bubble;      // 整条气泡图(大椭圆 + 尾巴 + 两个小泡泡)
    lv_obj_t *label;
    lv_obj_t *amount;
    lv_obj_t *hint;
    lv_obj_t *badge;
    lv_obj_t *badge_text;
    lv_obj_t *status;
    lv_obj_t *whale;
    lv_obj_t *coin;
} home_t;

typedef struct {
    page_base_t base;
    lv_obj_t *rows[WHALE_SET_COUNT];
    lv_obj_t *labels[WHALE_SET_COUNT];
    lv_obj_t *values[WHALE_SET_COUNT];
} settings_t;

typedef struct {
    page_base_t base;
    lv_obj_t *card;
    lv_obj_t *ap_ssid;
    lv_obj_t *ap_password;
    lv_obj_t *url;
    lv_obj_t *status;
} provision_t;

static home_t s_home;
static settings_t s_settings;
static provision_t s_prov;
static whale_page_t s_page = WHALE_PAGE_HOME;
static bool s_ready;
static bool s_bub_shown;     // 气泡层目标态:false = 收起(详见下面的进出场动画)
static int s_rendered_view = -1;         // 气泡里当前画着的是哪个视图(-1 = 还没画过)
static whale_ui_state_t s_pending;       // 切换动画期间暂存的新内容
static int s_pending_view;               // 上面那份内容对应的视图

// ---- 通用小工具 ------------------------------------------------------------

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font,
                            uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, LINE_H);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    return label;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                          uint32_t bg)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *make_circle(lv_obj_t *parent, int x, int y, int size, uint32_t bg)
{
    return make_box(parent, x, y, size, size, size / 2, bg);
}

// 泡泡的两块:白底 + 深蓝描边。圆角取半高,出来是挂件那种胶囊形轮廓。
// 小圆点另给更细的描边:挂件里椭圆只有 25 px 半径却带 18 px 描边,缩到名片上
// 再用同样比例就糊成一个实心点,这里按可读性折细。
static lv_obj_t *make_bubble_part(lv_obj_t *parent, int x, int y, int w, int h, int line)
{
    lv_obj_t *box = make_box(parent, x, y, w, h, h / 2, C_BUBBLE);
    lv_obj_set_style_border_width(box, line, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_BUBBLE_LINE), 0);
    return box;
}

static void build_top_bar(lv_obj_t *screen, const char *title, page_base_t *base)
{
    lv_obj_t *label = make_label(screen, MARGIN, TOP_Y, CONTENT_W - BATT_W, &whale_ui_font_16,
                                 C_TOP, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(label, title);

    // 电量块整体一个容器,内部再摆文字与图形;隐藏电量计不可用时整块隐藏即可。
    const int group_x = SCREEN_W - MARGIN - BATT_W;
    lv_obj_t *group = make_box(screen, group_x, TOP_Y, BATT_W, TOP_H, 0, C_BG);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);

    base->battery = make_label(group, 0, 0, BATT_NUM_W, &whale_ui_font_16, C_TOP,
                               LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(base->battery, "--");

    base->batt_shell = make_box(group, BATT_ICON_X, BATT_ICON_Y, BATT_ICON_W, BATT_ICON_H, 2, C_BG);
    lv_obj_set_style_bg_opa(base->batt_shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(base->batt_shell, 1, 0);
    lv_obj_set_style_border_color(base->batt_shell, lv_color_hex(C_TOP), 0);

    base->batt_cap = make_box(group, BATT_ICON_X + BATT_ICON_W,
                              BATT_ICON_Y + (BATT_ICON_H - BATT_CAP_H) / 2, BATT_CAP_W, BATT_CAP_H,
                              1, C_TOP);

    base->batt_fill = make_box(group, BATT_FILL_X, BATT_FILL_Y, 0, BATT_FILL_H, 0, C_VALLEY);
    lv_obj_add_flag(base->batt_shell, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(base->batt_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(base->batt_fill, LV_OBJ_FLAG_HIDDEN);

    base->screen = screen;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

// ---- 小鲸鱼 ----------------------------------------------------------------
//
// 原来用圆角矩形 + 旋转拼出一只极简小鲸鱼;现在直接用参考挂件的原版角色图
// (DSniang1.png → RGB565A8 C 数组),与挂件 right:0;bottom:0 的贴法一致:
// 角色本体是 139 × 139 的 PNG cut-out(已裁掉原图的透明边距),底边压在 262、
// 右边距 6 px;左侧空档留给消费时的金币掉落。改尺寸/位置前后跑一次
// tools/preview_whale_screen.py,先看截图再决定要不要刷机。

static void build_whale(lv_obj_t *parent)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_set_pos(group, WHALE_X, WHALE_Y - STAGE_Y0);
    lv_obj_set_size(group, WHALE_W, WHALE_H);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    s_home.whale = group;

    // 角色本体:原版 cut-out,透明底,水平居中。
    lv_obj_t *img = lv_image_create(group);
    lv_image_set_src(img, &whale_dsnang_img);
    lv_obj_set_pos(img, WHALE_IMG_X, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    // 金币:平时藏着,检测到消费时才掉下来(落在角色图左侧的空档)。
    s_home.coin = make_circle(group, COIN_X, 0, 16, C_GOLD);
    lv_obj_add_flag(s_home.coin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *coin_mark = make_label(s_home.coin, 0, 1, 16, &whale_ui_font_16, C_PUPIL,
                                     LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(coin_mark, "¥");
}

// 一次 Q 弹 = 压扁(110ms ease-out)→ 弹回(220ms 回弹曲线,会过冲一点点再收住)。
// 两段动画串起来,中间不落地,所以看起来是「按下去、噌地弹回来」。

static void squash_apply(int32_t sy256)
{
    if (!s_home.stage) return;
    lv_obj_set_style_transform_scale_y(s_home.stage, sy256, 0);
    lv_obj_set_style_transform_scale_x(s_home.stage, SQ_X_OF(sy256), 0);
}

static void squash_exec_cb(void *obj, int32_t value)
{
    (void)obj;
    squash_apply(value);
}

static void start_idle_boing(void)
{
    if (!s_home.stage) return;
    lv_anim_delete(s_home.stage, squash_exec_cb);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_home.stage);
    lv_anim_set_exec_cb(&anim, squash_exec_cb);
    lv_anim_set_values(&anim, SQ_SCALE_1, SQ_IDLE_Y);
    lv_anim_set_duration(&anim, SQ_IDLE_MS);
    lv_anim_set_playback_time(&anim, SQ_IDLE_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

// 回弹结束后把常驻呼吸接回去(否则按一次之后画面就彻底静止了)。
static void boing_back_done_cb(lv_anim_t *anim)
{
    (void)anim;
    start_idle_boing();
}

static void boing_press_done_cb(lv_anim_t *anim)
{
    (void)anim;
    lv_anim_t back;
    lv_anim_init(&back);
    lv_anim_set_var(&back, s_home.stage);
    lv_anim_set_exec_cb(&back, squash_exec_cb);
    lv_anim_set_values(&back, SQ_PRESS_Y, SQ_SCALE_1);
    lv_anim_set_duration(&back, SQ_BACK_MS);
    // cubic-bezier(.34,1.56,.64,1) —— 参考挂件那条 overshoot 回弹曲线,
    // 值会冲过 1.0 再回落,这就是「Q」的手感来源。
    lv_anim_set_path_cb(&back, lv_anim_path_custom_bezier3);
    _PARA(&back, 0.34, 1.56, 0.64, 1);
    lv_anim_set_completed_cb(&back, boing_back_done_cb);
    lv_anim_start(&back);
}

void whale_ui_boing(void)
{
    if (!s_ready || !s_home.stage) return;
    if (!bsp_lvgl_lock(200)) return;

    lv_anim_delete(s_home.stage, squash_exec_cb);   // 先掐掉常驻呼吸,别抢同一个属性
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_home.stage);
    lv_anim_set_exec_cb(&anim, squash_exec_cb);
    lv_anim_set_values(&anim, SQ_SCALE_1, SQ_PRESS_Y);
    lv_anim_set_duration(&anim, SQ_PRESS_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, boing_press_done_cb);
    lv_anim_start(&anim);

    bsp_lvgl_unlock();
}

// ---- 气泡进出场 ------------------------------------------------------------
//
// 气泡不做硬切换:整层(图 + 三行文本)走缓进缓出,方向固定是「下进下出」——
//   消失:由上至下,一边往下沉一边淡出(终点 下方 +DY);
//   出现:由下至上,从下方 +DY 浮到位一边淡入。
// 收起 / 展开是单段;**切换内容**是把两段串起来:旧的先沉下去消失,换好字,
// 新的再从下面浮上来(见 bubble_swap_content)。
// opa 只设在层上就够:LVGL 9 的 opa 会递归作用到子对象,图像和三行文本一起变。

static void bubble_in_exec_cb(void *obj, int32_t v)
{
    lv_obj_t *layer = obj;
    if (layer == NULL) return;
    lv_obj_set_y(layer, BUB_LAYER_Y + BUB_SLIDE_DY - (BUB_SLIDE_DY * v) / 256);
    lv_obj_set_style_opa(layer, (lv_opa_t)((LV_OPA_COVER * v) / 256), 0);
}

// 由上至下:往下沉 + 淡出。
static void bubble_out_exec_cb(void *obj, int32_t v)
{
    lv_obj_t *layer = obj;
    if (layer == NULL) return;
    lv_obj_set_y(layer, BUB_LAYER_Y + (BUB_SLIDE_DY * v) / 256);
    lv_obj_set_style_opa(layer, (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER * v) / 256), 0);
}

// 由下至上:浮上来 + 淡入。
static void bubble_play_in(void)
{
    lv_obj_t *layer = s_home.bubble_layer;
    if (layer == NULL) return;

    lv_obj_remove_flag(layer, LV_OBJ_FLAG_HIDDEN);
    bubble_in_exec_cb(layer, 0);              // 先落到起点(下方 + 全透明)再淡入

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, layer);
    lv_anim_set_exec_cb(&anim, bubble_in_exec_cb);
    lv_anim_set_values(&anim, 0, 256);
    lv_anim_set_duration(&anim, BUB_IN_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);   // 缓进缓出
    lv_anim_start(&anim);
}

static void bubble_out_done_cb(lv_anim_t *anim)
{
    lv_obj_t *layer = anim->var;
    if (layer == NULL) return;
    // 收起才真的藏起来;切换内容时 s_bub_shown 仍是 true,交给入场动画接管。
    if (!s_bub_shown) lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
}

static void bubble_set_open(bool open)
{
    lv_obj_t *layer = s_home.bubble_layer;
    if (layer == NULL || open == s_bub_shown) return;

    s_bub_shown = open;
    lv_anim_delete(layer, bubble_in_exec_cb);
    lv_anim_delete(layer, bubble_out_exec_cb);

    if (open) {
        bubble_play_in();
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, layer);
    lv_anim_set_exec_cb(&anim, bubble_out_exec_cb);
    lv_anim_set_values(&anim, 0, 256);
    lv_anim_set_duration(&anim, BUB_OUT_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);   // 缓进缓出
    lv_anim_set_completed_cb(&anim, bubble_out_done_cb);
    lv_anim_start(&anim);
}

// 切换气泡内容:旧的由上至下沉下去 → 趁全透明换字 → 新的由下至上浮上来。
// 换字放在消失动画的完成回调里,否则会看到旧字在半透明状态上跳变成新字。
static void bubble_swap_done_cb(lv_anim_t *anim);

static void bubble_swap_content(void)
{
    lv_obj_t *layer = s_home.bubble_layer;
    if (layer == NULL) return;

    lv_anim_delete(layer, bubble_in_exec_cb);
    lv_anim_delete(layer, bubble_out_exec_cb);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, layer);
    lv_anim_set_exec_cb(&anim, bubble_out_exec_cb);
    lv_anim_set_values(&anim, 0, 256);
    lv_anim_set_duration(&anim, BUB_OUT_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, bubble_swap_done_cb);
    lv_anim_start(&anim);
}

// ---- 余额滚动动画 ----------------------------------------------------------
//
// 挂件里余额变化是"数字滚动"。名片上没有逐位翻牌的部件,这里用数值补间近似:
// 每帧把当前值格式化成 ¥x.xx 重画一遍,整数与两位小数一起动,观感就是数字在滚。

#define BALANCE_ROLL_MS 600

static int64_t s_shown_cents;    // 泡泡上当前画出来的余额
static bool s_shown_valid;       // false = 还没有起点(首次 / 断过线),直接落值不滚
static int64_t s_roll_target;    // 本次补间的终点,收尾时补最后一帧

static void set_amount_money(int64_t cents)
{
    char money[24];
    char big[32];
    whale_money_format(cents, money, sizeof(money));
    snprintf(big, sizeof(big), "¥%s", money);
    lv_label_set_text(s_home.amount, big);
}

static void balance_roll_cb(void *var, int32_t value)
{
    (void)var;
    s_shown_cents = (int64_t)value;
    set_amount_money(s_shown_cents);
}

static void balance_roll_done_cb(lv_anim_t *anim)
{
    (void)anim;
    // 补间的最后一帧不一定正好落在终值上,这里补一次,免得余额差一分钱。
    s_shown_cents = s_roll_target;
    set_amount_money(s_roll_target);
}

// 只在余额视图里滚:切到别的视图时 amount 会被别的文案占用,动画会跟它打架。
static void balance_roll_stop(void)
{
    lv_anim_delete(s_home.amount, balance_roll_cb);
}

static void balance_roll_start(int64_t from, int64_t to)
{
    balance_roll_stop();
    s_roll_target = to;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_home.amount);
    lv_anim_set_exec_cb(&anim, balance_roll_cb);
    lv_anim_set_values(&anim, (int32_t)from, (int32_t)to);
    lv_anim_set_duration(&anim, BALANCE_ROLL_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, balance_roll_done_cb);
    lv_anim_start(&anim);
}

// ---- 挂件页 ----------------------------------------------------------------

static void build_home(void)
{
    lv_obj_t *screen = make_screen();
    build_top_bar(screen, "小鲸鱼记账", &s_home.base);

    // Q 弹舞台:只放角色。气泡单独成层(见下),不跟着弹 —— 气泡跟着缩会晃得厉害,
    // 而且舞台一变大每帧要重绘整屏,屏刷新跟不上就卡。
    // 顶栏、状态行、底部提示留在舞台外(它们是屏幕固定元素,不该跟着晃)。
    lv_obj_t *stage = lv_obj_create(screen);
    lv_obj_set_pos(stage, 0, STAGE_Y0);
    lv_obj_set_size(stage, STAGE_W, STAGE_H);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_set_style_radius(stage, 0, 0);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(stage, STAGE_PIVOT_X, 0);
    lv_obj_set_style_transform_pivot_y(stage, STAGE_PIVOT_Y, 0);
    s_home.stage = stage;

    // 角色先画、气泡压在上面 —— 参考挂件里 img 在 bubble 之前、
    // bubble 带 z-index:1,所以小气泡的黑边是压在角色身上的,这里保持一致。
    build_whale(stage);

    // 气泡层:图 + 三行文本。整层一起做进出场(位移 + 淡入淡出),不参与 Q 弹。
    // 建在 stage 之后 → 画在角色上面,和参考的 z 序一致。
    lv_obj_t *blayer = lv_obj_create(screen);
    lv_obj_set_pos(blayer, BUB_LAYER_X, BUB_LAYER_Y);
    lv_obj_set_size(blayer, BUB_LAYER_W, BUB_LAYER_H);
    lv_obj_set_style_bg_opa(blayer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(blayer, 0, 0);
    lv_obj_set_style_pad_all(blayer, 0, 0);
    lv_obj_set_style_radius(blayer, 0, 0);
    lv_obj_clear_flag(blayer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(blayer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(blayer, LV_OPA_TRANSP, 0);
    s_home.bubble_layer = blayer;
    s_bub_shown = false;

    // 气泡整条一张图:大椭圆 + 半椭圆尾巴 + 两个小泡泡,几何来自参考挂件的 SVG。
    s_home.bubble = lv_image_create(blayer);
    lv_image_set_src(s_home.bubble, &whale_bubble_img);
    lv_obj_set_pos(s_home.bubble, BUB_IMG_X, BUB_IMG_Y - BUB_LAYER_Y);
    lv_obj_clear_flag(s_home.bubble, LV_OBJ_FLAG_CLICKABLE);

    // 泡泡内三行文本:标签 / 主数值 / 补充。气泡是张图,文本直接按椭圆中心排版;
    // 主数值与补充行用省略号截断 —— 连接视图要显示 SSID,最长的 32 字节会撑破白底。
    // 三行放进气泡层:收起气泡时它们跟着一起消失(之前挂在舞台上会留下三行孤字)。
    s_home.label = make_label(blayer, BUB_TEXT_X, BUB_ROW1_Y - BUB_LAYER_Y, BUB_TEXT_W,
                              &whale_ui_font_16, C_LABEL, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_home.label, LV_LABEL_LONG_DOT);
    s_home.amount = make_label(blayer, BUB_TEXT_X, BUB_ROW2_Y - BUB_LAYER_Y, BUB_TEXT_W,
                               &whale_ui_font_24, C_LABEL, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(s_home.amount, BUB_TEXT_W, 30);
    lv_label_set_long_mode(s_home.amount, LV_LABEL_LONG_DOT);
    s_home.hint = make_label(blayer, BUB_TEXT_X, BUB_ROW3_Y - BUB_LAYER_Y, BUB_TEXT_W,
                             &whale_ui_font_16, C_HINT, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_home.hint, LV_LABEL_LONG_DOT);

    // 状态行:左侧挂峰谷徽章,右侧是连接 / 预警文案。
    s_home.badge = make_box(screen, MARGIN, STATUS_Y, BADGE_W, LINE_H, 9, C_VALLEY);
    s_home.badge_text = make_label(s_home.badge, 0, 0, BADGE_W, &whale_ui_font_16, C_TEXT,
                                   LV_TEXT_ALIGN_CENTER);
    s_home.status = make_label(screen, MARGIN, STATUS_Y, CONTENT_W, &whale_ui_font_16,
                               C_DIM, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *hint = make_label(screen, MARGIN, HINT_Y, CONTENT_W, &whale_ui_font_16,
                                C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(hint, "长按菜单 · 上刷新 · 下试听");
}

// ---- 菜单页 ----------------------------------------------------------------

static void build_settings(void)
{
    lv_obj_t *screen = make_screen();
    build_top_bar(screen, "菜单", &s_settings.base);

    static const char *LABELS[WHALE_SET_COUNT] = {
        "配网与 API Key", "余额刷新间隔", "今日预算", "余额预警", "峰谷样式", "音效总开关",
        "按压音效", "消费金币声", "连接提示音", "熄屏时间", "今日记账清零", "返回挂件页",
    };

    for (int i = 0; i < WHALE_SET_COUNT; i++) {
        lv_obj_t *row = make_box(screen, ROW_X, ROW_Y0 + i * ROW_GAP, ROW_W, ROW_H, 8, C_ROW);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_ROW_LINE), 0);
        s_settings.rows[i] = row;

        // 值列加宽到 68 px:峰谷样式最长的两个值是「梁文峰谷」和「!?强强?!」,
        // 16 px 下都要 64 px,54 px 的列会把它挤出去。
        s_settings.labels[i] = make_label(row, 10, 2, 122, &whale_ui_font_16, C_TEXT,
                                          LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(s_settings.labels[i], LABELS[i]);
        s_settings.values[i] = make_label(row, 136, 2, 68, &whale_ui_font_16, C_DIM,
                                          LV_TEXT_ALIGN_RIGHT);
        lv_label_set_text(s_settings.values[i], "");
    }

    lv_obj_t *hint = make_label(screen, MARGIN, HINT_Y + 4, CONTENT_W, &whale_ui_font_16,
                                C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(hint, "长按返回 · 长按首项清凭据");
}

// ---- 配网页 ---------------------------------------------------------------

static void build_provision(void)
{
    lv_obj_t *screen = make_screen();
    build_top_bar(screen, "配网", &s_prov.base);

    s_prov.card = make_bubble_part(screen, 14, 26, 212, 150, BUB_LINE);
    lv_obj_set_style_radius(s_prov.card, 14, 0);

    const int x = 14;
    lv_obj_t *line1 = make_label(s_prov.card, x, 10, 184, &whale_ui_font_16, C_DIM,
                                 LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(line1, "1. 连上名片热点");

    s_prov.ap_ssid = make_label(s_prov.card, x, 32, 184, &whale_ui_font_16, C_LABEL,
                                LV_TEXT_ALIGN_LEFT);

    lv_obj_t *line3 = make_label(s_prov.card, x, 54, 184, &whale_ui_font_16, C_DIM,
                                 LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(line3, "密码");
    s_prov.ap_password = make_label(s_prov.card, x + 44, 54, 140, &whale_ui_font_16, C_LABEL,
                                    LV_TEXT_ALIGN_LEFT);

    lv_obj_t *line4 = make_label(s_prov.card, x, 80, 184, &whale_ui_font_16, C_DIM,
                                 LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(line4, "2. 浏览器打开该地址");
    s_prov.url = make_label(s_prov.card, x, 102, 184, &whale_ui_font_16, C_LABEL,
                            LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_prov.url, "http://192.168.4.1");

    lv_obj_t *line6 = make_label(s_prov.card, x, 126, 184, &whale_ui_font_16, C_DIM,
                                 LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(line6, "3. 填好 Wi-Fi 与 API Key 提交");

    s_prov.status = make_label(screen, MARGIN, 190, CONTENT_W, &whale_ui_font_16, C_TEXT,
                               LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_prov.status, "等待网页提交");

    lv_obj_t *hint = make_label(screen, MARGIN, HINT_Y - 4, CONTENT_W, &whale_ui_font_16,
                                C_DIM, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(hint, "长按确定键返回挂件页");
}

// ---- 对外接口 --------------------------------------------------------------

bool whale_ui_init(void)
{
    if (s_ready) return true;

    build_home();
    build_settings();
    build_provision();

    s_ready = true;
    whale_ui_show_page(WHALE_PAGE_HOME);
    start_idle_boing();
    return true;
}

void whale_ui_show_page(whale_page_t page)
{
    if (!s_ready) return;

    s_page = page;
    switch (page) {
    case WHALE_PAGE_HOME:
        lv_screen_load(s_home.base.screen);
        break;
    case WHALE_PAGE_SETTINGS:
        lv_screen_load(s_settings.base.screen);
        break;
    case WHALE_PAGE_PROVISION:
        lv_screen_load(s_prov.base.screen);
        break;
    }
}

whale_page_t whale_ui_current_page(void)
{
    return s_page;
}

// 电量块的刷新。soc < 0 表示电量计读不到:只留 "--",电池图形整块隐藏
// (半枚空壳比没有更容易被误读成"没电")。
//
// 配色沿用界面里已有的三档语义色:>50 绿、20..50 金、<=20 橙 —— 低于 20% 就该
// 准备充电了,这条线的目的是让人一眼扫到,而不是精确到百分比。
static void update_battery_widget(page_base_t *base, int soc)
{
    if (soc < 0) {
        lv_label_set_text(base->battery, "--");
        lv_obj_add_flag(base->batt_shell, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(base->batt_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(base->batt_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char text[16];
    snprintf(text, sizeof(text), "%d%%", soc);
    lv_label_set_text(base->battery, text);

    const uint32_t color = (soc <= 20) ? C_WARN : ((soc <= 50) ? C_GOLD : C_VALLEY);
    lv_obj_set_style_text_color(base->battery, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(base->batt_fill, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(base->batt_shell, lv_color_hex(color), 0);

    int w = 0;
    if (soc > 0) {
        w = (soc * BATT_FILL_W + 50) / 100;   // +50 是四舍五入
        if (w < 2) w = 2;                     // 还剩一点点时也要看得见有内容
        if (w > BATT_FILL_W) w = BATT_FILL_W;
    }
    lv_obj_set_size(base->batt_fill, w, BATT_FILL_H);

    lv_obj_clear_flag(base->batt_shell, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(base->batt_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(base->batt_fill, LV_OBJ_FLAG_HIDDEN);
}

void whale_ui_battery_refresh(void)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(200)) return;

    const int soc = bsp_battery_soc();
    update_battery_widget(&s_home.base, soc);
    update_battery_widget(&s_settings.base, soc);
    update_battery_widget(&s_prov.base, soc);

    bsp_lvgl_unlock();
}

static void apply_status_text(lv_obj_t *label, whale_status_t status, const char *ip,
                              const char *updated)
{
    char text[64];
    switch (status) {
    case WHALE_ST_UNCONFIGURED:
        lv_label_set_text(label, "未配网 · 长按开菜单");
        break;
    case WHALE_ST_CONNECTING:
        lv_label_set_text(label, "正在连接 Wi-Fi…");
        break;
    case WHALE_ST_ONLINE:
        // 状态行左侧还要挂峰谷徽章,只有约 150 px 可用:这里显示"已连接 · 时刻",
        // 完整 IP 交给泡泡的「连接」视图去显示。
        if (updated[0] != '\0' && strcmp(updated, "--:--") != 0) {
            snprintf(text, sizeof(text), "已连接 · %s", updated);
        } else if (ip[0] != '\0') {
            snprintf(text, sizeof(text), "已连接 · %s", ip);
        } else {
            snprintf(text, sizeof(text), "已连接");
        }
        lv_label_set_text(label, text);
        break;
    case WHALE_ST_REFRESHING:
        lv_label_set_text(label, "正在取余额…");
        break;
    case WHALE_ST_AUTH_FAILED:
        lv_label_set_text(label, "Key 无效,请配网");
        break;
    case WHALE_ST_NETWORK_FAILED:
        lv_label_set_text(label, "网络不可用 · 重试");
        break;
    case WHALE_ST_NO_TIME:
        lv_label_set_text(label, "时间未同步");
        break;
    case WHALE_ST_BUDGET_HIT:
        lv_label_set_text(label, "今日已用超预算");
        break;
    case WHALE_ST_BALANCE_LOW:
        lv_label_set_text(label, "余额低于预警线");
        break;
    default:
        lv_label_set_text(label, "");
        break;
    }
}

// 气泡里那三行字按视图渲染。抽成函数是为了让切换动画能在气泡全透明的那一刻
// 再换字(见 bubble_swap_done_cb),否则会看见旧字在半透明上直接跳成新字。
static void render_bubble_content(const whale_ui_state_t *state, int view)
{
    char money[24];
    char big[32];
    char line[64];

    if (view == 0) {
        lv_label_set_text(s_home.label, "余额");
        if (!state->has_balance) {
            // 没有余额可显示:停掉在跑的动画,并丢掉起点 —— 下次拿到余额时
            // 从"没有"跳到"有",不该滚。
            balance_roll_stop();
            s_shown_valid = false;
            lv_label_set_text(s_home.amount, "¥--.--");
        } else {
            const int64_t target = state->balance_cents;
            // lv_anim 的值是 int32:超过约 2147 万元的余额不做补间,直接落值。
            const bool in_range = target >= 0 && target <= INT32_MAX &&
                                  s_shown_cents >= 0 && s_shown_cents <= INT32_MAX;
            if (s_shown_valid && in_range && s_shown_cents != target) {
                balance_roll_start(s_shown_cents, target);
            } else {
                balance_roll_stop();
                s_shown_cents = target;
                s_shown_valid = true;
                set_amount_money(target);
            }
        }
        whale_money_format(state->today_used_cents, money, sizeof(money));
        snprintf(line, sizeof(line), "今日已用 ¥%s", money);
        lv_label_set_text(s_home.hint, line);
    } else if (view == 1) {
        lv_label_set_text(s_home.label, "今日已用");
        whale_money_format(state->today_used_cents, money, sizeof(money));
        snprintf(big, sizeof(big), "¥%s", money);
        lv_label_set_text(s_home.amount, big);
        if (state->last_delta_cents < 0) {
            whale_money_format(-state->last_delta_cents, money, sizeof(money));
            snprintf(line, sizeof(line), "本次消费 ¥%s", money);
        } else if (state->last_delta_cents > 0) {
            whale_money_format(state->last_delta_cents, money, sizeof(money));
            snprintf(line, sizeof(line), "本次充值 ¥%s", money);
        } else {
            snprintf(line, sizeof(line), "本次无变化");
        }
        lv_label_set_text(s_home.hint, line);
    } else if (view == 2) {
        // 峰谷视图照挂件的「显示样式」走:标签随样式变(倒计时样式另标),主数值是
        // 该样式的状态文字,补充行始终是倒计时。倒计时样式反过来 —— 主数值是倒计时。
        const bool peak = state->tariff == WHALE_TARIFF_PEAK;
        const char *status = whale_peak_status_text(state->peak_style, peak);
        lv_label_set_text(s_home.label,
                          status == NULL ? "时段倒计时" : "峰谷时段");
        if (!state->tariff_known) {
            lv_label_set_text(s_home.amount, "时间未同步");
            lv_label_set_text(s_home.hint, "连上网后自动校准");
        } else if (status == NULL) {
            if (state->minutes_to_change >= 0) {
                snprintf(big, sizeof(big), "%d:%02d", state->minutes_to_change / 60,
                         state->minutes_to_change % 60);
            } else {
                snprintf(big, sizeof(big), "--:--");
            }
            lv_label_set_text(s_home.amount, big);
            // 三元里直接给字符串常量、不走 snprintf:避免格式串不是字面量被判 warning。
            lv_label_set_text(s_home.hint, state->minutes_to_change >= 0
                              ? (peak ? "后转谷时" : "后转峰时") : "今天不再切换");
        } else {
            lv_label_set_text(s_home.amount, status);
            if (state->minutes_to_change >= 0) {
                const int hh = state->minutes_to_change / 60;
                const int mm = state->minutes_to_change % 60;
                if (peak) {
                    snprintf(line, sizeof(line), "距谷时 %d:%02d", hh, mm);
                } else {
                    snprintf(line, sizeof(line), "距峰时 %d:%02d", hh, mm);
                }
            } else {
                snprintf(line, sizeof(line), "%s", peak ? "工作日 9-12 / 14-18 峰" : "今天不再进峰时");
            }
            lv_label_set_text(s_home.hint, line);
        }
    } else {
        lv_label_set_text(s_home.label, "连接");
        if (state->ssid_preview[0] != '\0') {
            lv_label_set_text(s_home.amount, state->ssid_preview);
        } else {
            lv_label_set_text(s_home.amount, "未配网");
        }
        snprintf(line, sizeof(line), "间隔 %lus · %s", (unsigned long)state->refresh_sec,
                 state->updated);
        lv_label_set_text(s_home.hint, line);
    }
}

// 沉下去之后:趁气泡全透明把三行字换成新的,再让它从下面浮上来。
static void bubble_swap_done_cb(lv_anim_t *anim)
{
    (void)anim;
    render_bubble_content(&s_pending, s_pending_view);
    bubble_play_in();
}

void whale_ui_update(const whale_ui_state_t *state)
{
    if (!s_ready || state == NULL) return;
    if (!bsp_lvgl_lock(200)) return;

    // 峰谷徽章挂在状态行左侧;时间没同步出来时整行让给状态文案。
    if (state->tariff_known) {
        lv_obj_remove_flag(s_home.badge, LV_OBJ_FLAG_HIDDEN);
        const bool peak = state->tariff == WHALE_TARIFF_PEAK;
        lv_obj_set_style_bg_color(s_home.badge, lv_color_hex(peak ? C_PEAK : C_VALLEY), 0);
        lv_label_set_text(s_home.badge_text, peak ? "峰时" : "谷时");
        lv_obj_set_x(s_home.status, MARGIN + BADGE_W + 6);
        lv_obj_set_size(s_home.status, CONTENT_W - BADGE_W - 6, LINE_H);
    } else {
        lv_obj_add_flag(s_home.badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_home.status, MARGIN);
        lv_obj_set_size(s_home.status, CONTENT_W, LINE_H);
    }

    const int view = (state->view % WHALE_VIEW_COUNT + WHALE_VIEW_COUNT) % WHALE_VIEW_COUNT;

    // 离开余额视图就停滚:amount 那一行改显示别的视图内容,动画继续跑会覆盖它。
    if (view != 0) balance_roll_stop();

    const bool open = state->bubble_open;
    if (open != s_bub_shown) {
        // 收起 / 展开:内容趁气泡还没露出来(或已经沉下去)直接换好,只播单段动画。
        s_rendered_view = view;
        render_bubble_content(state, view);
        bubble_set_open(open);
    } else if (open && s_rendered_view >= 0 && view != s_rendered_view) {
        // 切换视图:旧的由上至下沉下去 → 换字 → 新的由下至上浮出来。
        // 这里先把 s_rendered_view 记成新值,免得同一次切换被反复触发
        // (余额刷新每秒都会调进来)。
        s_rendered_view = view;
        s_pending = *state;
        s_pending_view = view;
        bubble_swap_content();
    } else {
        s_rendered_view = view;
        render_bubble_content(state, view);
    }

    apply_status_text(s_home.status, state->status, state->ip, state->updated);
    update_battery_widget(&s_home.base, state->battery);

    bsp_lvgl_unlock();
}

// 阈值行:未设置显示「关」,否则显示 ¥ 金额。
static void set_threshold_text(lv_obj_t *label, int64_t cents)
{
    // value 要比 money 宽一格以上:"¥" 占 3 字节(UTF-8),加上 23 字节的金额字符串,
    // 24 字节的缓冲区会被 -Werror=format-truncation 判死。
    char value[32];
    if (cents <= 0) {
        lv_label_set_text(label, "关");
        return;
    }
    char money[24];
    whale_money_format(cents, money, sizeof(money));
    snprintf(value, sizeof(value), "¥%s", money);
    lv_label_set_text(label, value);
}

void whale_ui_settings_update(whale_setting_t selected, uint32_t refresh_sec,
                              int64_t budget_cents, int64_t warn_cents,
                              whale_peak_style_t peak_style, uint32_t sleep_sec,
                              bool sound_all, bool sound_press, bool sound_spend,
                              bool sound_connect)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(200)) return;

    char value[24];
    for (int i = 0; i < WHALE_SET_COUNT; i++) {
        const bool active = (i == (int)selected);
        lv_obj_set_style_bg_color(s_settings.rows[i],
                                  lv_color_hex(active ? C_ROW_SEL : C_ROW), 0);
        lv_obj_set_style_border_color(s_settings.rows[i],
                                      lv_color_hex(active ? C_INK : C_ROW_LINE), 0);

        switch (i) {
        case WHALE_SET_PROVISION:
            lv_label_set_text(s_settings.values[i], "网页");
            break;
        case WHALE_SET_REFRESH:
            snprintf(value, sizeof(value), "%lus", (unsigned long)refresh_sec);
            lv_label_set_text(s_settings.values[i], value);
            break;
        case WHALE_SET_BUDGET:
            set_threshold_text(s_settings.values[i], budget_cents);
            break;
        case WHALE_SET_WARN:
            set_threshold_text(s_settings.values[i], warn_cents);
            break;
        case WHALE_SET_PEAK_STYLE:
            lv_label_set_text(s_settings.values[i], whale_peak_style_name(peak_style));
            break;
        case WHALE_SET_SOUND_ALL:
            lv_label_set_text(s_settings.values[i], sound_all ? "开" : "关");
            break;
        case WHALE_SET_SOUND_PRESS:
            lv_label_set_text(s_settings.values[i], sound_press ? "开" : "关");
            break;
        case WHALE_SET_SOUND_SPEND:
            lv_label_set_text(s_settings.values[i], sound_spend ? "开" : "关");
            break;
        case WHALE_SET_SOUND_CONNECT:
            lv_label_set_text(s_settings.values[i], sound_connect ? "开" : "关");
            break;
        case WHALE_SET_SLEEP:
            lv_label_set_text(s_settings.values[i], whale_sleep_name(sleep_sec));
            break;
        case WHALE_SET_LEDGER:
            lv_label_set_text(s_settings.values[i], "清零");
            break;
        default:
            lv_label_set_text(s_settings.values[i], "");
            break;
        }
    }

    bsp_lvgl_unlock();
}

void whale_ui_provision_update(whale_prov_status_t status, const char *ap_ssid,
                               const char *ap_password, const char *ip)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(200)) return;

    if (ap_ssid != NULL) lv_label_set_text(s_prov.ap_ssid, ap_ssid);
    if (ap_password != NULL) lv_label_set_text(s_prov.ap_password, ap_password);

    char text[64];
    switch (status) {
    case WHALE_PV_WAITING:
        lv_label_set_text(s_prov.status, "等待网页提交");
        break;
    case WHALE_PV_SAVED:
        lv_label_set_text(s_prov.status, "已保存 · 正在连接");
        break;
    case WHALE_PV_CONNECTING:
        lv_label_set_text(s_prov.status, "正在连接 Wi-Fi…");
        break;
    case WHALE_PV_ONLINE:
        if (ip != NULL && ip[0] != '\0') {
            snprintf(text, sizeof(text), "已连接 · %s", ip);
        } else {
            snprintf(text, sizeof(text), "已连接");
        }
        lv_label_set_text(s_prov.status, text);
        break;
    case WHALE_PV_FAILED:
        lv_label_set_text(s_prov.status, "连接失败 · 检查名称与密码");
        break;
    default:
        break;
    }

    bsp_lvgl_unlock();
}

void whale_ui_sleep_notice(void)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(200)) return;
    lv_label_set_text(s_home.status, "休眠中 · 按键唤醒");
    bsp_lvgl_unlock();
}

static void coin_y_cb(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void coin_done_cb(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_home.coin, LV_OBJ_FLAG_HIDDEN);
}

void whale_ui_coin_drop(void)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(200)) return;

    lv_anim_delete(s_home.coin, NULL);
    lv_obj_remove_flag(s_home.coin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_home.coin, 0);
    lv_obj_set_style_bg_opa(s_home.coin, LV_OPA_COVER, 0);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_home.coin);
    lv_anim_set_exec_cb(&anim, coin_y_cb);
    lv_anim_set_values(&anim, 0, 72);
    lv_anim_set_duration(&anim, 520);
    lv_anim_set_path_cb(&anim, lv_anim_path_bounce);
    lv_anim_set_completed_cb(&anim, coin_done_cb);
    lv_anim_start(&anim);

    bsp_lvgl_unlock();
}
