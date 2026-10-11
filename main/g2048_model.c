// main/g2048_model.c —— 2048 纯逻辑实现,不触碰任何硬件或 UI。
//
// 棋盘按 [行][列] 存放,行 0 在屏幕上方。四个方向的移动复用同一条"抽取一条线 →
// 压缩合并 → 写回"的流水线:方向只决定这条线取哪 4 个格子、按什么顺序取,合并
// 规则本身与方向无关,因此只需要一处实现,测试也只需要覆盖这一处。
#include "g2048_model.h"

// 一次移动里单个方块允许合并的上限:值超过它就不合并,避免 uint16 溢出。
// 4x4 棋盘上 32768 已是极限附近,该分支在正常对局中走不到。
#define G2048_MERGE_LIMIT 16384u

static void put_u16(uint8_t *out, size_t offset, uint16_t value)
{
    out[offset] = (uint8_t)(value & 0xFFu);
    out[offset + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_u32(uint8_t *out, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4; i++) {
        out[offset + i] = (uint8_t)((value >> (8 * i)) & 0xFFu);
    }
}

static uint16_t get_u16(const uint8_t *in, size_t offset)
{
    return (uint16_t)((uint16_t)in[offset] | ((uint16_t)in[offset + 1] << 8));
}

static uint32_t get_u32(const uint8_t *in, size_t offset)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; i++) {
        value |= ((uint32_t)in[offset + i]) << (8 * i);
    }
    return value;
}

static uint32_t rng_next(g2048_t *g)
{
    // xorshift32:状态自 0 无法逃逸,先兜一个非零值。
    if (g->rng_state == 0u) g->rng_state = 0x6D2B79F5u;
    uint32_t x = g->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng_state = x;
    return x;
}

static bool is_valid_tile(uint16_t value)
{
    return value == 0u || (value >= 2u && value <= 32768u && (value & (value - 1u)) == 0u);
}

// 把一条 4 格的线按 2048 规则压缩合并。in 的顺序就是"移动方向上的先后",
// 例如向上移动时 in[0] 是最上面一格。merged_out 标出哪些结果是合并产生的。
static bool collapse_line(const uint16_t *in, uint16_t *out, bool *merged_out, uint32_t *gain)
{
    uint16_t packed[G2048_SIZE];
    int n = 0;
    for (int i = 0; i < G2048_SIZE; i++) {
        if (in[i] != 0u) packed[n++] = in[i];
    }

    uint16_t result[G2048_SIZE];
    bool merged[G2048_SIZE];
    int m = 0;
    for (int i = 0; i < G2048_SIZE; i++) {
        result[i] = 0u;
        merged[i] = false;
    }
    for (int i = 0; i < n; i++) {
        // 每个方块一次移动内最多参与一次合并,合并后跳过被吃掉的那一格。
        if (i + 1 < n && packed[i] == packed[i + 1] && packed[i] <= G2048_MERGE_LIMIT) {
            uint16_t value = (uint16_t)(packed[i] * 2u);
            result[m] = value;
            merged[m] = true;
            *gain += value;
            m++;
            i++;
        } else {
            result[m] = packed[i];
            m++;
        }
    }

    bool changed = false;
    for (int i = 0; i < G2048_SIZE; i++) {
        if (result[i] != in[i]) changed = true;
        out[i] = result[i];
        if (merged_out != NULL) merged_out[i] = merged[i];
    }
    return changed;
}

// 取出第 idx 条线在方向 dir 上的 4 个格子(返回摊平下标,顺序即移动方向上的先后)。
static void line_indices(g2048_dir_t dir, int idx, int out[G2048_SIZE])
{
    for (int i = 0; i < G2048_SIZE; i++) {
        int row;
        int col;
        switch (dir) {
        case G2048_DIR_UP:
            row = i;
            col = idx;
            break;
        case G2048_DIR_DOWN:
            row = G2048_SIZE - 1 - i;
            col = idx;
            break;
        case G2048_DIR_LEFT:
            row = idx;
            col = i;
            break;
        case G2048_DIR_RIGHT:
        default:
            row = idx;
            col = G2048_SIZE - 1 - i;
            break;
        }
        out[i] = row * G2048_SIZE + col;
    }
}

static void clear_step_marks(g2048_t *g)
{
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) g->merged[r][c] = false;
    }
    g->spawned_r = -1;
    g->spawned_c = -1;
}

// 在随机一个空格上生成新方块,标准 2048 的 90%/10% 取 2/4。
static bool spawn_tile(g2048_t *g)
{
    int empty[G2048_CELLS];
    int count = 0;
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            if (g->grid[r][c] == 0u) empty[count++] = r * G2048_SIZE + c;
        }
    }
    if (count == 0) return false;

    const int pick = (int)(rng_next(g) % (uint32_t)count);
    const int flat = empty[pick];
    const int row = flat / G2048_SIZE;
    const int col = flat % G2048_SIZE;
    g->grid[row][col] = (rng_next(g) % 10u == 0u) ? 4u : 2u;
    g->spawned_r = (int8_t)row;
    g->spawned_c = (int8_t)col;
    return true;
}

static void refresh_stats(g2048_t *g)
{
    uint16_t max_tile = 0u;
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            if (g->grid[r][c] > max_tile) max_tile = g->grid[r][c];
        }
    }
    g->max_tile = max_tile;
    if (max_tile >= (uint16_t)G2048_TARGET) g->won = true;
    g->over = !g2048_can_move(g);
}

