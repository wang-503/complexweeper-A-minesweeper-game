// 复扫雷 · 规则与状态（安卓版的 C 实现）。
// 见 game.h 顶部的移植纪律：这里是 正式版/src/rules.zig 的逐行照抄，
// 不要在这里顺手改进。改动规则时 rules.zig 必须一起改。
#include "game.h"

#include <stddef.h>

const int32_t game_types[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

const uint16_t game_achievable[GAME_ACHIEVABLE_COUNT] = {
    0, 1, 2, 4, 5, 8, 9, 10, 13, 16, 17, 18, 20, 25, 26, 29, 32, 34, 36, 37, 40, 49, 50, 64
};

const GamePreset game_presets[GAME_PRESET_COUNT] = {
    { 9, 9, 10, "初级 9×9 · 10 雷" },
    { 16, 16, 40, "中级 16×16 · 40 雷" },
    { 30, 16, 99, "高级 30×16 · 99 雷" },
};

// ---------------------------------------------------------------- 随机数
void game_rng_init(GameRng *r, uint32_t seed) {
    // 与 Zig 一样：种子 0 换成 1
    r->a = (seed == 0) ? 1u : seed;
}

double game_rng_next(GameRng *r) {
    r->a += 0x6D2B79F5u;
    uint32_t t = r->a;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + ((t ^ (t >> 7)) * (t | 61u));
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

size_t game_rng_below(GameRng *r, size_t n) {
    if (n == 0) return 0;
    // 与 Zig 的 @intFromFloat（向零取整）一致：floor 在非负值上等价
    return (size_t)(game_rng_next(r) * (double)n);
}

// ---------------------------------------------------------------- 邻域
size_t game_nbrs(const Game *g, size_t cell, size_t *buf) {
    const int32_t w = (int32_t)g->w;
    const int32_t r = (int32_t)(cell / g->w);
    const int32_t c = (int32_t)(cell % g->w);
    size_t k = 0;
    int32_t dr = -1;
    while (dr <= 1) {
        int32_t dc = -1;
        while (dc <= 1) {
            if (!(dr == 0 && dc == 0)) {
                const int32_t rr = r + dr;
                const int32_t cc = c + dc;
                if (!(rr < 0 || cc < 0 || rr >= (int32_t)g->h || cc >= (int32_t)g->w)) {
                    buf[k] = (size_t)(rr * w + cc);
                    k += 1;
                }
            }
            dc += 1;
        }
        dr += 1;
    }
    return k;
}

size_t game_nbr_mine_count(const Game *g, size_t cell) {
    size_t buf[8];
    const size_t k = game_nbrs(g, cell, buf);
    size_t n = 0;
    for (size_t i = 0; i < k; i++) {
        if (g->mine[buf[i]] != 0) n += 1;
    }
    return n;
}

bool game_is_blank(const Game *g, size_t cell) {
    return g->mine[cell] == 0 && game_nbr_mine_count(g, cell) == 0;
}

size_t game_flags_total(const Game *g) {
    size_t s = 0;
    for (size_t t = 1; t < 5; t++) s += g->flags_of[t];
    return s;
}

int32_t game_unmarked(const Game *g, size_t t) {
    return (int32_t)g->type_total[t] - (int32_t)g->flags_of[t];
}

void game_set_msg(Game *g, GameMsg m) {
    g->msg = m;
    g->msg_arg = 0;
}

// ---------------------------------------------------------------- 生成
void game_set_seed(Game *g, uint32_t s) {
    g->seed = (s == 0) ? 1u : s;
    game_rng_init(&g->rng, g->seed);
}

void game_new_game(Game *g, uint32_t seed) {
    g->n = (size_t)g->w * (size_t)g->h;
    for (size_t i = 0; i < g->n; i++) {
        g->mine[i] = 0;
        g->clue[i] = -1;
        g->open[i] = 0;
        g->flag[i] = 0;
    }
    g->started = false;
    g->over = false;
    g->win = false;
    g->boom = -1;
    g->start_cell = -1;
    g->elapsed_ms = 0;
    g->t0 = 0;
    g->moves = 0;
    for (size_t t = 0; t < 5; t++) {
        g->type_total[t] = 0;
        g->flags_of[t] = 0;
    }
    game_set_seed(g, seed);
    game_set_msg(g, GAME_MSG_NONE);
}

void game_gen_board(Game *g, size_t start_cell) {
    const size_t N = g->n;
    for (size_t i = 0; i < N; i++) {
        g->mine[i] = 0;
        g->clue[i] = -1;
        g->open[i] = 0;
        // 注意：这里**不动 flag[]**。开局前插的旗要活过第一次左键
        // （传统扫雷就是这样）；清旗由 newGame() 负责，那里 flag 与 flags_of 一起归零。
    }

    static bool is_safe[GAME_MAX_CELLS];
    for (size_t i = 0; i < N; i++) is_safe[i] = false;
    is_safe[start_cell] = true;
    size_t nbuf[8];
    const size_t nk = game_nbrs(g, start_cell, nbuf);
    for (size_t i = 0; i < nk; i++) is_safe[nbuf[i]] = true;

    static size_t pool[GAME_MAX_CELLS];
    size_t m = 0;
    for (size_t i = 0; i < N; i++) {
        if (!is_safe[i]) {
            pool[m] = i;
            m += 1;
        }
    }
    // 洗位置
    if (m > 1) {
        size_t i = m - 1;
        while (i > 0) {
            const size_t j = game_rng_below(&g->rng, i + 1);
            const size_t t = pool[i];
            pool[i] = pool[j];
            pool[j] = t;
            i -= 1;
        }
    }

    size_t want = 0;
    for (size_t t = 1; t < 5; t++) want += g->type_count[t];
    size_t count = (want > 0) ? want : (size_t)g->mines;
    if (count > m) count = m;

    if (want > 0) {
        // 精确配比：先铺类型序列，再洗一遍
        static uint8_t list[GAME_MAX_CELLS];
        size_t ln = 0;
        for (size_t t = 1; t < 5; t++) {
            size_t k = 0;
            while (k < g->type_count[t]) {
                list[ln] = (uint8_t)t;
                ln += 1;
                k += 1;
            }
        }
        if (ln > count) ln = count;
        if (ln > 1) {
            size_t i = ln - 1;
            while (i > 0) {
                const size_t j = game_rng_below(&g->rng, i + 1);
                const uint8_t t = list[i];
                list[i] = list[j];
                list[j] = t;
                i -= 1;
            }
        }
        for (size_t k = 0; k < ln; k++) g->mine[pool[k]] = list[k];
    }
    else {
        for (size_t k = 0; k < count; k++) {
            g->mine[pool[k]] = (uint8_t)(1 + game_rng_below(&g->rng, 4));
        }
    }
    g->mines = (uint16_t)count;
    game_compute_clues(g);
    game_count_types(g);
    // 这里既不碰 flag[] 也不碰 flags_of：开局前插的旗（以及它的计数）要留到开局之后。
    const size_t seeds[1] = { start_cell };
    (void)game_cascade_open(g, seeds, 1);
}

void game_compute_clues(Game *g) {
    for (size_t i = 0; i < g->n; i++) {
        if (g->mine[i] != 0) {
            g->clue[i] = -1;
            continue;
        }
        int32_t a = 0;
        int32_t b = 0;
        size_t buf[8];
        const size_t k = game_nbrs(g, i, buf);
        for (size_t x = 0; x < k; x++) {
            // 必须跳过空邻居：mine[j]==0 时 game_types[mine[j]-1] 会越界读到垃圾，
            // 把显示值算错（这个 bug 一度因为越界正好读到 0 而"看起来正常"）
            const size_t j = buf[x];
            if (g->mine[j] == 0) continue;
            const int32_t *t = game_types[g->mine[j] - 1];
            a += t[0];
            b += t[1];
        }
        g->clue[i] = (int16_t)(a * a + b * b);
    }
}

void game_count_types(Game *g) {
    for (size_t t = 0; t < 5; t++) g->type_total[t] = 0;
    for (size_t i = 0; i < g->n; i++) {
        if (g->mine[i] != 0) g->type_total[g->mine[i]] += 1;
    }
}

size_t game_cascade_open(Game *g, const size_t *seeds, size_t seed_count) {
    static size_t stack[GAME_MAX_CELLS];
    static bool queued[GAME_MAX_CELLS];
    for (size_t i = 0; i < g->n; i++) queued[i] = false;
    size_t sp = 0;
    for (size_t i = 0; i < seed_count; i++) {
        const size_t s = seeds[i];
        if (!queued[s]) {
            queued[s] = true;
            stack[sp] = s;
            sp += 1;
        }
    }
    size_t opened = 0;
    while (sp > 0) {
        sp -= 1;
        const size_t i = stack[sp];
        // 连片也不碰插了旗的格子（传统扫雷：旗子保护它，得玩家自己撤旗）
        if (g->open[i] != 0 || g->mine[i] != 0 || g->flag[i] != 0) continue;
        g->open[i] = 1;
        opened += 1;
        if (game_is_blank(g, i)) {
            size_t buf[8];
            const size_t k = game_nbrs(g, i, buf);
            for (size_t x = 0; x < k; x++) {
                const size_t j = buf[x];
                // 查重必须标在**压栈时**。改成弹出时才标，同一个格子会被相邻的
                // 多个空白格重复压进去，栈照样能塞满。
                if (!queued[j] && g->open[j] == 0 && g->mine[j] == 0 && g->flag[j] == 0) {
                    queued[j] = true;
                    stack[sp] = j;
                    sp += 1;
                }
            }
        }
    }
    return opened;
}

// ---------------------------------------------------------------- 操作
void game_start_at(Game *g, size_t cell, uint32_t now_ms) {
    game_set_seed(g, g->seed);
    game_gen_board(g, cell);
    g->start_cell = (int32_t)cell;
    g->started = true;
    g->over = false;
    g->win = false;
    g->elapsed_ms = 0;
    g->t0 = now_ms;
    g->moves = 1;
    game_set_msg(g, GAME_MSG_NONE);
}

bool game_set_flag(Game *g, size_t cell, uint8_t t) {
    if (t > 4) return false;
    const uint8_t old = g->flag[cell];
    if (old == t) return true;
    if (old != 0) g->flags_of[old] -= 1;
    g->flag[cell] = t;
    if (t != 0) g->flags_of[t] += 1;
    return true;
}

bool game_cycle_flag(Game *g, size_t cell) {
    if (g->over || g->open[cell] != 0) return false;
    const uint8_t next = (uint8_t)(((size_t)g->flag[cell] + 1) % 5);
    (void)game_set_flag(g, cell, next);
    g->moves += 1;
    return true;
}

void game_reveal(Game *g, size_t cell, uint32_t now_ms) {
    (void)now_ms;
    if (g->over || g->open[cell] != 0 || g->flag[cell] != 0) return;
    if (g->mine[cell] != 0) {
        g->open[cell] = 1;
        game_lose(g, cell);
        return;
    }
    g->open[cell] = 1;
    if (game_is_blank(g, cell)) {
        size_t buf[8];
        const size_t k = game_nbrs(g, cell, buf);
        (void)game_cascade_open(g, buf, k);
    }
    g->moves += 1;
    game_check_win(g);
}

bool game_match_combo_truth(const Game *g, size_t cell) {
    uint16_t truth[4] = { 0, 0, 0, 0 };
    uint16_t got[4] = { 0, 0, 0, 0 };
    size_t buf[8];
    const size_t k = game_nbrs(g, cell, buf);
    for (size_t x = 0; x < k; x++) {
        const size_t j = buf[x];
        if (g->mine[j] != 0) truth[g->mine[j] - 1] += 1;
        if (g->flag[j] != 0) got[g->flag[j] - 1] += 1;
    }
    const uint16_t P = truth[0] + truth[1];
    const uint16_t V = truth[2] + truth[3];
    const uint16_t gp = got[0] + got[1];
    const uint16_t gv = got[2] + got[3];
    return (gp + gv == P + V) && (((gp == P) && (gv == V)) || ((gp == V) && (gv == P)));
}

void game_try_expand(Game *g, size_t cell) {
    if (g->over || g->open[cell] == 0 || g->mine[cell] != 0) return;
    size_t buf[8];
    const size_t k = game_nbrs(g, cell, buf);
    size_t uns[8];
    size_t un = 0;
    for (size_t x = 0; x < k; x++) {
        const size_t j = buf[x];
        if (g->open[j] == 0 && g->flag[j] == 0) {
            uns[un] = j;
            un += 1;
        }
    }
    if (un == 0) return;
    if (!game_match_combo_truth(g, cell)) {
        game_set_msg(g, GAME_MSG_JUDGE_FAIL);
        return;
    }
    int32_t boom = -1;
    for (size_t x = 0; x < un; x++) {
        if (g->mine[uns[x]] != 0) {
            boom = (int32_t)uns[x];
            break;
        }
    }
    if (boom >= 0) {
        const size_t b = (size_t)boom;
        g->open[b] = 1;
        game_lose(g, b);
        return;
    }
    (void)game_cascade_open(g, uns, un);
    g->moves += 1;
    g->msg_arg = (uint16_t)un;
    game_set_msg(g, GAME_MSG_EXPAND_OK);
    game_check_win(g);
}

void game_check_win(Game *g) {
    for (size_t i = 0; i < g->n; i++) {
        if (g->mine[i] == 0 && g->open[i] == 0) return;
    }
    g->over = true;
    g->win = true;
    game_set_msg(g, GAME_MSG_WIN);
}

void game_lose(Game *g, size_t cell) {
    g->over = true;
    g->win = false;
    g->boom = (int32_t)cell;
    game_set_msg(g, GAME_MSG_LOSE);
}

size_t game_opened_count(const Game *g) {
    size_t k = 0;
    for (size_t i = 0; i < g->n; i++) {
        if (g->open[i] != 0) k += 1;
    }
    return k;
}

size_t game_safe_count(const Game *g) {
    size_t k = 0;
    for (size_t i = 0; i < g->n; i++) {
        if (g->mine[i] == 0) k += 1;
    }
    return k;
}

size_t game_correct_flags(const Game *g) {
    size_t k = 0;
    for (size_t i = 0; i < g->n; i++) {
        if (g->mine[i] != 0 && g->flag[i] == g->mine[i]) k += 1;
    }
    return k;
}

size_t game_type_sum(const Game *g) {
    size_t s = 0;
    for (size_t t = 1; t < 5; t++) s += g->type_total[t];
    return s;
}

void game_split_evenly(uint16_t total, uint16_t out[5]) {
    for (size_t t = 0; t < 5; t++) out[t] = 0;
    const uint16_t base = (uint16_t)(total / 4);
    uint16_t rest = (uint16_t)(total - base * 4);
    for (size_t t = 1; t < 5; t++) {
        out[t] = base;
        if (rest > 0) {
            out[t] += 1;
            rest -= 1;
        }
    }
}
