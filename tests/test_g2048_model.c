// 2048 模型的主机测试:完全不依赖 ESP-IDF/LVGL,验证棋盘规则、计分、结束判定
// 与存档序列化。编译方式见 tools/validate.sh(--std=c11 -Wall -Wextra -Werror)。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "g2048_model.h"

static int count_tiles(const g2048_t *g)
{
    int count = 0;
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            if (g->grid[r][c] != 0u) count++;
        }
    }
    return count;
}

static void set_row(g2048_t *g, int row, uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    g->grid[row][0] = a;
    g->grid[row][1] = b;
    g->grid[row][2] = c;
    g->grid[row][3] = d;
}

static void test_initial_state(void)
{
    g2048_t g;
    g2048_init(&g, 12345u);
    assert(g.score == 0u);
    assert(g.moves == 0u);
    assert(!g.won && !g.over);
    assert(count_tiles(&g) == 2);
    // 开局两格的取值只能是 2 或 4。
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            const uint16_t v = g.grid[r][c];
            assert(v == 0u || v == 2u || v == 4u);
        }
    }
}

// 同一 seed 必须走出同一局,否则"掉电续玩"无法复现。
static void test_seed_is_deterministic(void)
{
    g2048_t a;
    g2048_t b;
    g2048_init(&a, 0xABCDEFu);
    g2048_init(&b, 0xABCDEFu);
    assert(memcmp(a.grid, b.grid, sizeof(a.grid)) == 0);

    g2048_move(&a, G2048_DIR_LEFT, NULL);
    g2048_move(&b, G2048_DIR_LEFT, NULL);
    assert(memcmp(a.grid, b.grid, sizeof(a.grid)) == 0);
    assert(a.score == b.score);

    // seed 为 0 时内部替换成非零值,不能让 xorshift 卡死在 0。
    g2048_t z;
    g2048_init(&z, 0u);
    assert(z.rng_state != 0u);
    assert(count_tiles(&z) == 2);
}

static void test_merge_left_and_score(void)
{
    g2048_t g;
    g2048_init(&g, 1u);
    set_row(&g, 0, 2, 2, 2, 2);
    set_row(&g, 1, 4, 8, 4, 8);
    set_row(&g, 2, 2, 4, 2, 4);
    set_row(&g, 3, 8, 2, 8, 2);
    g.score = 0u;

    uint32_t gained = 0u;
    assert(g2048_move(&g, G2048_DIR_LEFT, &gained));
    // [2 2 2 2] → [4 4],一次移动内每个方块只合并一次。
    assert(g.grid[0][0] == 4u);
    assert(g.grid[0][1] == 4u);
    assert(gained == 8u);
    assert(g.score == 8u);
    assert(g.moves == 1u);
    // 其余三行不动,生成的新方块只可能落在第 0 行剩下的两格里。
    assert(g.grid[1][0] == 4u && g.grid[1][1] == 8u);
    assert((g.grid[0][2] == 0u) != (g.grid[0][3] == 0u));
    assert(count_tiles(&g) == 15);
    // 合并位置要能被 UI 用来做动画。
    assert(g.merged[0][0] || g.merged[0][1]);
}

static void test_merge_happens_once_per_tile(void)
{
    g2048_t g;
    g2048_init(&g, 2u);
    set_row(&g, 0, 4, 2, 2, 0);
    set_row(&g, 1, 4, 8, 4, 8);
    set_row(&g, 2, 2, 4, 2, 4);
    set_row(&g, 3, 8, 2, 8, 2);
    g.score = 0u;

    uint32_t gained = 0u;
    assert(g2048_move(&g, G2048_DIR_LEFT, &gained));
    // [4 2 2 0] → [4 4 0 0]:领先的 4 不参与后面的合并。
    assert(g.grid[0][0] == 4u);
    assert(g.grid[0][1] == 4u);
    assert(gained == 4u);
}

static void test_no_move_keeps_state(void)
{
    g2048_t g;
    g2048_init(&g, 3u);
    // 交错棋盘:任何方向都既没有空格也没有相邻同值,必然无路可走。
    set_row(&g, 0, 2, 4, 2, 4);
    set_row(&g, 1, 4, 2, 4, 2);
    set_row(&g, 2, 2, 4, 2, 4);
    set_row(&g, 3, 4, 2, 4, 2);
    g.score = 100u;

    assert(!g2048_can_move(&g));
    const int before = count_tiles(&g);
    assert(!g2048_move(&g, G2048_DIR_LEFT, NULL));
    assert(!g2048_move(&g, G2048_DIR_RIGHT, NULL));
    assert(!g2048_move(&g, G2048_DIR_UP, NULL));
    assert(!g2048_move(&g, G2048_DIR_DOWN, NULL));
    // 无效移动不得生成方块、不得改分数和步数。
    assert(count_tiles(&g) == before);
    assert(g.score == 100u);
    assert(g.moves == 0u);
}

