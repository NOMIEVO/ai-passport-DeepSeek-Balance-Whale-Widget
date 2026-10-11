// main/g2048_ui.c —— 2048 的界面实现。
//
// 这是应用自己画的界面:深色底 + 高饱和方块 + 底部方向轮盘,和基线 demo 的像素纸感
// 测试菜单没有任何共用代码(不 include ui_pixel.h,也不用它的 screen/panel 构造)。
// BSP 只提供显示与 LVGL 加锁,其余全部由本文件负责。
//
// 竖屏 240x320 的纵向分配(像素都写死在这里,改布局只改这一组常量):
//   6..26    标题栏:左"2048",右电量
//   28..64   两块信息板:分数 / 最大方块
//   66..230  4x4 棋盘(164x164,格 36,间隙 4)
//   232..252 操作提示一
//   254..288 方向轮盘(上 右 下 左)
//   290..310 操作提示二
// 屏幕四角被 BSP 裁成半径 30 的圆角,故内容横向保持在 20..220 以内。
#include "g2048_ui.h"

#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"

// 应用自带的中文字模(assets/fonts/game2048_font_16.c),含界面用到的全部汉字。
LV_FONT_DECLARE(game2048_font_16);

#define SCREEN_W 240
#define SCREEN_H 320

#define STATUS_Y      6
#define STATUS_H      20
#define TITLE_X       14
#define BATT_W        64

#define PANEL_Y       28
#define PANEL_H       36
#define PANEL1_X      14
#define PANEL2_X      122
#define PANEL_W       104

#define BOARD_X       38
#define BOARD_Y       66
#define BOARD_SIZE    164
#define CELL_SIZE     36
#define CELL_GAP      4

#define HINT1_Y       232
#define PAD_Y         254
#define PAD_H         34
#define CHIP_W        46
#define CHIP_GAP      6
#define CHIP_X0       19
#define HINT2_Y       290
#define TEXT_H        20

#define OVERLAY_X     42
#define OVERLAY_Y     98
#define OVERLAY_W     156
#define OVERLAY_H     116

#define BANNER_X      20
#define BANNER_Y      140
#define BANNER_W      200
#define BANNER_H      44

// 深色主题:棋盘用冷灰蓝衬托,方块按数值走一条自己的暖色到紫色的梯度。
#define C_BG          0x10141C
#define C_PANEL       0x1A2130
#define C_BOARD       0x202938
#define C_CELL_EMPTY  0x29313F
#define C_TEXT        0xE6ECF8
#define C_TEXT_DIM    0x8A94A6
#define C_ACCENT      0xFFB54D
#define C_HILITE      0x4C7DFF
#define C_MARK        0xF2F6FF

// 方块配色按 2 的幂排列,下标 = log2(值);超过表的走最后一个。
static const uint32_t TILE_BG[] = {
    0x3B4A63, 0x4A6A8A, 0xD98A3A, 0xDE6F3C,
    0xDC5A55, 0xCE424C, 0xE3B13F, 0xEFCC33,
    0x6FBF52, 0x3FA96B, 0x3E86E0, 0x8A5BD6,
};
static const uint32_t TILE_FG[] = {
    0xC8D2E4, 0xDCE5F2, 0xFFFFFF, 0xFFFFFF,
    0xFFFFFF, 0xFFFFFF, 0x2A1F08, 0x2A1F08,
    0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF,
};
#define TILE_LEVELS (sizeof(TILE_BG) / sizeof(TILE_BG[0]))

// 轮盘按罗盘顺序排:上 右 下 左 —— 上/下键在这个顺序上前后移动,高亮移动方向直观。
static const char *const DIR_LABELS[G2048_DIR_COUNT] = { "上", "右", "下", "左" };
static const g2048_dir_t DIR_ORDER[G2048_DIR_COUNT] = {
    G2048_DIR_UP, G2048_DIR_RIGHT, G2048_DIR_DOWN, G2048_DIR_LEFT,
};

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *battery;
    lv_obj_t *score_value;
    lv_obj_t *best_value;
    lv_obj_t *board;
    lv_obj_t *cells[G2048_CELLS];
    lv_obj_t *cell_labels[G2048_CELLS];
    lv_obj_t *hint1;
    lv_obj_t *chips[G2048_DIR_COUNT];
    lv_obj_t *chip_labels[G2048_DIR_COUNT];
    lv_obj_t *hint2;
    lv_obj_t *overlay;
    lv_obj_t *overlay_title;
    lv_obj_t *overlay_score;
    lv_obj_t *overlay_hint;
    lv_obj_t *banner;
    lv_obj_t *banner_label;
    lv_timer_t *banner_timer;
} g2048_ui_t;

