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

/*============================================================
 * Move Scoring for Ordering
 *============================================================*/
static int score_move(State* state, const Move& m, const Move& tt_move, int ply, int opp) {
    if (m == tt_move) return 10000000; 

    int victim = state->piece_at(opp, m.second.first, m.second.second);
    if (victim > 0) {
        int attacker = state->piece_at(state->player, m.first.first, m.first.second);
        return 1000000 + victim * 10 - attacker; 
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

    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
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
        int val1 = state->piece_at(opp, m1.second.first, m1.second.second);
        int val2 = state->piece_at(opp, m2.second.first, m2.second.second);
        return val1 > val2;
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
                hist += depth; // 修正: 降低歷史過度自信
                if (hist > 50000) hist = 50000; 
            }
            break; 
        }
    }
    history.pop(hash);

    int tt_score = best_score;
    if (tt_score > P_MAX - 1000) tt_score += ply;
    if (tt_score < M_MAX + 1000) tt_score -= ply;

    // 修正: 正確的 TT 替換邏輯 (保留深層記憶)
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
        
        // 【最關鍵修復】: 徹底拔除 ctx.on_root_update()！
        // 避免超時被 cli.py 誤判為走垃圾步而暴斃
        
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