static void test_direction_mapping(void)
{
    // 棋盘上只放一个方块时,移动后它必定贴在目标边;生成的新方块不改变这一点。
    const struct {
        g2048_dir_t dir;
        int row;
        int col;
        // 起点 [1][1]:贴到目标边后应落在这些坐标上。
    } cases[] = {
        { G2048_DIR_UP,    0, 1 },
        { G2048_DIR_DOWN,  3, 1 },
        { G2048_DIR_LEFT,  1, 0 },
        { G2048_DIR_RIGHT, 1, 3 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g2048_t g;
        g2048_init(&g, 7u);
        for (int r = 0; r < G2048_SIZE; r++) {
            for (int c = 0; c < G2048_SIZE; c++) g.grid[r][c] = 0u;
        }
        g.grid[1][1] = 2u;
        assert(g2048_move(&g, cases[i].dir, NULL));
        assert(g.grid[cases[i].row][cases[i].col] == 2u);
        assert(count_tiles(&g) == 2);
    }

    // 只放一个方块且已经贴边时,该方向不产生移动。
    g2048_t g;
    g2048_init(&g, 8u);
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) g.grid[r][c] = 0u;
    }
    g.grid[0][0] = 2u;
    assert(!g2048_move(&g, G2048_DIR_UP, NULL));
    assert(!g2048_move(&g, G2048_DIR_LEFT, NULL));
}

static void test_win_and_max_tile(void)
{
    g2048_t g;
    g2048_init(&g, 9u);
    set_row(&g, 0, 1024, 1024, 0, 0);
    set_row(&g, 1, 2, 4, 2, 4);
    set_row(&g, 2, 4, 2, 4, 2);
    set_row(&g, 3, 2, 4, 2, 4);
    g.score = 0u;
    g.won = false;

    uint32_t gained = 0u;
    assert(g2048_move(&g, G2048_DIR_LEFT, &gained));
    assert(g.grid[0][0] == G2048_TARGET);
    assert(g.won);
    assert(g.max_tile == G2048_TARGET);
    assert(gained == G2048_TARGET);
    // 通关不是结束:还能继续玩。
    assert(!g.over || !g2048_can_move(&g));
}

static void test_game_over_flag(void)
{
    // 交错棋盘:格子全满且没有相邻同值。
    g2048_t stuck;
    g2048_init(&stuck, 10u);
    set_row(&stuck, 0, 2, 4, 2, 4);
    set_row(&stuck, 1, 4, 2, 4, 2);
    set_row(&stuck, 2, 2, 4, 2, 4);
    set_row(&stuck, 3, 4, 2, 4, 2);
    assert(!g2048_can_move(&stuck));

    // 只要留出一个空格就一定还能动(可以滑向空位)。
    stuck.grid[3][3] = 0u;
    assert(g2048_can_move(&stuck));
    // 填成与左邻、上邻都不同的值,又回到无路可走。
    stuck.grid[3][3] = 8u;
    assert(!g2048_can_move(&stuck));
}

// 真机上一局会走几百步,这里跑一整局验证"结束标记"与"还能不能动"始终一致。
static void test_play_until_game_over(void)
{
    g2048_t g;
    g2048_init(&g, 0xC0FFEEu);
    int steps = 0;
    for (;;) {
        bool moved = false;
        for (int d = 0; d < (int)G2048_DIR_COUNT; d++) {
            if (g2048_move(&g, (g2048_dir_t)d, NULL)) {
                moved = true;
                break;
            }
        }
        if (!moved) break;
        // 每一步之后,结束标记都必须等价于"四个方向都动不了"。
        assert(g.over == !g2048_can_move(&g));
        assert(g.max_tile >= 2u);
        steps++;
        assert(steps < 100000);
    }
    assert(g.over);
    assert(!g2048_can_move(&g));
    // 无路可走时棋盘必然是满的:只要有空格就能滑过去。
    assert(count_tiles(&g) == G2048_CELLS);
    assert(g.score > 0u);
    printf("  (随机对局 %d 步后结束,得分 %lu,最大方块 %u)\n",
           steps, (unsigned long)g.score, (unsigned)g.max_tile);
}

static void test_merge_limit_prevents_overflow(void)
{
    g2048_t g;
    g2048_init(&g, 11u);
    set_row(&g, 0, 32768, 32768, 0, 0);
    set_row(&g, 1, 2, 4, 2, 4);
    set_row(&g, 2, 4, 2, 4, 2);
    set_row(&g, 3, 2, 4, 2, 4);
    // 合并会溢出 uint16,故按上限拒绝合并:这一格动不了,整步也不算移动。
    assert(!g2048_move(&g, G2048_DIR_LEFT, NULL));
    assert(g.grid[0][0] == 32768u);
    assert(g.grid[0][1] == 32768u);
}

