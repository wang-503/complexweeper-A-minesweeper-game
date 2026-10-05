// 无界面规则自检（安卓版）—— 正式版/src/selftest.zig 的移植。
//
// 目的很直接：C 这份规则实现必须和 Zig 那份表现一致，否则两端会长出两个不同的游戏。
// 这里把 Zig 版自检里的**每一组不变量**都照抄一遍，包括那些"看起来很傻但确实抓到过 bug"
// 的用例（例如显示 0 的格子绝不能连片、大盘连片不许因为栈吃满而漏格）。
//
// 输出格式刻意与 Zig 版保持一致，方便 tools/parity_check.js 直接把两份报告对着看。
#include "selftest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "game.h"

static uint32_t fails = 0;
static uint32_t checks = 0;

static void expectf(StrBuf *out, bool cond, const char *name) {
    checks += 1;
    if (!cond) {
        fails += 1;
        sb_addf(out, "  [失败] %s\n", name);
    }
}

// 独立的洪水填充实现，用来核对游戏里的连片（照抄 selftest.zig 的 expectedCascade）
static void expected_cascade(const Game *game, size_t start, bool *set) {
    for (size_t i = 0; i < GAME_MAX_CELLS; i++) set[i] = false;
    static size_t stack[GAME_MAX_CELLS];
    size_t sp = 0;
    stack[sp] = start;
    sp += 1;
    static bool comp[GAME_MAX_CELLS];
    for (size_t i = 0; i < GAME_MAX_CELLS; i++) comp[i] = false;
    comp[start] = true;
    while (sp > 0) {
        sp -= 1;
        const size_t i = stack[sp];
        size_t buf[8];
        const size_t k = game_nbrs(game, i, buf);
        for (size_t x = 0; x < k; x++) {
            const size_t j = buf[x];
            if (comp[j] || game->mine[j] != 0) continue;
            if (game_is_blank(game, j)) {
                comp[j] = true;
                stack[sp] = j;
                sp += 1;
            }
        }
    }
    // 连片覆盖 = 连通空白格 ∪ 它们的非雷邻居
    for (size_t i = 0; i < game->n; i++) {
        if (!comp[i]) continue;
        set[i] = true;
        size_t buf[8];
        const size_t k = game_nbrs(game, i, buf);
        for (size_t x = 0; x < k; x++) {
            const size_t j = buf[x];
            if (game->mine[j] == 0) set[j] = true;
        }
    }
}

static void build_board(Game *game, uint16_t w, uint16_t h, uint16_t mines,
                        const uint16_t tc[5], uint32_t seed, size_t start) {
    game->w = w;
    game->h = h;
    game->mines = mines;
    for (size_t t = 0; t < 5; t++) game->type_count[t] = tc[t];
    game_new_game(game, seed);
    game_start_at(game, start, 0);
}

static const uint16_t TC_NONE[5] = { 0, 0, 0, 0, 0 };

