#pragma once

#include <stdbool.h>

typedef enum {
    HIDE_STRATEGY_NONE = -1,
    HIDE_BY_SINK       = 0,
    HIDE_BY_CLOAK,
    HIDE_BY_STASH,
    HIDE_BY_SW_HIDE,
    HIDE_STRATEGY_COUNT,
} HideStrategyId;

typedef enum {
    HIDE_PLAN_CLOAK = 0,
    HIDE_PLAN_SHOWWINDOW,
} HidePlanPolicy;

typedef struct {
    bool sunk;
    bool cloaked;
    bool stashed;
    bool sw_hidden;
} HideFlags;

int hide_plan(HidePlanPolicy policy, HideStrategyId *out, int cap);

bool hide_strategy_is_off_screen(HideStrategyId id);

HideStrategyId hide_flags_strategy(const HideFlags *f);
bool           hide_flags_off_screen_without_showwindow(const HideFlags *f);
bool           hide_flags_any(const HideFlags *f);

HideStrategyId hide_outcome(HidePlanPolicy policy, const bool *can, int n);
