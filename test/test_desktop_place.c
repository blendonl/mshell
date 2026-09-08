#include "../src/desktop_place.h"
#include "tests.h"

#define MAX_MON 8

static const char *kind_name(DesktopSwitchKind k) {
    switch (k) {
    case DESKTOP_SWITCH_ALREADY_HERE: return "already-here";
    case DESKTOP_SWITCH_SWAP:         return "swap";
    case DESKTOP_SWITCH_REPLACE:      return "replace";
    default:                          return "?";
    }
}

static void test_visible_on(void) {
    int md[4] = {7, 0, 3, 0};

    CHECK(desktop_visible_on(md, 4, 7) == 0, "a desktop on monitor 0 is found");
    CHECK(desktop_visible_on(md, 4, 3) == 2, "a desktop on monitor 2 is found");
    CHECK(desktop_visible_on(md, 4, 9) == -1, "an unseen desktop is not found");
    CHECK(desktop_visible_on(md, 4, 0) == -1,
          "id 0 means empty and is never reported as visible");
    CHECK(desktop_visible_on(md, 1, 3) == -1,
          "a monitor outside the span is not searched");
    CHECK(desktop_visible_on(NULL, 4, 3) == -1, "a NULL table finds nothing");
}

static void test_empty_monitor(void) {
    int none[3] = {1, 2, 3};
    int some[3] = {1, 0, 3};

    CHECK(desktop_empty_monitor(none, 3) == -1, "a full table has no gap");
    CHECK(desktop_empty_monitor(some, 3) == 1, "the first gap is reported");
    CHECK(desktop_empty_monitor(some, 1) == -1, "the span bounds the search");
}

static void test_switch_to_the_desktop_already_here(void) {
    int md[2] = {5, 6};

    DesktopSwitchPlan p = desktop_switch_plan(md, 2, 5, 0);
    CHECK(p.kind == DESKTOP_SWITCH_ALREADY_HERE,
          "switching to the desktop already on the target monitor is a no-op");
    CHECK(p.mon == 0, "the monitor is the one it is already on");
    CHECK(p.prev_id == 5, "the previous desktop is itself");
    CHECK(!p.needs_fill, "nothing needs filling");

    int before[2] = {5, 6};
    desktop_switch_apply(md, 2, &p, 5);
    CHECK(md[0] == before[0] && md[1] == before[1],
          "applying a no-op plan changes nothing");
}

static void test_switch_to_a_hidden_desktop_replaces(void) {
    int md[2] = {5, 6};

    DesktopSwitchPlan p = desktop_switch_plan(md, 2, 9, 1);
    CHECK(p.kind == DESKTOP_SWITCH_REPLACE,
          "a desktop that is not visible replaces what is on the monitor");
    CHECK(p.mon == 1, "it lands on the preferred monitor");
    CHECK(p.prev_id == 6, "the displaced desktop is reported");
    CHECK(p.other == -1, "there is no other monitor involved");

    desktop_switch_apply(md, 2, &p, 9);
    CHECK(md[0] == 5, "the untouched monitor keeps its desktop");
    CHECK(md[1] == 9, "the target monitor now shows the target");
}

static void test_preferred_monitor_is_clamped(void) {
    int md[2] = {5, 6};

    DesktopSwitchPlan hi = desktop_switch_plan(md, 2, 9, 7);
    CHECK(hi.mon == 0, "a preferred monitor past the span falls back to 0");

    DesktopSwitchPlan lo = desktop_switch_plan(md, 2, 9, -3);
    CHECK(lo.mon == 0, "a negative preferred monitor falls back to 0");
}

static void test_replacing_onto_an_empty_monitor(void) {
    int md[2] = {5, 0};

    DesktopSwitchPlan p = desktop_switch_plan(md, 2, 9, 1);
    CHECK(p.kind == DESKTOP_SWITCH_REPLACE, "an empty monitor is replaced into");
    CHECK(p.prev_id == 0, "nothing was displaced");

    desktop_switch_apply(md, 2, &p, 9);
    CHECK(md[1] == 9, "the empty monitor now shows the target");
    CHECK(desktop_empty_monitor(md, 2) == -1, "no monitor is left empty");
}

static void test_swap_exchanges_the_two_monitors(void) {
    int md[3] = {1, 2, 3};

    DesktopSwitchPlan p = {DESKTOP_SWITCH_SWAP, 0, 1, 2, false};
    desktop_switch_apply(md, 3, &p, 3);

    CHECK(md[0] == 3, "the target moved to the requested monitor");
    CHECK(md[2] == 1, "the displaced desktop took the target's old monitor");
    CHECK(md[1] == 2, "the uninvolved monitor is untouched");
    CHECK(desktop_empty_monitor(md, 3) == -1, "no monitor is left empty");
}