static g2048_ui_t s_ui;
static bool s_ready;

// 合并方块的"弹一下"动画:直接改格子尺寸(中心不动),只用 LVGL 核心 API。
#define POP_POOL 8
typedef struct {
    lv_obj_t *obj;
    int32_t x;
    int32_t y;
    int32_t size;
} g2048_pop_t;

static g2048_pop_t s_pop[POP_POOL];

static void pop_exec_cb(void *var, int32_t value)
{
    g2048_pop_t *pop = (g2048_pop_t *)var;
    const int32_t size = pop->size * value / 256;
    const int32_t offset = (pop->size - size) / 2;
    lv_obj_set_size(pop->obj, size, size);
    lv_obj_set_pos(pop->obj, pop->x + offset, pop->y + offset);
}

// 先掐掉同一槽位上可能还在跑的动画,避免两个动画共用一块上下文互相覆盖。
static void pop_start(int slot, lv_obj_t *obj)
{
    lv_anim_delete(&s_pop[slot], NULL);
    s_pop[slot].obj = obj;
    s_pop[slot].x = lv_obj_get_x(obj);
    s_pop[slot].y = lv_obj_get_y(obj);
    s_pop[slot].size = CELL_SIZE;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &s_pop[slot]);
    lv_anim_set_exec_cb(&anim, pop_exec_cb);
    lv_anim_set_values(&anim, 170, 256);
    lv_anim_set_duration(&anim, 170);
    lv_anim_set_path_cb(&anim, lv_anim_path_overshoot);
    lv_anim_start(&anim);
}

static void pop_stop_all(void)
{
    for (int i = 0; i < POP_POOL; i++) {
        lv_anim_delete(&s_pop[i], NULL);
        s_pop[i].obj = NULL;
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, TEXT_H);
    lv_obj_set_style_text_font(label, &game2048_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    return panel;
}

static void banner_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_ui.banner != NULL) lv_obj_add_flag(s_ui.banner, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_ui.banner_timer);
}

