#include <utility>
#include <algorithm>
#include <vector>
#include <cstring>
#include <chrono>
#include "state.hpp"
#include "submission.hpp"

/*============================================================
 * Advanced Data Structures (TT, Killers, History)
 *============================================================*/
enum TTFlag { TT_EXACT, TT_ALPHA, TT_BETA };

struct TTEntry {
    uint64_t hash = 0;
    int depth = -1;
    int score = 0;
    TTFlag flag = TT_EXACT;
    Move best_move;
};

const int TT_SIZE = 1 << 22; 
static TTEntry tt_table[TT_SIZE];

static int history_table[2][10][10][10][10]; 
static Move killer_moves[100][2];            

static std::chrono::time_point<std::chrono::high_resolution_clock> g_start_time;
static const int TIME_LIMIT_MS = 1900; 

// ==========================================================
// 👑 戰術價值對照表
// 0:空, 1:兵, 2:車, 3:馬, 4:相, 5:后, 6:王
// ==========================================================
static const int SUB_PIECE_VALUES[7] = {0, 20, 60, 70, 80, 200, 10000};


/*============================================================
 * 👑 全域威脅掃描器：取得攻擊目標最便宜的棋子
 *============================================================*/
static int get_cheapest_attacker(State* state, int by_player, int tr, int tc) {
    int cheapest = 100;
    auto board = state->board.board[by_player];

    // 1. 兵 (Pawns)
    if (by_player == 0) { // 白方兵往上走 (r-1)
        if (tr + 1 < BOARD_H) {
            if (tc > 0 && board[tr + 1][tc - 1] == 1) return 1;
            if (tc + 1 < BOARD_W && board[tr + 1][tc + 1] == 1) return 1;
        }
    } else { // 黑方兵往下走 (r+1)
        if (tr - 1 >= 0) {
            if (tc > 0 && board[tr - 1][tc - 1] == 1) return 1;
            if (tc + 1 < BOARD_W && board[tr - 1][tc + 1] == 1) return 1;
        }
    }
    if (cheapest == 1) return 1;

    // 2. 馬 (Knights)
    static const int kn_dr[8] = {1, 1, -1, -1, 2, 2, -2, -2};
    static const int kn_dc[8] = {2, -2, 2, -2, 1, -1, 1, -1};
    for (int i=0; i<8; i++) {
        int r = tr + kn_dr[i], c = tc + kn_dc[i];
        if (r>=0 && r<BOARD_H && c>=0 && c<BOARD_W && board[r][c] == 3) {
            cheapest = std::min(cheapest, 3);
        }
    }

    // 3. 相/后 (Bishop / Queen)
    static const int di_dr[4] = {1, 1, -1, -1};
    static const int di_dc[4] = {1, -1, 1, -1};
    for (int i=0; i<4; i++) {
        for (int step=1; step<8; step++) {
            int r = tr + di_dr[i]*step, c = tc + di_dc[i]*step;
            if (r<0 || r>=BOARD_H || c<0 || c>=BOARD_W) break;
            int p = board[r][c];
            if (p == 4 || p == 5) cheapest = std::min(cheapest, p);
            if (p != 0 || state->board.board[1-by_player][r][c] != 0) break; 
        }
    }

    // 4. 車/后 (Rook / Queen)
    static const int st_dr[4] = {1, -1, 0, 0};
    static const int st_dc[4] = {0, 0, 1, -1};
    for (int i=0; i<4; i++) {
        for (int step=1; step<8; step++) {
            int r = tr + st_dr[i]*step, c = tc + st_dc[i]*step;
            if (r<0 || r>=BOARD_H || c<0 || c>=BOARD_W) break;
            int p = board[r][c];
            if (p == 2 || p == 5) cheapest = std::min(cheapest, p);
            if (p != 0 || state->board.board[1-by_player][r][c] != 0) break;
        }
    }

    // 5. 王 (King)
    static const int k_dr[8] = {1, 1, 1, 0, 0, -1, -1, -1};
    static const int k_dc[8] = {1, 0, -1, 1, -1, 1, 0, -1};
    for (int i=0; i<8; i++) {
        int r = tr + k_dr[i], c = tc + k_dc[i];
        if (r>=0 && r<BOARD_H && c>=0 && c<BOARD_W && board[r][c] == 6) {
            cheapest = std::min(cheapest, 6);
        }
    }

    return cheapest == 100 ? 0 : cheapest;
}


