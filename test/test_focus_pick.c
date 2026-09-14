#include "../src/focus_pick.h"
#include "tests.h"

static FocusCandidate tile(long x, long y) {
    FocusCandidate c = { x, y, true, true };
    return c;
}

static FocusCandidate app_hidden(long x, long y) {
    FocusCandidate c = { x, y, false, false };
    return c;
}

static FocusCandidate off_screen(long x, long y) {
    FocusCandidate c = { x, y, true, false };
    return c;
}

static void test_the_nearest_window_in_the_direction_wins(void) {
    FocusCandidate row[] = { tile(480, 520), tile(1440, 260), tile(1440, 780) };

    CHECK(focus_pick_neighbor(row, 3, 0, FOCUS_RIGHT) == 1,
          "right from the master picks the closest stack window");
    CHECK(focus_pick_neighbor(row, 3, 1, FOCUS_LEFT) == 0,
          "left from the stack returns to the master");
    CHECK(focus_pick_neighbor(row, 3, 1, FOCUS_DOWN) == 2,
          "down walks the stack");
    CHECK(focus_pick_neighbor(row, 3, 0, FOCUS_LEFT) == -1,
          "nothing lies left of the leftmost window");
}

static void test_a_window_the_app_hid_is_never_a_neighbour(void) {
    FocusCandidate row[] = { tile(480, 520), app_hidden(900, 520),
                             tile(1440, 520) };

    CHECK(focus_pick_neighbor(row, 3, 0, FOCUS_RIGHT) == 2,
          "right skips the invisible popup sitting between two tiles");
    CHECK(focus_pick_neighbor(row, 3, 2, FOCUS_LEFT) == 0,
          "left skips it too");
}

static void test_a_window_off_screen_is_never_a_neighbour(void) {
    FocusCandidate row[] = { tile(480, 520), off_screen(900, 520),
                             tile(1440, 520) };

    CHECK(focus_pick_neighbor(row, 3, 0, FOCUS_RIGHT) == 2,
          "a minimized or layout-hidden window has no place to move to");
}

static void test_only_hidden_windows_in_the_direction_finds_nothing(void) {
    FocusCandidate row[] = { tile(480, 520), app_hidden(1440, 520) };

    CHECK(focus_pick_neighbor(row, 2, 0, FOCUS_RIGHT) == -1,
          "a hidden window alone to the right is not a target");
}

static void test_an_origin_without_a_place_has_no_neighbours(void) {
    FocusCandidate row[] = { off_screen(480, 520), tile(1440, 520) };

    CHECK(focus_pick_neighbor(row, 2, 0, FOCUS_RIGHT) == -1,
          "direction is meaningless from a window that is not on screen");
    CHECK(focus_pick_neighbor(row, 2, 5, FOCUS_RIGHT) == -1,
          "an origin past the end is refused");
    CHECK(focus_pick_neighbor(row, 2, -1, FOCUS_RIGHT) == -1,
          "a negative origin is refused");
    CHECK(focus_pick_neighbor(NULL, 2, 0, FOCUS_RIGHT) == -1,
          "no candidates finds nothing");
}

static void test_cycling_steps_over_hidden_windows(void) {
    FocusCandidate ring[] = { tile(0, 0), app_hidden(0, 0), tile(0, 0),
                              app_hidden(0, 0) };

    CHECK(focus_pick_cycle(ring, 4, 0, false) == 2,
          "next skips the hidden window after the current one");
    CHECK(focus_pick_cycle(ring, 4, 2, false) == 0,
          "next wraps past a hidden window at the end");
    CHECK(focus_pick_cycle(ring, 4, 0, true) == 2,
          "prev wraps past a hidden window at the end");
    CHECK(focus_pick_cycle(ring, 4, 2, true) == 0,
          "prev skips the hidden window before the current one");
}

static void test_cycling_keeps_windows_that_are_only_off_screen(void) {
    FocusCandidate ring[] = { tile(0, 0), off_screen(0, 0), off_screen(0, 0) };

    CHECK(focus_pick_cycle(ring, 3, 0, false) == 1,
          "monocle cycles into windows its layout tucked away");
    CHECK(focus_pick_cycle(ring, 3, 0, true) == 2,
          "and back the other way");
}

static void test_cycling_with_nothing_else_to_focus_finds_nothing(void) {
    FocusCandidate lone[] = { tile(0, 0), app_hidden(0, 0) };

    CHECK(focus_pick_cycle(lone, 2, 0, false) == -1,
          "the current window is not its own next");
    CHECK(focus_pick_cycle(lone, 2, 0, true) == -1,
          "nor its own prev");
    CHECK(focus_pick_cycle(lone, 0, 0, false) == -1,
          "an empty desktop finds nothing");
    CHECK(focus_pick_cycle(NULL, 2, 0, false) == -1,
          "no candidates finds nothing");
}

int main(void) {
    test_the_nearest_window_in_the_direction_wins();
    test_a_window_the_app_hid_is_never_a_neighbour();
    test_a_window_off_screen_is_never_a_neighbour();
    test_only_hidden_windows_in_the_direction_finds_nothing();
    test_an_origin_without_a_place_has_no_neighbours();
    test_cycling_steps_over_hidden_windows();
    test_cycling_keeps_windows_that_are_only_off_screen();
    test_cycling_with_nothing_else_to_focus_finds_nothing();
    return tests_report("focus_pick");
}
