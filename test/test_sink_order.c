#include "../src/sink_order.h"
#include "tests.h"

#define O SINK_SLOT_OTHER
#define S SINK_SLOT_SUNK
#define W SINK_SLOT_SHOWN
#define B SINK_SLOT_BACKDROP

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int sunk_at_or_above(const SinkSlot *z, int count, int from) {
    int n = 0;
    for (int i = from; i < count; i++) n += (z[i] == S);
    return n;
}

static void test_helper_windows_between_sunk_ones_leave_the_backdrop_alone(void) {
    SinkSlot z[] = { O, S, O, O, S, O, S, O, O, B, O, W, O, W };
    SinkOrderPlan p = sink_order_plan(z, COUNT(z));

    CHECK(p.backdrop_below == -1,
          "every sunk window is already under the backdrop, yet it was asked "
          "to move under slot %d — the invisible popups a hidden app owns "
          "stack between its windows, and that is not a broken sink",
          p.backdrop_below);
    CHECK(sunk_at_or_above(z, COUNT(z), p.surfaced_from) == 0,
          "nothing has surfaced");
}

static void test_a_raised_backdrop_returns_above_the_highest_sunk_window(void) {
    SinkSlot z[] = { O, S, O, S, O, O, W, O, W, B };
    SinkOrderPlan p = sink_order_plan(z, COUNT(z));

    CHECK(p.backdrop_below == 4,
          "the backdrop goes directly above the highest sunk window (slot 3), "
          "not the lowest, or slot 3 surfaces; got %d", p.backdrop_below);
    CHECK(sunk_at_or_above(z, COUNT(z), p.surfaced_from) == 0,
          "nothing has surfaced");
}

static void test_a_window_just_hidden_sits_directly_under_the_backdrop(void) {
    SinkSlot z[] = { S, O, S, B, O, W };
    SinkOrderPlan p = sink_order_plan(z, COUNT(z));

    CHECK(p.backdrop_below == -1, "a fresh sink needs no fixing, got %d",
          p.backdrop_below);
}

static void test_a_sunk_window_above_a_shown_one_has_surfaced(void) {
    SinkSlot z[] = { S, B, W, O, S, W };
    SinkOrderPlan p = sink_order_plan(z, COUNT(z));

    CHECK(p.backdrop_below == -1,
          "the backdrop already covers what it should, got %d",
          p.backdrop_below);
    CHECK(p.surfaced_from == 2, "surfacing starts at the lowest shown window, "
          "got %d", p.surfaced_from);
    CHECK(sunk_at_or_above(z, COUNT(z), p.surfaced_from) == 1,
          "the sunk window above a shown one goes back under the backdrop");
}

static void test_a_backdrop_over_shown_windows_drops_below_them(void) {
    SinkSlot z[] = { O, W, S, B };
    SinkOrderPlan p = sink_order_plan(z, COUNT(z));

    CHECK(p.backdrop_below == 1,
          "with nothing sunk underneath, the backdrop goes just under the "
          "lowest shown window, got %d", p.backdrop_below);
    CHECK(sunk_at_or_above(z, COUNT(z), p.surfaced_from) == 1,
          "and the sunk window left above it is pushed under");
}

static void test_with_nothing_on_screen_only_the_sunk_windows_matter(void) {
    SinkSlot covered[] = { S, O, S, O, B, O };
    SinkOrderPlan p = sink_order_plan(covered, COUNT(covered));

    CHECK(p.backdrop_below == -1, "an empty desktop's backdrop may sit high, "
          "got %d", p.backdrop_below);
    CHECK(p.surfaced_from == COUNT(covered), "nothing can have surfaced");

    SinkSlot exposed[] = { S, B, O, S };
    p = sink_order_plan(exposed, COUNT(exposed));

    CHECK(p.backdrop_below == COUNT(exposed),
          "a sunk window at the top of the snapshot puts the backdrop above "
          "everything walked, got %d", p.backdrop_below);
}

static void test_a_backdrop_beyond_the_walk_is_brought_back(void) {
    SinkSlot z[] = { S, O, W };
    SinkOrderPlan p = sink_order_plan(z, COUNT(z));

    CHECK(p.backdrop_below == 1, "got %d", p.backdrop_below);
}

static void test_an_empty_snapshot_asks_for_nothing(void) {
    SinkOrderPlan p = sink_order_plan(NULL, 4);
    CHECK(p.backdrop_below == -1, "no snapshot, no move");

    SinkSlot z[] = { B };
    p = sink_order_plan(z, 0);
    CHECK(p.backdrop_below == -1 && p.surfaced_from == 0, "zero slots, no move");
}

int main(void) {
    test_helper_windows_between_sunk_ones_leave_the_backdrop_alone();
    test_a_raised_backdrop_returns_above_the_highest_sunk_window();
    test_a_window_just_hidden_sits_directly_under_the_backdrop();
    test_a_sunk_window_above_a_shown_one_has_surfaced();
    test_a_backdrop_over_shown_windows_drops_below_them();
    test_with_nothing_on_screen_only_the_sunk_windows_matter();
    test_a_backdrop_beyond_the_walk_is_brought_back();
    test_an_empty_snapshot_asks_for_nothing();
    return tests_report("sink_order");
}