bool g2048_ui_init(void)
{
    if (s_ready) return true;
    if (!bsp_lvgl_lock(1000)) return false;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    // 字体属性会继承:整屏设一次,后面除棋盘数字(单独指定 Montserrat)外都用它。
    lv_obj_set_style_text_font(scr, &game2048_font_16, 0);
    s_ui.screen = scr;

    s_ui.title = make_label(scr, TITLE_X, STATUS_Y, 100, "2048");
    lv_obj_set_style_text_font(s_ui.title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ui.title, lv_color_hex(C_ACCENT), 0);

    s_ui.battery = make_label(scr, SCREEN_W - BATT_W - 14, STATUS_Y, BATT_W, "--");
    lv_obj_set_style_text_align(s_ui.battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_ui.battery, lv_color_hex(C_TEXT_DIM), 0);

    // 分数板
    lv_obj_t *panel1 = make_panel(scr, PANEL1_X, PANEL_Y, PANEL_W, PANEL_H);
    lv_obj_t *score_caption = make_label(panel1, 8, 8, 40, "分数");
    lv_obj_set_style_text_color(score_caption, lv_color_hex(C_TEXT_DIM), 0);
    s_ui.score_value = make_label(panel1, PANEL_W - 8 - 64, 6, 64, "0");
    lv_obj_set_style_text_font(s_ui.score_value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ui.score_value, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_align(s_ui.score_value, LV_TEXT_ALIGN_RIGHT, 0);

    // 最大方块板
    lv_obj_t *panel2 = make_panel(scr, PANEL2_X, PANEL_Y, PANEL_W, PANEL_H);
    lv_obj_t *best_caption = make_label(panel2, 8, 8, 40, "最大");
    lv_obj_set_style_text_color(best_caption, lv_color_hex(C_TEXT_DIM), 0);
    s_ui.best_value = make_label(panel2, PANEL_W - 8 - 64, 6, 64, "0");
    lv_obj_set_style_text_font(s_ui.best_value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ui.best_value, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_align(s_ui.best_value, LV_TEXT_ALIGN_RIGHT, 0);

    // 棋盘与 16 个格子
    s_ui.board = lv_obj_create(scr);
    lv_obj_set_pos(s_ui.board, BOARD_X, BOARD_Y);
    lv_obj_set_size(s_ui.board, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_style_bg_color(s_ui.board, lv_color_hex(C_BOARD), 0);
    lv_obj_set_style_bg_opa(s_ui.board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.board, 0, 0);
    lv_obj_set_style_radius(s_ui.board, 10, 0);
    lv_obj_set_style_pad_all(s_ui.board, 0, 0);

    for (int row = 0; row < G2048_SIZE; row++) {
        for (int col = 0; col < G2048_SIZE; col++) {
            const int index = row * G2048_SIZE + col;
            const int x = BOARD_X + CELL_GAP + col * (CELL_SIZE + CELL_GAP);
            const int y = BOARD_Y + CELL_GAP + row * (CELL_SIZE + CELL_GAP);

            lv_obj_t *cell = lv_obj_create(scr);
            lv_obj_set_pos(cell, x, y);
            lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
            lv_obj_set_style_bg_color(cell, lv_color_hex(C_CELL_EMPTY), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 8, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            s_ui.cells[index] = cell;

            lv_obj_t *label = lv_label_create(cell);
            lv_obj_set_size(label, CELL_SIZE, CELL_SIZE);
            lv_obj_set_pos(label, 0, 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(C_TEXT), 0);
            // 数字是居中显示且定宽的,直接按中心对齐即可,不需要额外容器。
            lv_label_set_text(label, "");
            s_ui.cell_labels[index] = label;
        }
    }

    s_ui.hint1 = make_label(scr, 14, HINT1_Y, SCREEN_W - 28, "上下键选方向，确定键滑动");
    lv_obj_set_style_text_align(s_ui.hint1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ui.hint1, lv_color_hex(C_TEXT_DIM), 0);

    for (int i = 0; i < G2048_DIR_COUNT; i++) {
        const int x = CHIP_X0 + i * (CHIP_W + CHIP_GAP);
        lv_obj_t *chip = lv_obj_create(scr);
        lv_obj_set_pos(chip, x, PAD_Y);
        lv_obj_set_size(chip, CHIP_W, PAD_H);
        lv_obj_set_style_bg_color(chip, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 2, 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_radius(chip, 8, 0);
        lv_obj_set_style_pad_all(chip, 0, 0);
        s_ui.chips[i] = chip;

        lv_obj_t *chip_label = lv_label_create(chip);
        lv_obj_set_size(chip_label, CHIP_W, PAD_H);
        lv_obj_set_pos(chip_label, 0, 0);
        lv_obj_set_style_text_align(chip_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(chip_label, &game2048_font_16, 0);
        lv_obj_set_style_text_color(chip_label, lv_color_hex(C_TEXT_DIM), 0);
        lv_label_set_text(chip_label, DIR_LABELS[i]);
        s_ui.chip_labels[i] = chip_label;
    }

    s_ui.hint2 = make_label(scr, 14, HINT2_Y, SCREEN_W - 28, "长按确定键 重新开始");
    lv_obj_set_style_text_align(s_ui.hint2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ui.hint2, lv_color_hex(C_TEXT_DIM), 0);

    // 结算浮层:盖在棋盘上,默认隐藏。
    s_ui.overlay = lv_obj_create(scr);
    lv_obj_set_pos(s_ui.overlay, OVERLAY_X, OVERLAY_Y);
    lv_obj_set_size(s_ui.overlay, OVERLAY_W, OVERLAY_H);
    lv_obj_set_style_bg_color(s_ui.overlay, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_ui.overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_ui.overlay, 2, 0);
    lv_obj_set_style_border_color(s_ui.overlay, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(s_ui.overlay, 12, 0);
    lv_obj_set_style_pad_all(s_ui.overlay, 0, 0);
    lv_obj_add_flag(s_ui.overlay, LV_OBJ_FLAG_HIDDEN);

    s_ui.overlay_title = make_label(s_ui.overlay, 8, 12, OVERLAY_W - 16, "无路可走了");
    lv_obj_set_style_text_align(s_ui.overlay_title, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.overlay_score = make_label(s_ui.overlay, 8, 44, OVERLAY_W - 16, "得分 0");
    lv_obj_set_style_text_align(s_ui.overlay_score, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ui.overlay_score, lv_color_hex(C_ACCENT), 0);
    s_ui.overlay_hint = make_label(s_ui.overlay, 8, 78, OVERLAY_W - 16, "确定键再来一局");
    lv_obj_set_style_text_align(s_ui.overlay_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ui.overlay_hint, lv_color_hex(C_TEXT_DIM), 0);

    // 通关提示条:默认隐藏,靠暂停的定时器计时,不重复创建/删除定时器。
    s_ui.banner = lv_obj_create(scr);
    lv_obj_set_pos(s_ui.banner, BANNER_X, BANNER_Y);
    lv_obj_set_size(s_ui.banner, BANNER_W, BANNER_H);
    lv_obj_set_style_bg_color(s_ui.banner, lv_color_hex(C_HILITE), 0);
    lv_obj_set_style_bg_opa(s_ui.banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.banner, 0, 0);
    lv_obj_set_style_radius(s_ui.banner, 10, 0);
    lv_obj_set_style_pad_all(s_ui.banner, 0, 0);
    lv_obj_add_flag(s_ui.banner, LV_OBJ_FLAG_HIDDEN);

    s_ui.banner_label = make_label(s_ui.banner, 8, 12, BANNER_W - 16, "恭喜达成 2048！");
    lv_obj_set_style_text_align(s_ui.banner_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ui.banner_label, lv_color_hex(0xFFFFFF), 0);

    s_ui.banner_timer = lv_timer_create(banner_timer_cb, 2000, NULL);
    lv_timer_pause(s_ui.banner_timer);

    for (int i = 0; i < POP_POOL; i++) s_pop[i].obj = NULL;

    lv_screen_load(scr);
    bsp_lvgl_unlock();
    s_ready = true;
    return true;
}

void g2048_ui_deinit(void)
{
    if (!s_ready) return;
    if (bsp_lvgl_lock(1000)) {
        pop_stop_all();
        if (s_ui.banner_timer != NULL) {
            lv_timer_delete(s_ui.banner_timer);
            s_ui.banner_timer = NULL;
        }
        if (s_ui.screen != NULL) {
            lv_obj_delete(s_ui.screen);
        }
        bsp_lvgl_unlock();
    }
    for (int i = 0; i < POP_POOL; i++) s_pop[i].obj = NULL;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ready = false;
}

// 按数值挑字体:位数越多字越小,保证 4 位数字也能塞进 36px 的格子。
static void apply_tile_font(lv_obj_t *label, uint16_t value)
{
    const lv_font_t *font = &lv_font_montserrat_24;
    if (value >= 1000u) {
        font = &lv_font_montserrat_14;
    } else if (value >= 100u) {
        font = &lv_font_montserrat_20;
    }
    lv_obj_set_style_text_font(label, font, 0);
}

static int tile_level(uint16_t value)
{
    int level = 0;
    while (value > 1u && level < (int)TILE_LEVELS - 1) {
        value >>= 1;
        level++;
    }
    return level;
}

void g2048_ui_render(const g2048_t *game)
{
    if (!s_ready || game == NULL) return;
    if (!bsp_lvgl_lock(500)) return;

    for (int row = 0; row < G2048_SIZE; row++) {
        for (int col = 0; col < G2048_SIZE; col++) {
            const int index = row * G2048_SIZE + col;
            const uint16_t value = g2048_tile(game, row, col);
            lv_obj_t *cell = s_ui.cells[index];
            lv_obj_t *label = s_ui.cell_labels[index];

            if (value == 0u) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(C_CELL_EMPTY), 0);
                lv_obj_set_style_border_width(cell, 0, 0);
                lv_label_set_text(label, "");
                continue;
            }

            const int level = tile_level(value);
            lv_obj_set_style_bg_color(cell, lv_color_hex(TILE_BG[level]), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(TILE_FG[level]), 0);
            apply_tile_font(label, value);
            lv_label_set_text_fmt(label, "%u", (unsigned)value);

            // 上一步合并出来的格子描一圈亮边,新生成的格子描一圈强调色,
            // 都是同步改样式,不需要额外定时器。
            if (game->merged[row][col]) {
                lv_obj_set_style_border_width(cell, 2, 0);
                lv_obj_set_style_border_color(cell, lv_color_hex(C_MARK), 0);
            } else if (game->spawned_r == row && game->spawned_c == col) {
                lv_obj_set_style_border_width(cell, 2, 0);
                lv_obj_set_style_border_color(cell, lv_color_hex(C_ACCENT), 0);
            } else {
                lv_obj_set_style_border_width(cell, 0, 0);
            }
        }
    }

    // 合并的格子弹一下:最多 8 个槽位,超出的不再动画(不影响正确性)。
    int slot = 0;
    for (int row = 0; row < G2048_SIZE && slot < POP_POOL; row++) {
        for (int col = 0; col < G2048_SIZE && slot < POP_POOL; col++) {
            if (game->merged[row][col]) {
                pop_start(slot, s_ui.cells[row * G2048_SIZE + col]);
                slot++;
            }
        }
    }

    lv_label_set_text_fmt(s_ui.score_value, "%lu", (unsigned long)game->score);
    lv_label_set_text_fmt(s_ui.best_value, "%u", (unsigned)game->max_tile);

    if (game->over) {
        lv_label_set_text_fmt(s_ui.overlay_score, "得分 %lu", (unsigned long)game->score);
        lv_obj_remove_flag(s_ui.overlay, LV_OBJ_FLAG_HIDDEN);
        // 结束时把操作提示换成"再来一局",避免和浮层里的说明重复。
        lv_label_set_text(s_ui.hint2, "长按确定键 再来一局");
    } else {
        lv_obj_add_flag(s_ui.overlay, LV_OBJ_FLAG_HIDDEN);
    }

    bsp_lvgl_unlock();
}

void g2048_ui_set_direction(g2048_dir_t dir)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(500)) return;

    for (int i = 0; i < G2048_DIR_COUNT; i++) {
        const bool selected = (DIR_ORDER[i] == dir);
        lv_obj_set_style_bg_color(s_ui.chips[i],
                                  lv_color_hex(selected ? C_HILITE : C_PANEL), 0);
        lv_obj_set_style_border_color(s_ui.chips[i],
                                      lv_color_hex(selected ? C_TEXT : C_PANEL), 0);
        lv_obj_set_style_text_color(s_ui.chip_labels[i],
                                    lv_color_hex(selected ? 0xFFFFFF : C_TEXT_DIM), 0);
    }
    bsp_lvgl_unlock();
}

void g2048_ui_show_win_banner(void)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_ui.banner_label, "恭喜达成 2048！");
    lv_obj_remove_flag(s_ui.banner, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_ui.banner_timer);
    lv_timer_resume(s_ui.banner_timer);
    bsp_lvgl_unlock();
}

void g2048_ui_hide_banner(void)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_obj_add_flag(s_ui.banner, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_ui.banner_timer);
    bsp_lvgl_unlock();
}

void g2048_ui_show_sleep_notice(void)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_ui.banner_label, "即将休眠");
    lv_obj_remove_flag(s_ui.banner, LV_OBJ_FLAG_HIDDEN);
    // 休眠提示不能自己消失:定时器保持暂停,直到真正睡下去。
    lv_timer_pause(s_ui.banner_timer);
    bsp_lvgl_unlock();
}

void g2048_ui_refresh_battery(void)
{
    if (!s_ready) return;
    const int soc = bsp_battery_soc();
    if (!bsp_lvgl_lock(500)) return;
    if (soc < 0) {
        lv_label_set_text(s_ui.battery, "--");
    } else {
        lv_label_set_text_fmt(s_ui.battery, "%d%%", soc);
    }
    bsp_lvgl_unlock();
}