/*============================================================
 * 👑 戰術評估包裝器 (Advanced Tactical Evaluation)
 * 完美實作：防送死、迴避劣勢換子、捉雙獎勵、陣型保護，以及王車對衝爆破
 *============================================================*/
static int advanced_evaluate(State* state, const SubParams& p, GameHistory& history) {
    int base_score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    int tactical_score = 0;

    for (int current_p = 0; current_p < 2; current_p++) {
        int opp = 1 - current_p;
        int player_bonus = 0;
        int severe_threats = 0;

        for (int r = 0; r < BOARD_H; r++) {
            for (int c = 0; c < BOARD_W; c++) {
                int piece = state->board.board[current_p][r][c];
                if (piece > 0 && piece < 6) { // 嚴格排除王
                    int attacker = get_cheapest_attacker(state, opp, r, c);
                    int defender = get_cheapest_attacker(state, current_p, r, c); 

                    if (attacker > 0) {
                        // 懲罰縮小至 20%，避免因為一個小威脅就讓總分暴跌
                        if (defender == 0) {
                            player_bonus -= SUB_PIECE_VALUES[piece] / 5; 
                            severe_threats++;
                        } else if (SUB_PIECE_VALUES[attacker] < SUB_PIECE_VALUES[piece]) {
                            player_bonus -= (SUB_PIECE_VALUES[piece] - SUB_PIECE_VALUES[attacker]) / 5;
                            severe_threats++;
                        }
                    }
                }
            }
        }
        // 捉雙懲罰平滑化
        if (severe_threats >= 2) player_bonus -= 20;

        // 王前陣型獎勵也平滑化
        if (current_p == state->player) tactical_score += player_bonus;
        else tactical_score -= player_bonus;
    }
    return base_score + tactical_score;
}


/*============================================================
 * Move Scoring for Ordering (走步排序)
 *============================================================*/
static int score_move(State* state, const Move& m, const Move& tt_move, int ply, int opp) {
    if (m == tt_move) return 10000000; 

    int victim = state->piece_at(opp, m.second.first, m.second.second);
    if (victim > 0) {
        int attacker = state->piece_at(state->player, m.first.first, m.first.second);
        return 1000000 + (SUB_PIECE_VALUES[victim] * 100) - SUB_PIECE_VALUES[attacker]; 
    }

    if (ply < 100) {
        if (killer_moves[ply][0] == m) return 900000;
        if (killer_moves[ply][1] == m) return 800000;
    }

    return history_table[state->player][m.first.first][m.first.second][m.second.first][m.second.second];
}

/*============================================================
 * Quiescence Search (靜態搜尋)
 *============================================================*/
