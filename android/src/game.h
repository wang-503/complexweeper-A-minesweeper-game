// 复扫雷 · 规则与状态（安卓版的 C 实现）。
//
// 这个文件是 正式版/src/rules.zig 的逐行移植。为什要两份：安卓侧用 NDK 的 clang
// 编译 C，而 Zig 0.14.1 没法给 Android 链接 libc，所以「同一份源码两端共用」这条
// 路在规则层走不通。于是规则有两份实现，靠这三道闸门锁住不让它们飘走：
//   1. selftest.c 移植了 selftest.zig 的断言；
//   2. `--rules-dump` 两端都输出同样的局面指纹，tools/parity_check.js 逐字对比；
//   3. 改动规则时两个文件必须一起改（这一点写在两边的文件头里）。
//
// 移植纪律：**不要在这里"顺手改进"**。命名、循环顺序、取整方式、边界条件都照抄，
// 因为顺序会直接影响 mulberry32 的取数序列，进而改变同种子下的棋盘。
#ifndef CS_GAME_H
#define CS_GAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------- 常量
#define GAME_MAX_W 40
#define GAME_MAX_H 30
#define GAME_MAX_CELLS (GAME_MAX_W * GAME_MAX_H)
#define GAME_MAX_MINES 999

// 四种雷：(a,b) 分别是实部与虚部的贡献。下标 0..3 对应类型 1..4
extern const int32_t game_types[4][2];

// 只可能出现的 24 个显示值 D = |S|^2
#define GAME_ACHIEVABLE_COUNT 24
extern const uint16_t game_achievable[GAME_ACHIEVABLE_COUNT];

typedef struct {
    uint16_t w, h, mines;
    const char *label;
} GamePreset;

// 标准三档（类型随机撒）
#define GAME_PRESET_COUNT 3
extern const GamePreset game_presets[GAME_PRESET_COUNT];

// 状态行文字（逻辑层只给枚举，文案在界面层）
typedef enum {
    GAME_MSG_NONE = 0,
    GAME_MSG_STARTED,
    GAME_MSG_JUDGE_FAIL,
    GAME_MSG_EXPAND_OK,
    GAME_MSG_WIN,
    GAME_MSG_LOSE,
} GameMsg;

// mulberry32：与 rules.zig / demo/archive.js 完全一致，同种子同棋盘
typedef struct {
    uint32_t a;
} GameRng;

void game_rng_init(GameRng *r, uint32_t seed);
double game_rng_next(GameRng *r);
// [0, n) 的整数
size_t game_rng_below(GameRng *r, size_t n);

typedef struct {
    uint16_t w;
    uint16_t h;
    size_t n;
    uint8_t mine[GAME_MAX_CELLS];
    int16_t clue[GAME_MAX_CELLS];
    uint8_t open[GAME_MAX_CELLS];
    uint8_t flag[GAME_MAX_CELLS];
    uint32_t seed;
    GameRng rng;
    uint16_t mines;
    // 各类雷的总数（下标 1..4），开局公开
    uint16_t type_total[5];
    // 当前插了各类旗几面
    uint16_t flags_of[5];
    // 自定义配比（下标 1..4）；全 0 表示"类型随机撒"
    uint16_t type_count[5];
    bool started;
    bool over;
    bool win;
    int32_t boom;
    int32_t start_cell;
    uint32_t elapsed_ms;
    uint32_t t0;
    uint32_t moves;
    // 上一次操作的反馈（文案在界面层）
    GameMsg msg;
    // 附加数字（例如展开格数），0 表示无
    uint16_t msg_arg;
} Game;

static inline size_t game_cell_count(const Game *g) { return g->n; }

static inline bool game_inb(const Game *g, int32_t r, int32_t c) {
    return r >= 0 && c >= 0 && r < (int32_t)g->h && c < (int32_t)g->w;
}

// 8 邻域，写到 buf 里，返回个数。buf 至少 8 个元素
size_t game_nbrs(const Game *g, size_t cell, size_t *buf);
size_t game_nbr_mine_count(const Game *g, size_t cell);
// 空白格：邻域一颗雷都没有。只有它会连片展开
bool game_is_blank(const Game *g, size_t cell);
size_t game_flags_total(const Game *g);
// 该类雷还有几颗没标（可以是负数）
int32_t game_unmarked(const Game *g, size_t t);
void game_set_msg(Game *g, GameMsg m);

// ---------------------------------------------------------------- 生成
void game_set_seed(Game *g, uint32_t s);
// 新开一局（未开局状态，棋盘等第一次点击时再生成）
void game_new_game(Game *g, uint32_t seed);
// 布雷 + 算显示值 + 从开局格连片。safe = 开局格及其（界内）8 邻居
void game_gen_board(Game *g, size_t start_cell);
void game_compute_clues(Game *g);
void game_count_types(Game *g);
// 连片翻开：只在空白格上继续扩散。返回新翻开的格数
size_t game_cascade_open(Game *g, const size_t *seeds, size_t seed_count);

// ---------------------------------------------------------------- 操作
void game_start_at(Game *g, size_t cell, uint32_t now_ms);
// 插/改/清旗。旗帜不限量，永远成功
bool game_set_flag(Game *g, size_t cell, uint8_t t);
// 循环：空 → 1 → 2 → 3 → 4 → 空
bool game_cycle_flag(Game *g, size_t cell);
// 翻开一格。插了旗的格子翻不开
void game_reveal(Game *g, size_t cell, uint32_t now_ms);
// 组合匹配（严档）：旗帜总数 = 邻域真实雷总数，且实/虚旗数与真实实/虚雷数一致（顺序不限）
bool game_match_combo_truth(const Game *g, size_t cell);
// 双击展开：判据通过就翻开周围未插旗的格（可能踩雷）
void game_try_expand(Game *g, size_t cell);
void game_check_win(Game *g);
void game_lose(Game *g, size_t cell);
size_t game_opened_count(const Game *g);
size_t game_safe_count(const Game *g);
size_t game_correct_flags(const Game *g);
// 精确定义下的合法配比（供自检用）：各类雷数之和 = 总雷数
size_t game_type_sum(const Game *g);

// 把总数尽量均匀分给四种雷（自定义对话框的预填值）
void game_split_evenly(uint16_t total, uint16_t out[5]);

#endif // CS_GAME_H
