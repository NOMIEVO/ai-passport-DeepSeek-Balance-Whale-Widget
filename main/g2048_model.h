// main/g2048_model.h —— 2048 的纯逻辑模型。
//
// 刻意不依赖 ESP-IDF、LVGL 和任何硬件:棋盘运算、计分、序列化全部是纯函数式状态
// 机,可以直接在 PC 上编译做主机测试(tests/test_g2048_model.c)。UI 与存档只读取
// 本文件暴露的状态,不反过来把显示细节塞进模型。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define G2048_SIZE  4
#define G2048_CELLS (G2048_SIZE * G2048_SIZE)

// 存档块的固定字节长度(小端显式序列化,不依赖编译器结构体对齐)。
#define G2048_SAVE_SIZE 56
// 存档头:魔数 "G248" 的 ASCII 拼接,用于识别 NVS 里是不是本应用写的块。
#define G2048_SAVE_MAGIC 0x38323447u
#define G2048_SAVE_VERSION 1u

// 达成该方块即算通关(仍可继续玩)。
#define G2048_TARGET 2048u

typedef enum {
    G2048_DIR_UP = 0,
    G2048_DIR_RIGHT,
    G2048_DIR_DOWN,
    G2048_DIR_LEFT,
    G2048_DIR_COUNT,
} g2048_dir_t;

typedef struct {
    // 0 表示空格,其余为 2 的幂。按 [行][列] 存放,行 0 在屏幕上方。
    uint16_t grid[G2048_SIZE][G2048_SIZE];
    uint32_t score;
    uint32_t moves;
    uint32_t rng_state;   // xorshift32 状态,随棋盘一起存档以保证续玩可复现
    uint16_t max_tile;
    bool won;             // 已经合出 G2048_TARGET(不结束游戏)
    bool over;            // 四个方向都动不了
    // 上一步的结果,供 UI 做动画:合并产生的格子与新生成的格子。
    bool merged[G2048_SIZE][G2048_SIZE];
    int8_t spawned_r;     // -1 表示上一步没有生成
    int8_t spawned_c;
} g2048_t;

// 用 seed 初始化一局新游戏(随机放两个方块)。seed 为 0 时内部替换成非零值。
void g2048_init(g2048_t *g, uint32_t seed);

// 从存档字节恢复;失败返回 false 且不改动 *g。长度必须等于 G2048_SAVE_SIZE。
bool g2048_load(g2048_t *g, const uint8_t *bytes, size_t len);
// 序列化到至少 G2048_SAVE_SIZE 字节的缓冲区。
void g2048_save(const g2048_t *g, uint8_t *out, size_t len);

// 沿 dir 滑动一次。成功(棋盘真的变了)返回 true 并写入本次得分增量 gained
// (可为 NULL);棋盘无变化返回 false 且状态不变,也不会生成新方块。
bool g2048_move(g2048_t *g, g2048_dir_t dir, uint32_t *gained);

// 还有任何一个方向可动即为 true(结束判定的公共入口)。
bool g2048_can_move(const g2048_t *g);

uint16_t g2048_tile(const g2048_t *g, int row, int col);

// 方向轮盘的循环顺序:UP/RIGHT/DOWN/LEFT,便于上/下键前后切换。
g2048_dir_t g2048_dir_next(g2048_dir_t dir);
g2048_dir_t g2048_dir_prev(g2048_dir_t dir);