static void test_save_round_trip(void)
{
    g2048_t g;
    g2048_init(&g, 0x13579BDFu);
    g2048_move(&g, G2048_DIR_LEFT, NULL);
    g2048_move(&g, G2048_DIR_UP, NULL);
    g2048_move(&g, G2048_DIR_RIGHT, NULL);

    uint8_t blob[G2048_SAVE_SIZE];
    memset(blob, 0, sizeof(blob));
    g2048_save(&g, blob, sizeof(blob));

    g2048_t restored;
    memset(&restored, 0, sizeof(restored));
    assert(g2048_load(&restored, blob, sizeof(blob)));
    assert(restored.score == g.score);
    assert(restored.moves == g.moves);
    assert(restored.rng_state == g.rng_state);
    assert(restored.won == g.won);
    assert(restored.over == g.over);
    assert(memcmp(restored.grid, g.grid, sizeof(g.grid)) == 0);

    // 续玩必须产生同样的后续局面。
    g2048_move(&g, G2048_DIR_DOWN, NULL);
    g2048_move(&restored, G2048_DIR_DOWN, NULL);
    assert(memcmp(restored.grid, g.grid, sizeof(g.grid)) == 0);
    assert(restored.score == g.score);
}

static void test_save_layout_is_fixed(void)
{
    assert(G2048_SAVE_SIZE == 56u);
    g2048_t g;
    g2048_init(&g, 4u);
    uint8_t blob[G2048_SAVE_SIZE];
    g2048_save(&g, blob, sizeof(blob));
    // 小端写出的魔数 "G248",以及版本字段。
    assert(blob[0] == 0x47u && blob[1] == 0x34u && blob[2] == 0x32u && blob[3] == 0x38u);
    assert(blob[4] == 1u && blob[5] == 0u && blob[6] == 0u && blob[7] == 0u);

    // 缓冲区不足时不得越界写入(调用直接返回)。
    uint8_t small[G2048_SAVE_SIZE - 1];
    memset(small, 0xAA, sizeof(small));
    g2048_save(&g, small, sizeof(small));
    for (size_t i = 0; i < sizeof(small); i++) assert(small[i] == 0xAAu);
}

static void test_load_rejects_bad_blobs(void)
{
    g2048_t g;
    g2048_init(&g, 5u);
    uint8_t blob[G2048_SAVE_SIZE];
    g2048_save(&g, blob, sizeof(blob));

    g2048_t out;
    g2048_init(&out, 6u);

    // 长度不足
    assert(!g2048_load(&out, blob, G2048_SAVE_SIZE - 1));
    // 魔数不对
    uint8_t bad[G2048_SAVE_SIZE];
    memcpy(bad, blob, sizeof(bad));
    bad[0] = 0x00u;
    assert(!g2048_load(&out, bad, sizeof(bad)));
    // 版本不对
    memcpy(bad, blob, sizeof(bad));
    bad[4] = 0x7Fu;
    assert(!g2048_load(&out, bad, sizeof(bad)));
    // 非法方块值(3 不是 2 的幂)
    memcpy(bad, blob, sizeof(bad));
    bad[24] = 3u;
    bad[25] = 0u;
    assert(!g2048_load(&out, bad, sizeof(bad)));
    // 空指针与 NULL 输入
    assert(!g2048_load(&out, NULL, G2048_SAVE_SIZE));
}

static void test_dir_cycle(void)
{
    assert(g2048_dir_next(G2048_DIR_UP) == G2048_DIR_RIGHT);
    assert(g2048_dir_next(G2048_DIR_RIGHT) == G2048_DIR_DOWN);
    assert(g2048_dir_next(G2048_DIR_DOWN) == G2048_DIR_LEFT);
    assert(g2048_dir_next(G2048_DIR_LEFT) == G2048_DIR_UP);

    assert(g2048_dir_prev(G2048_DIR_UP) == G2048_DIR_LEFT);
    assert(g2048_dir_prev(G2048_DIR_LEFT) == G2048_DIR_DOWN);
    assert(g2048_dir_prev(G2048_DIR_DOWN) == G2048_DIR_RIGHT);
    assert(g2048_dir_prev(G2048_DIR_RIGHT) == G2048_DIR_UP);

    for (int i = 0; i < (int)G2048_DIR_COUNT; i++) {
        const g2048_dir_t d = (g2048_dir_t)i;
        assert(g2048_dir_prev(g2048_dir_next(d)) == d);
    }
}

static void test_tile_accessor_bounds(void)
{
    g2048_t g;
    g2048_init(&g, 12u);
    g.grid[2][3] = 64u;
    assert(g2048_tile(&g, 2, 3) == 64u);
    assert(g2048_tile(&g, -1, 0) == 0u);
    assert(g2048_tile(&g, 0, G2048_SIZE) == 0u);
    assert(g2048_tile(NULL, 0, 0) == 0u);
}

int main(void)
{
    test_initial_state();
    test_seed_is_deterministic();
    test_merge_left_and_score();
    test_merge_happens_once_per_tile();
    test_no_move_keeps_state();
    test_direction_mapping();
    test_win_and_max_tile();
    test_game_over_flag();
    test_play_until_game_over();
    test_merge_limit_prevents_overflow();
    test_save_round_trip();
    test_save_layout_is_fixed();
    test_load_rejects_bad_blobs();
    test_dir_cycle();
    test_tile_accessor_bounds();
    printf("g2048 model tests: PASS\n");
    return 0;
}
