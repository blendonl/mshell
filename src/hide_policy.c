#include "hide_policy.h"

int hide_plan(HidePlanPolicy policy, HideStrategyId *out, int cap) {
    int n = 0;

    if (policy == HIDE_PLAN_CLOAK) {
        if (n < cap) out[n++] = HIDE_BY_SINK;
        if (n < cap) out[n++] = HIDE_BY_CLOAK;
        if (n < cap) out[n++] = HIDE_BY_STASH;
    }
    if (n < cap) out[n++] = HIDE_BY_SW_HIDE;

    return n;
}

bool hide_strategy_is_off_screen(HideStrategyId id) {
    return id == HIDE_BY_SINK || id == HIDE_BY_CLOAK || id == HIDE_BY_STASH;
}

HideStrategyId hide_flags_strategy(const HideFlags *f) {
    if (!f) return HIDE_STRATEGY_NONE;
    if (f->sunk)      return HIDE_BY_SINK;
    if (f->cloaked)   return HIDE_BY_CLOAK;
    if (f->stashed)   return HIDE_BY_STASH;
    if (f->sw_hidden) return HIDE_BY_SW_HIDE;
    return HIDE_STRATEGY_NONE;
}

bool hide_flags_off_screen_without_showwindow(const HideFlags *f) {
    return f && (f->sunk || f->cloaked || f->stashed);
}

bool hide_flags_any(const HideFlags *f) {
    return hide_flags_strategy(f) != HIDE_STRATEGY_NONE;
}

HideStrategyId hide_outcome(HidePlanPolicy policy, const bool *can, int n) {
    HideStrategyId plan[HIDE_STRATEGY_COUNT];
    int            steps = hide_plan(policy, plan, HIDE_STRATEGY_COUNT);

    for (int i = 0; i < steps; i++) {
        int id = (int)plan[i];
        if (id < n && can[id]) return plan[i];
    }
    return HIDE_STRATEGY_NONE;
}