void g2048_init(g2048_t *g, uint32_t seed)
{
    if (g == NULL) return;
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) g->grid[r][c] = 0u;
    }
    g->score = 0u;
    g->moves = 0u;
    g->rng_state = (seed == 0u) ? 0x6D2B79F5u : seed;
    g->max_tile = 0u;
    g->won = false;
    g->over = false;
    clear_step_marks(g);

    // 开局两格,都记在 spawned_* 上,方便 UI 一进来就做出现动画。
    spawn_tile(g);
    spawn_tile(g);
}

bool g2048_move(g2048_t *g, g2048_dir_t dir, uint32_t *gained)
{
    if (g == NULL || g->over) return false;
    if (dir != G2048_DIR_UP && dir != G2048_DIR_RIGHT &&
        dir != G2048_DIR_DOWN && dir != G2048_DIR_LEFT) {
        return false;
    }

    uint16_t current[G2048_CELLS];
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) current[r * G2048_SIZE + c] = g->grid[r][c];
    }

    uint16_t next[G2048_CELLS];
    bool merged_flat[G2048_CELLS];
    for (int i = 0; i < G2048_CELLS; i++) {
        next[i] = 0u;
        merged_flat[i] = false;
    }

    uint32_t gain = 0u;
    bool changed = false;
    for (int idx = 0; idx < G2048_SIZE; idx++) {
        int ids[G2048_SIZE];
        line_indices(dir, idx, ids);
        uint16_t in[G2048_SIZE];
        uint16_t out[G2048_SIZE];
        bool merged[G2048_SIZE];
        for (int i = 0; i < G2048_SIZE; i++) in[i] = current[ids[i]];
        if (collapse_line(in, out, merged, &gain)) changed = true;
        for (int i = 0; i < G2048_SIZE; i++) {
            next[ids[i]] = out[i];
            merged_flat[ids[i]] = merged[i];
        }
    }

    // 一步都没动:不生成新方块,也不计入步数,UI 据此给出"这个方向动不了"的反馈。
    if (!changed) return false;

    clear_step_marks(g);
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            g->grid[r][c] = next[r * G2048_SIZE + c];
            g->merged[r][c] = merged_flat[r * G2048_SIZE + c];
        }
    }
    g->score += gain;
    g->moves++;
    if (gained != NULL) *gained = gain;

    spawn_tile(g);
    refresh_stats(g);
    return true;
}

bool g2048_can_move(const g2048_t *g)
{
    if (g == NULL) return false;
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            const uint16_t value = g->grid[r][c];
            if (value == 0u) return true;
            if (r + 1 < G2048_SIZE && g->grid[r + 1][c] == value) return true;
            if (c + 1 < G2048_SIZE && g->grid[r][c + 1] == value) return true;
        }
    }
    return false;
}

uint16_t g2048_tile(const g2048_t *g, int row, int col)
{
    if (g == NULL || row < 0 || row >= G2048_SIZE || col < 0 || col >= G2048_SIZE) {
        return 0u;
    }
    return g->grid[row][col];
}

void g2048_save(const g2048_t *g, uint8_t *out, size_t len)
{
    if (g == NULL || out == NULL || len < G2048_SAVE_SIZE) return;

    put_u32(out, 0, G2048_SAVE_MAGIC);
    put_u32(out, 4, G2048_SAVE_VERSION);
    put_u32(out, 8, g->score);
    put_u32(out, 12, g->moves);
    put_u32(out, 16, g->rng_state);
    put_u16(out, 20, g->max_tile);
    out[22] = g->won ? 1u : 0u;
    out[23] = g->over ? 1u : 0u;
    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            put_u16(out, 24 + (size_t)(r * G2048_SIZE + c) * 2u, g->grid[r][c]);
        }
    }
}

bool g2048_load(g2048_t *g, const uint8_t *bytes, size_t len)
{
    if (g == NULL || bytes == NULL || len < G2048_SAVE_SIZE) return false;
    if (get_u32(bytes, 0) != G2048_SAVE_MAGIC) return false;
    if (get_u32(bytes, 4) != G2048_SAVE_VERSION) return false;

    g2048_t restored;
    restored.score = get_u32(bytes, 8);
    restored.moves = get_u32(bytes, 12);
    restored.rng_state = get_u32(bytes, 16);
    restored.max_tile = get_u16(bytes, 20);
    restored.won = bytes[22] != 0u;
    restored.over = bytes[23] != 0u;

    for (int r = 0; r < G2048_SIZE; r++) {
        for (int c = 0; c < G2048_SIZE; c++) {
            const uint16_t value = get_u16(bytes, 24 + (size_t)(r * G2048_SIZE + c) * 2u);
            // 存档被别的版本写坏时宁可拒收,也不要把非法值画到屏幕上。
            if (!is_valid_tile(value)) return false;
            restored.grid[r][c] = value;
        }
    }
    clear_step_marks(&restored);

    *g = restored;
    // 分数/最大方块/结束状态一律由棋盘重新推导,避免存档里的陈旧字段和实际棋盘打架。
    refresh_stats(g);
    return true;
}

g2048_dir_t g2048_dir_next(g2048_dir_t dir)
{
    const int value = (int)dir + 1;
    if (value >= (int)G2048_DIR_COUNT) return (g2048_dir_t)0;
    return (g2048_dir_t)value;
}

g2048_dir_t g2048_dir_prev(g2048_dir_t dir)
{
    const int value = (int)dir - 1;
    if (value < 0) return (g2048_dir_t)(G2048_DIR_COUNT - 1);
    return (g2048_dir_t)value;
}