static void test_swap_out_of_an_empty_monitor_needs_a_fill(void) {
    int md[3] = {0, 2, 3};

    DesktopSwitchPlan p = {DESKTOP_SWITCH_SWAP, 0, 0, 2, true};
    desktop_switch_apply(md, 3, &p, 3);

    CHECK(md[0] == 3, "the target moved to the empty monitor");
    CHECK(md[2] == 0, "its old monitor is now empty");
    CHECK(desktop_empty_monitor(md, 3) == 2,
          "a swap out of an empty monitor leaves one empty, hence needs_fill");
    CHECK(p.needs_fill, "the plan flagged that a fill is required");
}

static void test_apply_ignores_an_out_of_range_plan(void) {
    int md[2] = {1, 2};

    DesktopSwitchPlan bad_mon   = {DESKTOP_SWITCH_REPLACE, 5, 0, -1, false};
    DesktopSwitchPlan bad_other = {DESKTOP_SWITCH_SWAP, 0, 1, 9, false};

    desktop_switch_apply(md, 2, &bad_mon, 7);
    CHECK(md[0] == 1 && md[1] == 2, "an out-of-range monitor is ignored");

    desktop_switch_apply(md, 2, &bad_other, 7);
    CHECK(md[0] == 1 && md[1] == 2, "an out-of-range swap partner is ignored");

    desktop_switch_apply(NULL, 2, &bad_mon, 7);
    desktop_switch_apply(md, 2, NULL, 7);
    CHECK(md[0] == 1 && md[1] == 2, "NULL arguments are ignored");
}

static void test_a_visible_desktop_always_short_circuits(void) {
    int checked = 0;

    for (int span = 1; span <= 4; span++) {
        for (unsigned config = 0; config < 2401u; config++) {
            int md[4];
            unsigned c = config;
            for (int m = 0; m < span; m++) { md[m] = (int)(c % 7u); c /= 7u; }

            for (int target = 1; target <= 6; target++) {
                for (int pref = -1; pref <= span; pref++) {
                    DesktopSwitchPlan p =
                        desktop_switch_plan(md, span, target, pref);

                    int shown = desktop_visible_on(md, span, target);
                    checked++;

                    if (shown >= 0) {
                        CHECK(p.kind == DESKTOP_SWITCH_ALREADY_HERE,
                              "span %d config %u target %d: a visible desktop "
                              "short-circuits, got %s",
                              span, config, target, kind_name(p.kind));
                        CHECK(p.mon == shown,
                              "span %d config %u target %d: it focuses the "
                              "monitor already showing it", span, config,
                              target);
                    } else {
                        CHECK(p.kind == DESKTOP_SWITCH_REPLACE,
                              "span %d config %u target %d: a hidden desktop "
                              "replaces, got %s",
                              span, config, target, kind_name(p.kind));
                    }
                }
            }
        }
    }

    CHECK(checked > 100000, "the sweep covered a real space, %d cases", checked);
}

static void test_apply_never_loses_a_desktop(void) {
    for (int span = 1; span <= 4; span++) {
        for (unsigned config = 0; config < 2401u; config++) {
            int md[4], before[4];
            unsigned c = config;
            for (int m = 0; m < span; m++) {
                md[m] = (int)(c % 7u);
                before[m] = md[m];
                c /= 7u;
            }

            for (int target = 1; target <= 6; target++) {
                for (int m = 0; m < span; m++) md[m] = before[m];

                DesktopSwitchPlan p =
                    desktop_switch_plan(md, span, target, target % span);
                desktop_switch_apply(md, span, &p, target);

                if (p.kind == DESKTOP_SWITCH_ALREADY_HERE) continue;

                CHECK(desktop_visible_on(md, span, target) >= 0,
                      "span %d config %u target %d: the target ends up visible",
                      span, config, target);

                int seen = 0;
                for (int m = 0; m < span; m++)
                    if (md[m] == target) seen++;
                CHECK(seen == 1,
                      "span %d config %u target %d: the target is on exactly "
                      "one monitor, saw %d", span, config, target, seen);
            }
        }
    }
}

int main(void) {
    test_visible_on();
    test_empty_monitor();
    test_switch_to_the_desktop_already_here();
    test_switch_to_a_hidden_desktop_replaces();
    test_preferred_monitor_is_clamped();
    test_replacing_onto_an_empty_monitor();
    test_swap_exchanges_the_two_monitors();
    test_swap_out_of_an_empty_monitor_needs_a_fill();
    test_apply_ignores_an_out_of_range_plan();
    test_a_visible_desktop_always_short_circuits();
    test_apply_never_loses_a_desktop();
    return tests_report("desktop_place");
}