uint32_t selftest_run(StrBuf *out, const char *app_version) {
    fails = 0;
    checks = 0;
    static Game game;

    sb_addf(out, "复扫雷 %s · 规则自检\n==========================\n", app_version);

    // ---- 1. 随机数确定性 ----
    {
        static Game a, b, c;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        memset(&c, 0, sizeof(c));
        build_board(&a, 16, 16, 40, TC_NONE, 12345, 100);
        build_board(&b, 16, 16, 40, TC_NONE, 12345, 100);
        bool same = true;
        for (size_t i = 0; i < a.n; i++) {
            if (a.mine[i] != b.mine[i]) same = false;
        }
        expectf(out, same, "同种子 + 同开局格应生成完全相同的棋盘");
        build_board(&c, 16, 16, 40, TC_NONE, 999, 100);
        bool diff = false;
        for (size_t i = 0; i < a.n; i++) {
            if (a.mine[i] != c.mine[i]) diff = true;
        }
        expectf(out, diff, "不同种子应生成不同棋盘");
        sb_addf(out, "1 随机数确定性：通过（校验 %u 项）\n", checks);
    }

    // ---- 2. 精确配比 ----
    {
        static const uint16_t cases[5][5] = {
            { 0, 3, 2, 4, 1 },
            { 0, 0, 0, 0, 6 },
            { 0, 7, 0, 0, 0 },
            { 0, 5, 5, 0, 0 },
            { 0, 0, 0, 4, 4 },
        };
        uint32_t bad = 0;
        static const uint32_t seeds[3] = { 11, 22, 33 };
        for (size_t ci = 0; ci < 5; ci++) {
            const uint16_t *tc = cases[ci];
            uint16_t sum = 0;
            for (size_t t = 1; t < 5; t++) sum += tc[t];
            for (size_t si = 0; si < 3; si++) {
                build_board(&game, 12, 12, sum, tc, seeds[si], 70);
                for (size_t t = 1; t < 5; t++) {
                    if (game.type_total[t] != tc[t]) bad += 1;
                }
                if (game_type_sum(&game) != sum) bad += 1;
                if (game.mines != sum) bad += 1;
            }
        }
        expectf(out, bad == 0, "指定配比必须被精确执行（5 种配比 × 3 种子）");
        sb_addf(out, "2 精确配比：%s\n", bad == 0 ? "通过" : "有偏差");
    }

    // ---- 3. 纯实 / 纯虚局面的显示值全是完全平方数 ----
    {
        uint32_t bad = 0;
        static const uint16_t pure[2][5] = { { 0, 5, 5, 0, 0 }, { 0, 0, 0, 4, 4 } };
        static const uint32_t seeds[3] = { 7, 8, 9 };
        for (size_t pi = 0; pi < 2; pi++) {
            const uint16_t *tc = pure[pi];
            uint16_t sum = 0;
            for (size_t t = 1; t < 5; t++) sum += tc[t];
            for (size_t si = 0; si < 3; si++) {
                build_board(&game, 12, 12, sum, tc, seeds[si], 70);
                int32_t first_bad = -1;
                for (size_t i = 0; i < game.n; i++) {
                    if (game.mine[i] != 0) continue;
                    const uint32_t D = (uint32_t)game.clue[i];
                    uint32_t r = 0;
                    while (r * r < D) r += 1;
                    if (r * r != D) {
                        bad += 1;
                        if (first_bad < 0) first_bad = (int32_t)i;
                    }
                }
                if (first_bad >= 0) {
                    sb_addf(out, "  [调试] 请求配比 %u/%u/%u/%u，实际 type_total %u/%u/%u/%u，首个非平方格 %d 的 D=%d，周围雷 %u 颗\n",
                            tc[1], tc[2], tc[3], tc[4],
                            game.type_total[1], game.type_total[2], game.type_total[3], game.type_total[4],
                            first_bad, game.clue[first_bad], (unsigned)game_nbr_mine_count(&game, (size_t)first_bad));
                }
            }
        }
        expectf(out, bad == 0, "纯实/纯虚局面里所有显示值都必须是完全平方数");
        sb_addf(out, "3 纯实/纯虚不变量：%s\n", bad == 0 ? "通过" : "失败");
    }

    // ---- 4. 连片：与独立洪水填充逐格一致，且绝不翻雷 ----
    {
        uint32_t mismatch = 0;
        uint32_t mine_opened = 0;
        uint32_t samples = 0;
        uint32_t zero_cascaded = 0;
        uint32_t zero_samples = 0;
        static const uint32_t seeds[5] = { 301, 302, 303, 304, 305 };
        static bool before[GAME_MAX_CELLS];
        static bool want[GAME_MAX_CELLS];
        for (size_t si = 0; si < 5; si++) {
            build_board(&game, 12, 12, 24, TC_NONE, seeds[si], 70);
            size_t i = 0;
            while (i < game.n && samples < 40) {
                if (game.mine[i] != 0 || game.open[i] != 0 || !game_is_blank(&game, i)) {
                    i += 1;
                    continue;
                }
                // 干净棋盘上单独点这一格
                build_board(&game, 12, 12, 24, TC_NONE, seeds[si], 70);
                if (game.open[i] != 0) {
                    i += 1;
                    continue;
                }
                for (size_t k = 0; k < GAME_MAX_CELLS; k++) before[k] = game.open[k] != 0;
                game_reveal(&game, i, 0);
                expected_cascade(&game, i, want);
                samples += 1;
                for (size_t k = 0; k < game.n; k++) {
                    const bool got = (game.open[k] != 0) && !before[k];
                    const bool exp = want[k] && !before[k];
                    if (got != exp) mismatch += 1;
                    if (got && game.mine[k] != 0) mine_opened += 1;
                }
                i += 1;
            }
            // 显示 0（邻域有雷相消）绝不连片
            build_board(&game, 12, 12, 24, TC_NONE, seeds[si], 70);
            for (size_t k = 0; k < game.n; k++) {
                if (game.mine[k] != 0 || game.open[k] != 0) continue;
                if (game.clue[k] != 0 || game_nbr_mine_count(&game, k) == 0) continue;
                for (size_t m = 0; m < GAME_MAX_CELLS; m++) before[m] = game.open[m] != 0;
                game_reveal(&game, k, 0);
                uint32_t opened_now = 0;
                for (size_t m = 0; m < game.n; m++) {
                    if (game.open[m] != 0 && !before[m]) opened_now += 1;
                }
                zero_samples += 1;
                if (opened_now != 1) zero_cascaded += 1;
            }
        }
        expectf(out, samples >= 20, "空白格连片样本数足够");
        expectf(out, mismatch == 0, "连片结果必须与独立洪水填充逐格一致");
        expectf(out, mine_opened == 0, "连片绝不能翻开雷");
        expectf(out, zero_samples >= 5, "显示 0 的样本数足够");
        expectf(out, zero_cascaded == 0, "显示 0 的格子绝不能连片");
        sb_addf(out, "4 连片展开：空白样本 %u，显示 0 样本 %u，不一致 %u，翻雷 %u\n",
                samples, zero_samples, mismatch, mine_opened);
    }

    // ---- 4b. 大盘开局连片：栈吃满也不许漏格 ----
    {
        static const struct { uint16_t w, h, m; } shapes[6] = {
            { 40, 30, 1 }, { 40, 30, 5 }, { 40, 30, 20 }, { 40, 30, 60 }, { 40, 20, 40 }, { 30, 30, 60 },
        };
        uint32_t mismatch = 0;
        uint32_t opened_mine = 0;
        uint32_t samples = 0;
        uint32_t big = 0;
        size_t worst = 0;
        static const uint32_t seeds[6] = { 4111, 4127, 4133, 4139, 4153, 4159 };
        static bool want[GAME_MAX_CELLS];
        for (size_t shi = 0; shi < 6; shi++) {
            const size_t bw = shapes[shi].w;
            const size_t bh = shapes[shi].h;
            // 角、上边中点、正中、右下角：换着开局格，覆盖不同的扩散顺序
            const size_t starts[4] = { 0, bw / 2, (bh / 2) * bw + bw / 2, bw * bh - 1 };
            for (size_t si = 0; si < 6; si++) {
                for (size_t sti = 0; sti < 4; sti++) {
                    const size_t st = starts[sti];
                    build_board(&game, shapes[shi].w, shapes[shi].h, shapes[shi].m, TC_NONE, seeds[si], st);
                    expected_cascade(&game, st, want);
                    for (size_t k = 0; k < game.n; k++) {
                        if ((game.open[k] != 0) != want[k]) mismatch += 1;
                        if (game.open[k] != 0 && game.mine[k] != 0) opened_mine += 1;
                    }
                    const size_t oc = game_opened_count(&game);
                    if (oc > worst) worst = oc;
                    if (oc >= 400) big += 1;
                    samples += 1;
                }
            }
        }
        expectf(out, samples >= 72, "大盘连片样本数足够（6 种盘面 × 6 种子 × 4 开局格）");
        expectf(out, big >= 8, "大盘样本里应有真正的大连片（≥400 格）");
        expectf(out, mismatch == 0, "大盘开局连片必须与独立洪水填充逐格一致（栈吃满也不许漏格）");
        expectf(out, opened_mine == 0, "大盘连片也绝不能翻开雷");
        sb_addf(out, "4b 大盘连片：样本 %u，大连片 %u 次，一次最多翻开 %u 格，逐格不一致 %u\n",
                samples, big, (unsigned)worst, mismatch);
    }

    // ---- 5. 开局必定连片且不踩雷 ----
    {
        uint32_t bad = 0;
        static const uint32_t seeds[5] = { 501, 502, 503, 504, 505 };
        for (size_t si = 0; si < 5; si++) {
            static Game gm;
            memset(&gm, 0, sizeof(gm));
            gm.w = 16;
            gm.h = 16;
            gm.mines = 40;
            game_new_game(&gm, seeds[si]);
            const size_t start = 16 * 8 + 8;
            game_start_at(&gm, start, 0);
            if (gm.over) bad += 1;
            if (game_opened_count(&gm) < 9) bad += 1;
            if (!game_is_blank(&gm, start)) bad += 1;
            // 开局后各类雷数必须已知，且合计 = 总雷数
            if (game_type_sum(&gm) != 40) bad += 1;
            for (size_t t = 1; t < 5; t++) {
                if (gm.type_total[t] == 0 && gm.mines >= 40) bad += 1;
                if (game_unmarked(&gm, t) != (int32_t)gm.type_total[t]) bad += 1;
            }
        }
        expectf(out, bad == 0, "开局格必为空白格、必连片（≥9 格）且不踩雷；各类雷数已知");
        sb_addf(out, "5 开局连片与分类计数：%s\n", bad == 0 ? "通过" : "失败");
    }

    // ---- 6. 判据：与独立实现一致 ----
    {
        uint32_t mismatch = 0;
        uint32_t pass = 0;
        uint32_t total = 0;
        static const uint32_t seeds[3] = { 601, 602, 603 };
        for (size_t si = 0; si < 3; si++) {
            build_board(&game, 12, 12, 24, TC_NONE, seeds[si], 70);
            for (size_t i = 0; i < game.n; i++) {
                if (game.mine[i] != 0 || game.open[i] == 0) continue;
                // 随机插一些旗
                size_t buf[8];
                const size_t k = game_nbrs(&game, i, buf);
                for (size_t x = 0; x < k; x++) {
                    const size_t j = buf[x];
                    if (game.open[j] != 0) continue;
                    (void)game_set_flag(&game, j, (uint8_t)(1 + ((i + j) % 4)));
                }
                // 独立算一遍
                int32_t truth[4] = { 0, 0, 0, 0 };
                int32_t got[4] = { 0, 0, 0, 0 };
                for (size_t x = 0; x < k; x++) {
                    const size_t j = buf[x];
                    if (game.mine[j] != 0) truth[game.mine[j] - 1] += 1;
                    if (game.flag[j] != 0) got[game.flag[j] - 1] += 1;
                }
                const int32_t P = truth[0] + truth[1];
                const int32_t V = truth[2] + truth[3];
                const int32_t gp = got[0] + got[1];
                const int32_t gv = got[2] + got[3];
                const bool want = (gp + gv == P + V) && (((gp == P) && (gv == V)) || ((gp == V) && (gv == P)));
                const bool mine = game_match_combo_truth(&game, i);
                total += 1;
                if (want != mine) mismatch += 1;
                if (want) pass += 1;
            }
            // 清旗，避免影响下一轮
            for (size_t j = 0; j < game.n; j++) (void)game_set_flag(&game, j, 0);
        }
        expectf(out, total > 50, "判据样本数足够");
        expectf(out, mismatch == 0, "判据结果必须与独立实现一致");
        expectf(out, pass > 0, "判据应至少放行一部分组合");
        sb_addf(out, "6 组合匹配判据：样本 %u，放行 %u，不一致 %u\n", total, pass, mismatch);
    }

    // ---- 7. 插旗不限量 + 循环顺序 ----
    {
        build_board(&game, 9, 9, 10, TC_NONE, 701, 40);
        uint32_t placed = 0;
        for (size_t i = 0; i < game.n; i++) {
            if (game.open[i] != 0) continue;
            if (game_set_flag(&game, i, 1)) placed += 1;
        }
        expectf(out, placed > 0, "所有未翻开格都能插旗");
        expectf(out, game.flags_of[1] == placed, "计数与实际插旗数一致");
        expectf(out, game_unmarked(&game, 1) < 0, "插超后未标记数应为负数");
        // 循环顺序
        size_t cell = 0;
        for (size_t i = 0; i < game.n; i++) {
            if (game.open[i] == 0) {
                cell = i;
                break;
            }
        }
        (void)game_set_flag(&game, cell, 0);
        uint8_t seq[6];
        for (size_t k = 0; k < 6; k++) {
            (void)game_cycle_flag(&game, cell);
            seq[k] = game.flag[cell];
        }
        const uint8_t want_seq[6] = { 1, 2, 3, 4, 0, 1 };
        bool ok = true;
        for (size_t k = 0; k < 6; k++) {
            if (seq[k] != want_seq[k]) ok = false;
        }
        expectf(out, ok, "右键循环必须是 1,2,3,4,0,1");
        sb_addf(out, "7 插旗不限量：插了 %u 面，循环 { %u, %u, %u, %u, %u, %u }\n",
                placed, seq[0], seq[1], seq[2], seq[3], seq[4], seq[5]);
    }

    // ---- 8. 翻开已插旗格：先清旗再翻开 ----
    {
        build_board(&game, 9, 9, 10, TC_NONE, 801, 40);
        size_t cell = 0;
        for (size_t i = 0; i < game.n; i++) {
            if (game.open[i] == 0 && game.mine[i] == 0) {
                cell = i;
                break;
            }
        }
        (void)game_set_flag(&game, cell, 3);
        expectf(out, game.flags_of[3] == 1, "插旗后计数为 1");
        // 旗子保护格子：插了旗就翻不开（传统扫雷的做法）
        game_reveal(&game, cell, 0);
        expectf(out, game.open[cell] == 0, "插旗的格子翻不开");
        expectf(out, game.flag[cell] == 3, "翻不开时旗帜应原样保留");
        expectf(out, game.flags_of[3] == 1, "翻不开时计数不动");
        // 连片展开也不该把旗子吃掉：找一格空白格，给它的一个邻格插旗，再翻开那格
        size_t blank = 0;
        size_t flagged_nbr = 0;
        bool found = false;
        for (size_t i = 0; i < game.n && !found; i++) {
            if (game.open[i] != 0 || game.mine[i] != 0 || !game_is_blank(&game, i)) continue;
            size_t buf[8];
            const size_t k = game_nbrs(&game, i, buf);
            for (size_t x = 0; x < k; x++) {
                const size_t j = buf[x];
                if (game.open[j] == 0 && game.mine[j] == 0 && game.flag[j] == 0) {
                    blank = i;
                    flagged_nbr = j;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            (void)game_set_flag(&game, flagged_nbr, 1);
            game_reveal(&game, blank, 0);
            expectf(out, game.open[blank] == 1, "空白格应能翻开");
            expectf(out, game.open[flagged_nbr] == 0, "连片展开不该翻开插了旗的格子");
            expectf(out, game.flag[flagged_nbr] == 1, "连片展开不该清掉旗子");
        }
        // 撤旗之后才翻得开
        (void)game_set_flag(&game, cell, 0);
        game_reveal(&game, cell, 0);
        expectf(out, game.open[cell] == 1, "撤旗后应能翻开");
        expectf(out, game.flags_of[3] == 0, "撤旗后计数归还");
        sb_addf(out, "8 旗子保护格子（翻不开、连片也不碰）：通过\n");
    }

    // ---- 9. 胜负判定 ----
    {
        // 胜利：翻开全部非雷格即可，旗帜不参与
        build_board(&game, 9, 9, 10, TC_NONE, 901, 40);
        for (size_t i = 0; i < game.n; i++) {
            if (game.mine[i] == 0 && game.open[i] == 0) game_reveal(&game, i, 0);
        }
        expectf(out, game.win && game.over, "翻开所有非雷格必须判胜");
        // 反面：留一个非雷格就不算胜
        build_board(&game, 9, 9, 10, TC_NONE, 902, 40);
        size_t left = 0;
        for (size_t i = 0; i < game.n; i++) {
            if (game.mine[i] == 0 && game.open[i] == 0) {
                left = i;
                break;
            }
        }
        for (size_t i = 0; i < game.n; i++) {
            if (i != left && game.mine[i] == 0 && game.open[i] == 0) game_reveal(&game, i, 0);
        }
        expectf(out, !game.win, "还剩非雷格未翻开时不能判胜");
        // 踩雷
        build_board(&game, 9, 9, 10, TC_NONE, 903, 40);
        size_t m = 0;
        for (size_t i = 0; i < game.n; i++) {
            if (game.mine[i] != 0) {
                m = i;
                break;
            }
        }
        game_reveal(&game, m, 0);
        expectf(out, game.over && !game.win, "翻开雷必须判负");
        expectf(out, game.boom == (int32_t)m, "记录踩中的格子");
        sb_addf(out, "9 胜负判定：通过\n");
    }

    // ---- 10. 展开：判据不过时棋盘不变；过了才动 ----
    {
        build_board(&game, 12, 12, 24, TC_NONE, 1001, 70);
        size_t cell = 0;
        for (size_t i = 0; i < game.n; i++) {
            if (game.open[i] != 0 && game.mine[i] == 0) {
                size_t buf[8];
                const size_t k = game_nbrs(&game, i, buf);
                uint32_t uns = 0;
                for (size_t x = 0; x < k; x++) {
                    const size_t j = buf[x];
                    if (game.open[j] == 0 && game.flag[j] == 0) uns += 1;
                }
                if (uns > 0) {
                    cell = i;
                    break;
                }
            }
        }
        static bool before[GAME_MAX_CELLS];
        for (size_t i = 0; i < GAME_MAX_CELLS; i++) before[i] = game.open[i] != 0;
        game_try_expand(&game, cell);
        if (!game_match_combo_truth(&game, cell)) {
            bool changed = false;
            for (size_t i = 0; i < game.n; i++) {
                if ((game.open[i] != 0) != before[i]) changed = true;
            }
            expectf(out, !changed, "判据不通过时展开不能改变棋盘");
        }
        sb_addf(out, "10 展开门禁：通过\n");
    }

    sb_addf(out, "\n断言 %u 项，失败 %u 项\n", checks, fails);
    sb_addf(out, "%s\n", fails == 0 ? "全部通过" : "存在失败");
    return fails;
}
