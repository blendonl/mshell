#include "../src/hide_policy.h"
#include "tests.h"

static const char *name_of(HideStrategyId id) {
    switch (id) {
    case HIDE_BY_SINK:       return "sink";
    case HIDE_BY_CLOAK:      return "cloak";
    case HIDE_BY_STASH:      return "stash";
    case HIDE_BY_SW_HIDE:    return "SW_HIDE";
    case HIDE_STRATEGY_NONE: return "none";
    default:                 return "?";
    }
}

static void test_cloak_policy_plan(void) {
    HideStrategyId plan[HIDE_STRATEGY_COUNT];
    int n = hide_plan(HIDE_PLAN_CLOAK, plan, HIDE_STRATEGY_COUNT);

    CHECK(n == 4, "the cloak policy tries every strategy, got %d", n);
    CHECK(plan[0] == HIDE_BY_SINK, "sinking is tried first");
    CHECK(plan[1] == HIDE_BY_CLOAK, "cloaking is tried second");
    CHECK(plan[2] == HIDE_BY_STASH, "stashing is tried third");
    CHECK(plan[3] == HIDE_BY_SW_HIDE, "SW_HIDE is the last resort");
}

static void test_showwindow_policy_plan(void) {
    HideStrategyId plan[HIDE_STRATEGY_COUNT];
    int n = hide_plan(HIDE_PLAN_SHOWWINDOW, plan, HIDE_STRATEGY_COUNT);

    CHECK(n == 1, "the showwindow policy tries one strategy, got %d", n);
    CHECK(plan[0] == HIDE_BY_SW_HIDE,
          "the showwindow policy goes straight to SW_HIDE");
}

static void test_plan_respects_a_short_buffer(void) {
    for (int cap = 0; cap <= HIDE_STRATEGY_COUNT; cap++) {
        HideStrategyId plan[HIDE_STRATEGY_COUNT + 2];
        for (int i = 0; i < HIDE_STRATEGY_COUNT + 2; i++)
            plan[i] = (HideStrategyId)99;

        int n = hide_plan(HIDE_PLAN_CLOAK, plan, cap);
        CHECK(n <= cap, "cap %d: the plan never exceeds the buffer", cap);
        CHECK(plan[cap] == (HideStrategyId)99,
              "cap %d: nothing is written past the buffer", cap);
    }
}

static void test_off_screen_classification(void) {
    CHECK(hide_strategy_is_off_screen(HIDE_BY_SINK), "sinking is off screen");
    CHECK(hide_strategy_is_off_screen(HIDE_BY_CLOAK), "cloaking is off screen");
    CHECK(hide_strategy_is_off_screen(HIDE_BY_STASH), "stashing is off screen");
    CHECK(!hide_strategy_is_off_screen(HIDE_BY_SW_HIDE),
          "SW_HIDE is not an off-screen move, it unmaps the window");
    CHECK(!hide_strategy_is_off_screen(HIDE_STRATEGY_NONE),
          "no strategy is not off screen");
}

static void test_flags_report_the_active_strategy(void) {
    HideFlags none = {false, false, false, false};
    CHECK(hide_flags_strategy(&none) == HIDE_STRATEGY_NONE,
          "a visible window has no strategy");
    CHECK(!hide_flags_any(&none), "a visible window is not hidden");
    CHECK(!hide_flags_off_screen_without_showwindow(&none),
          "a visible window is not off screen");

    HideFlags sunk = {true, false, false, false};
    CHECK(hide_flags_strategy(&sunk) == HIDE_BY_SINK, "a sunk window reports sink");
    CHECK(hide_flags_off_screen_without_showwindow(&sunk),
          "a sunk window is off screen without SW_HIDE");

    HideFlags sw = {false, false, false, true};
    CHECK(hide_flags_strategy(&sw) == HIDE_BY_SW_HIDE,
          "an SW_HIDE window reports SW_HIDE");
    CHECK(!hide_flags_off_screen_without_showwindow(&sw),
          "SW_HIDE alone does not count as off screen");
    CHECK(hide_flags_any(&sw), "an SW_HIDE window is hidden");

    CHECK(hide_flags_strategy(NULL) == HIDE_STRATEGY_NONE,
          "a NULL flag set reports no strategy");
    CHECK(!hide_flags_off_screen_without_showwindow(NULL),
          "a NULL flag set is not off screen");
}

