#include <utility>
#include <algorithm>
#include <vector>
#include "state.hpp"
#include "minimax.hpp"

/*============================================================
 * Quiescence Search
 *============================================================*/
static int q_search(
    State *state,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
) {
    ctx.nodes++;
    if (ply > ctx.seldepth) ctx.seldepth = ply;
    if (ctx.stop) return 0;

    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if (stand_pat >= beta) {
        return beta;
    }
    if (alpha < stand_pat) {
        alpha = stand_pat;
    }

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

    std::sort(captures.begin(), captures.end(),
        [state, opp](const Move& m1, const Move& m2) {
            int val1 = state->piece_at(opp, m1.second.first, m1.second.second);
            int val2 = state->piece_at(opp, m2.second.first, m2.second.second);
            return val1 > val2;
        }
    );

    for (auto& action : captures) {
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if (same) {
            score = q_search(next, alpha, beta, history, ply + 1, ctx, p);
        } else {
            score = -q_search(next, -beta, -alpha, history, ply + 1, ctx, p);
        }
        
        delete next;

        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

/*============================================================
 * PVS (Principal Variation Search)
 *============================================================*/
static int pvs_search(
    State *state,
    int depth,
    int alpha,
    int beta,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
) {
    ctx.nodes++;
    if (ply > ctx.seldepth) ctx.seldepth = ply;
    if (ctx.stop) return 0;

    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }

    if (state->game_state == WIN) return P_MAX - ply;
    if (state->game_state == DRAW) return 0;

    int rep_score;
    if (state->check_repetition(history, rep_score)) {
        return rep_score;
    }
    history.push(state->hash());

    if (depth <= 0) {
        int score = q_search(state, alpha, beta, history, ply, ctx, p); 
        history.pop(state->hash());
        return score;
    }

    int opp = 1 - state->player;
    std::sort(state->legal_actions.begin(), state->legal_actions.end(),
        [state, opp](const Move& m1, const Move& m2) {
            int val1 = state->piece_at(opp, m1.second.first, m1.second.second);
            int val2 = state->piece_at(opp, m2.second.first, m2.second.second);
            return val1 > val2;
        }
    );

    int best_score = M_MAX;
    bool first_child = true;

    for (auto& action : state->legal_actions) {
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int score;

        if (first_child) {
            if (same) {
                score = pvs_search(next, depth - 1, alpha, beta, history, ply + 1, ctx, p);
            } else {
                score = -pvs_search(next, depth - 1, -beta, -alpha, history, ply + 1, ctx, p);
            }
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

        if (score > best_score) best_score = score;
        if (best_score > alpha) alpha = best_score;
        if (alpha >= beta) break; 
    }

    history.pop(state->hash());
    return best_score;
}

/*============================================================
 * MiniMax — Public Interface
 *============================================================*/
int MiniMax::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p
) {
    return pvs_search(state, depth, M_MAX, P_MAX, history, ply, ctx, p);
}

SearchResult MiniMax::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
) {
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    if (!state->legal_actions.size()) {
        state->get_legal_actions();
    }

    int opp = 1 - state->player;
    std::sort(state->legal_actions.begin(), state->legal_actions.end(),
        [state, opp](const Move& m1, const Move& m2) {
            int val1 = state->piece_at(opp, m1.second.first, m1.second.second);
            int val2 = state->piece_at(opp, m2.second.first, m2.second.second);
            return val1 > val2;
        }
    );

    int best_score = M_MAX - 10;
    int alpha = M_MAX;
    int beta = P_MAX;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();
    bool first_child = true;

    for (auto& action : state->legal_actions) {
        int score;

        if (state->game_state == WIN) {
            score = (move_index == 0) ? P_MAX : M_MAX;
        } else {
            State* next = state->next_state(action);
            if (first_child) {
                if (next->same_player_as_parent()) {
                    score = pvs_search(next, depth - 1, alpha, beta, history, 1, ctx, p);
                } else {
                    score = -pvs_search(next, depth - 1, -beta, -alpha, history, 1, ctx, p);
                }
                first_child = false;
            } else {
                if (next->same_player_as_parent()) {
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

        if (score > best_score) {
            best_score = score;
            result.best_move = action;
            if (p.report_partial && ctx.on_root_update) {
               ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        } 
        if (best_score > alpha) alpha = best_score;
        move_index++;
    }

    result.score = best_score;
    result.nodes = ctx.nodes;
    if (!state->legal_actions.empty()) {
        result.pv = { result.best_move };
    }
    return result;
}

ParamMap MiniMax::default_params() {
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> MiniMax::param_defs() {
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}