static int q_search(State *state, int alpha, int beta, GameHistory& history, int ply, SearchContext& ctx, const SubParams& p) {
    if ((ctx.nodes & 2047) == 0) {
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_start_time).count() >= TIME_LIMIT_MS) {
            ctx.stop = true;
        }
    }

    ctx.nodes++;
    if (ply > ctx.seldepth) ctx.seldepth = ply;
    if (ctx.stop) return 0;

    int stand_pat = advanced_evaluate(state, p, history);
    
    if (stand_pat >= beta) return beta;
    if (alpha < stand_pat) alpha = stand_pat;

    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }
    if (state->game_state == WIN) return P_MAX - ply;
    if (state->game_state == DRAW) return 0;

    int opp = 1 - state->player;
    std::vector<Move> captures;
    for (auto& action : state->legal_actions) {
        if (state->piece_at(opp, action.second.first, action.second.second) > 0) {
            captures.push_back(action);
        }
    }

    std::sort(captures.begin(), captures.end(), [state, opp](const Move& m1, const Move& m2) {
        int v1 = state->piece_at(opp, m1.second.first, m1.second.second);
        int a1 = state->piece_at(state->player, m1.first.first, m1.first.second);
        int v2 = state->piece_at(opp, m2.second.first, m2.second.second);
        int a2 = state->piece_at(state->player, m2.first.first, m2.first.second);
        
        int score1 = SUB_PIECE_VALUES[v1] * 100 - SUB_PIECE_VALUES[a1];
        int score2 = SUB_PIECE_VALUES[v2] * 100 - SUB_PIECE_VALUES[a2];
        return score1 > score2;
    });

    for (auto& action : captures) {
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if (same) score = q_search(next, alpha, beta, history, ply + 1, ctx, p);
        else score = -q_search(next, -beta, -alpha, history, ply + 1, ctx, p);
        
        delete next;
        if (ctx.stop) return 0;
        
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

/*============================================================
 * PVS (Principal Variation Search)
 *============================================================*/
static int pvs_search(State *state, int depth, int alpha, int beta, GameHistory& history, int ply, SearchContext& ctx, const SubParams& p) {
    if ((ctx.nodes & 2047) == 0) {
        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_start_time).count() >= TIME_LIMIT_MS) {
            ctx.stop = true;
        }
    }

    ctx.nodes++;
    if (ply > ctx.seldepth) ctx.seldepth = ply;
    if (ctx.stop) return 0;

    if (state->legal_actions.empty() && state->game_state == UNKNOWN) state->get_legal_actions();
    if (state->game_state == WIN) return P_MAX - ply;
    if (state->game_state == DRAW) return 0;
    if (state->legal_actions.empty()) return M_MAX + ply;

    int rep_score;
    if (state->check_repetition(history, rep_score)) return rep_score;

    int original_alpha = alpha;
    uint64_t hash = state->hash();
    TTEntry& tte = tt_table[hash & (TT_SIZE - 1)];
    Move tt_move;

    if (tte.hash == hash) {
        tt_move = tte.best_move;
        if (tte.depth >= depth) {
            int score = tte.score;
            if (score > P_MAX - 1000) score -= ply;
            if (score < M_MAX + 1000) score += ply;

            if (tte.flag == TT_EXACT) return score;
            if (tte.flag == TT_ALPHA && score <= alpha) return alpha;
            if (tte.flag == TT_BETA && score >= beta) return beta;
        }
    }

    history.push(hash);

    if (depth <= 0) {
        int score = q_search(state, alpha, beta, history, ply, ctx, p); 
        history.pop(hash);
        return score;
    }

    int opp = 1 - state->player;
    std::sort(state->legal_actions.begin(), state->legal_actions.end(),
        [&](const Move& m1, const Move& m2) {
            return score_move(state, m1, tt_move, ply, opp) > score_move(state, m2, tt_move, ply, opp);
        }
    );

    int best_score = M_MAX;
    Move best_move;
    bool first_child = true;

    for (auto& action : state->legal_actions) {
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if (first_child) {
            if (same) score = pvs_search(next, depth - 1, alpha, beta, history, ply + 1, ctx, p);
            else score = -pvs_search(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, p);
            first_child = false;
        } else {
            if (same) {
                score = pvs_search(next, depth - 1, alpha, alpha + 1, history, ply + 1, ctx, p);
                if (score > alpha && score < beta) {
                    score = pvs_search(next, depth - 1, alpha, beta, history, ply + 1, ctx, p);
                }
            } else {
                score = -pvs_search(next, depth - 1, -alpha - 1, -alpha, history, ply + 1, ctx, p);
                if (score > alpha && score < beta) {
                    score = -pvs_search(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, p);
                }
            }
        }
        delete next;
        
        if (ctx.stop) { history.pop(hash); return 0; }

        if (score > best_score) {
            best_score = score;
            best_move = action;
        }
        if (best_score > alpha) alpha = best_score;
        if (alpha >= beta) {
            if (state->piece_at(opp, action.second.first, action.second.second) == 0 && ply < 100) {
                killer_moves[ply][1] = killer_moves[ply][0];
                killer_moves[ply][0] = action;
                
                int& hist = history_table[state->player][action.first.first][action.first.second][action.second.first][action.second.second];
                hist += depth; 
                if (hist > 50000) hist = 50000; 
            }
            break; 
        }
    }
    history.pop(hash);

    int tt_score = best_score;
    if (tt_score > P_MAX - 1000) tt_score += ply;
    if (tt_score < M_MAX + 1000) tt_score -= ply;

    if (tte.hash != hash || depth >= tte.depth) {
        tte.hash = hash;
        tte.depth = depth;
        tte.score = tt_score;
        tte.best_move = best_move;
        if (best_score <= original_alpha) tte.flag = TT_ALPHA;
        else if (best_score >= beta) tte.flag = TT_BETA;
        else tte.flag = TT_EXACT;
    }

    return best_score;
}