static void test_flags_priority_when_several_are_set(void) {
    HideFlags all = {true, true, true, true};
    CHECK(hide_flags_strategy(&all) == HIDE_BY_SINK,
          "sinking wins the name when several are set");

    HideFlags cloak_stash = {false, true, true, true};
    CHECK(hide_flags_strategy(&cloak_stash) == HIDE_BY_CLOAK,
          "cloaking outranks stashing");

    HideFlags stash_sw = {false, false, true, true};
    CHECK(hide_flags_strategy(&stash_sw) == HIDE_BY_STASH,
          "stashing outranks SW_HIDE");
}

static HideStrategyId walk_plan(HidePlanPolicy policy, const bool *can) {
    HideStrategyId plan[HIDE_STRATEGY_COUNT];
    int steps = hide_plan(policy, plan, HIDE_STRATEGY_COUNT);

    for (int i = 0; i < steps; i++)
        if (can[plan[i]]) return plan[i];
    return HIDE_STRATEGY_NONE;
}

static void test_outcome_matches_walking_the_plan(void) {
    for (int policy = 0; policy <= 1; policy++) {
        for (unsigned mask = 0; mask < 16u; mask++) {
            bool can[HIDE_STRATEGY_COUNT];
            for (int i = 0; i < HIDE_STRATEGY_COUNT; i++)
                can[i] = (mask >> i) & 1u;

            HideStrategyId modelled =
                hide_outcome((HidePlanPolicy)policy, can, HIDE_STRATEGY_COUNT);
            HideStrategyId walked = walk_plan((HidePlanPolicy)policy, can);

            CHECK(modelled == walked,
                  "policy %d mask %u: the model says %s, walking says %s",
                  policy, mask, name_of(modelled), name_of(walked));
        }
    }
}

static void test_outcome_under_the_cloak_policy(void) {
    bool none[4]  = {false, false, false, false};
    bool sink[4]  = {true,  true,  true,  true};
    bool cloak[4] = {false, true,  true,  true};
    bool stash[4] = {false, false, true,  true};
    bool sw[4]    = {false, false, false, true};

    CHECK(hide_outcome(HIDE_PLAN_CLOAK, none, 4) == HIDE_STRATEGY_NONE,
          "when nothing works the hide fails");
    CHECK(hide_outcome(HIDE_PLAN_CLOAK, sink, 4) == HIDE_BY_SINK,
          "sinking is preferred when available");
    CHECK(hide_outcome(HIDE_PLAN_CLOAK, cloak, 4) == HIDE_BY_CLOAK,
          "cloaking is next when sinking is refused");
    CHECK(hide_outcome(HIDE_PLAN_CLOAK, stash, 4) == HIDE_BY_STASH,
          "stashing is next when cloaking is refused");
    CHECK(hide_outcome(HIDE_PLAN_CLOAK, sw, 4) == HIDE_BY_SW_HIDE,
          "SW_HIDE catches everything else");
}

static void test_outcome_under_the_showwindow_policy(void) {
    bool all_but_sw[4] = {true, true, true, false};
    CHECK(hide_outcome(HIDE_PLAN_SHOWWINDOW, all_but_sw, 4) ==
              HIDE_STRATEGY_NONE,
          "the showwindow policy ignores sink, cloak and stash entirely");

    bool only_sw[4] = {false, false, false, true};
    CHECK(hide_outcome(HIDE_PLAN_SHOWWINDOW, only_sw, 4) == HIDE_BY_SW_HIDE,
          "the showwindow policy uses SW_HIDE");
}

static void test_a_failed_hide_is_reported_not_assumed(void) {
    bool nothing[4] = {false, false, false, false};

    CHECK(hide_outcome(HIDE_PLAN_CLOAK, nothing, 4) == HIDE_STRATEGY_NONE,
          "a window that refuses every strategy reports no strategy");
    CHECK(hide_outcome(HIDE_PLAN_SHOWWINDOW, nothing, 4) == HIDE_STRATEGY_NONE,
          "the same holds under the showwindow policy");

    HideFlags after = {false, false, false, false};
    CHECK(!hide_flags_any(&after),
          "a window that could not be hidden is still on screen");
}

int main(void) {
    test_cloak_policy_plan();
    test_showwindow_policy_plan();
    test_plan_respects_a_short_buffer();
    test_off_screen_classification();
    test_flags_report_the_active_strategy();
    test_flags_priority_when_several_are_set();
    test_outcome_matches_walking_the_plan();
    test_outcome_under_the_cloak_policy();
    test_outcome_under_the_showwindow_policy();
    test_a_failed_hide_is_reported_not_assumed();
    return tests_report("hide_policy");
}