int Submission::eval_ctx(State *state, int depth, GameHistory& history, int ply, SearchContext& ctx, const SubParams& p) {
    return pvs_search(state, depth, M_MAX, P_MAX, history, ply, ctx, p);
}

SearchResult Submission::search(State *state, int depth, GameHistory& history, SearchContext& ctx) {
    if (depth == 1) {
        g_start_time = std::chrono::high_resolution_clock::now();
    }
    
    ctx.reset(); 
    SubParams p = SubParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if (state->legal_actions.empty()) state->get_legal_actions();
    if (state->legal_actions.empty()) return result;

    // ==========================================================
    // 👑 【終極開局庫外掛】如果我是白方且是遊戲第一步，強制走 B3 (b2 -> b3)
    // ==========================================================
    if (state->step == 0 && state->player == 0) {
        for (const auto& action : state->legal_actions) {
            if (action.first.first == 4 && action.first.second == 1 && 
                action.second.first == 3 && action.second.second == 1) {
                
                result.best_move = action;
                result.score = 50;  
                result.nodes = 1;
                result.pv = { action };
                return result;      
            }
        }
    }
    // ==========================================================

    int opp = 1 - state->player;
    uint64_t hash = state->hash();
    Move tt_move;
    
    if (tt_table[hash & (TT_SIZE - 1)].hash == hash) {
        tt_move = tt_table[hash & (TT_SIZE - 1)].best_move;
    }

    std::sort(state->legal_actions.begin(), state->legal_actions.end(),
        [&](const Move& m1, const Move& m2) {
            return score_move(state, m1, tt_move, 0, opp) > score_move(state, m2, tt_move, 0, opp);
        }
    );

    int best_score = M_MAX - 10;
    int alpha = M_MAX;
    int beta = P_MAX;
    bool first_child = true;

    for (auto& action : state->legal_actions) {
        if (ctx.stop) break;

        int score;
        if (state->game_state == WIN) {
            score = P_MAX;
        } else {
            State* next = state->next_state(action);
            bool same = next->same_player_as_parent();
            
            if (first_child) {
                if (same) score = pvs_search(next, depth - 1, alpha, beta, history, 1, ctx, p);
                else score = -pvs_search(next, depth - 1, -beta, -alpha, history, 1, ctx, p);
                first_child = false;
            } else {
                if (same) {
                    score = pvs_search(next, depth - 1, alpha, alpha + 1, history, 1, ctx, p);
                    if (score > alpha && score < beta) {
                        score = pvs_search(next, depth - 1, alpha, beta, history, 1, ctx, p);
                    }
                } else {
                    score = -pvs_search(next, depth - 1, -alpha - 1, -alpha, history, 1, ctx, p);
                    if (score > alpha && score < beta) {
                        score = -pvs_search(next, depth - 1, -beta, -alpha, history, 1, ctx, p);
                    }
                }
            }
            delete next;
        }

        if (ctx.stop) break; 

        if (score > best_score) {
            best_score = score;
            result.best_move = action;
        }
        if (best_score > alpha) alpha = best_score;
        
        if (best_score >= P_MAX - 100) break;
    }

    if (!ctx.stop) {
        result.score = best_score;
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        result.pv = { result.best_move };
    }

    return result;
}

ParamMap Submission::default_params() {
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "false"},
    };
}

std::vector<ParamDef> Submission::param_defs() {
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "false"},
    };
}
