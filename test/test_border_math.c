#include "../src/border_math.h"
#include "tests.h"

static const BorderRect WORK = { 0, 0, 1920, 1040 };

static bool same(BorderRect a, BorderRect b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

static void test_a_floating_window_gets_a_ring_outside_its_frame(void) {
    BorderRect frame = { 400, 300, 900, 700 };
    BorderGeometry g = border_geometry(frame, WORK, 2, BORDER_ACCENT_NONE, 0);

    CHECK(g.visible, "a window away from the edges is ringed");
    CHECK(same(g.bounds, (BorderRect){398, 298, 902, 702}),
          "the ring grows outward by the border width");
    CHECK(g.has_ring, "the ring has a hole");
    CHECK(same(g.hole, (BorderRect){2, 2, 502, 402}),
          "the hole is the frame in overlay coordinates");
    CHECK(!g.has_accent, "no accent was asked for");
}

static void test_a_tiled_window_is_clamped_to_the_work_area(void) {
    BorderRect left_half = { 0, 0, 960, 1040 };
    BorderGeometry g = border_geometry(left_half, WORK, 2, BORDER_ACCENT_NONE, 0);

    CHECK(same(g.bounds, (BorderRect){0, 0, 962, 1040}),
          "only the edge facing the neighbour grows outward");
    CHECK(g.has_ring, "the clamped ring still has a hole");
}

static void test_the_accent_sits_inside_the_frame(void) {
    BorderRect frame = { 400, 300, 900, 700 };
    BorderGeometry g = border_geometry(frame, WORK, 2, BORDER_ACCENT_BOTTOM, 4);

    CHECK(g.has_accent, "a bottom accent is produced");
    CHECK(same(g.accent, (BorderRect){2, 398, 502, 402}),
          "the bar spans the frame and hugs its bottom edge from inside");
    CHECK(g.has_ring, "the ring is kept alongside the accent");
}

static void test_the_accent_can_ride_the_top_edge(void) {
    BorderRect frame = { 400, 300, 900, 700 };
    BorderGeometry g = border_geometry(frame, WORK, 2, BORDER_ACCENT_TOP, 4);

    CHECK(same(g.accent, (BorderRect){2, 2, 502, 6}),
          "a top accent hugs the top edge from inside");
}

static void test_the_accent_survives_a_window_flush_with_the_screen(void) {
    BorderRect bottom_half = { 0, 520, 1920, 1040 };
    BorderGeometry g = border_geometry(bottom_half, WORK, 2,
                                       BORDER_ACCENT_BOTTOM, 4);

    CHECK(same(g.bounds, (BorderRect){0, 518, 1920, 1040}),
          "the bounds clamp to the work area");
    CHECK(g.has_accent, "the bar is still drawn");
    CHECK(same(g.accent, (BorderRect){0, 518, 1920, 522}),
          "the bar stays inside the frame instead of falling off the screen");
}

static void test_an_accent_alone_is_enough_to_draw(void) {
    BorderRect frame = { 400, 300, 900, 700 };
    BorderGeometry g = border_geometry(frame, WORK, 0, BORDER_ACCENT_BOTTOM, 4);

    CHECK(g.visible, "a zero-width ring with an accent still draws");
    CHECK(!g.has_ring, "there is no ring to cut");
    CHECK(same(g.bounds, frame), "the overlay is exactly the frame");
    CHECK(same(g.accent, (BorderRect){0, 396, 500, 400}),
          "the bar hugs the bottom of the frame");
}

static void test_nothing_is_drawn_when_nothing_is_configured(void) {
    BorderRect frame = { 400, 300, 900, 700 };

    CHECK(!border_geometry(frame, WORK, 0, BORDER_ACCENT_NONE, 0).visible,
          "no width and no accent draws nothing");
    CHECK(!border_geometry(frame, WORK, 0, BORDER_ACCENT_BOTTOM, 0).visible,
          "an accent edge without a width draws nothing");
    CHECK(!border_geometry(frame, WORK, 2, BORDER_ACCENT_NONE, 4).has_accent,
          "an accent width without an edge is ignored");
    CHECK(!border_geometry(frame, WORK, -3, BORDER_ACCENT_BOTTOM, -1).visible,
          "negative sizes are treated as zero");
}

static void test_a_degenerate_frame_is_refused(void) {
    CHECK(!border_geometry((BorderRect){0, 0, 0, 0}, WORK, 2,
                           BORDER_ACCENT_BOTTOM, 4).visible,
          "an empty frame has no border");
    CHECK(!border_geometry((BorderRect){900, 700, 400, 300}, WORK, 2,
                           BORDER_ACCENT_NONE, 0).visible,
          "an inverted frame has no border");
}

static void test_a_window_too_small_for_a_ring_keeps_its_accent(void) {
    BorderRect tiny = { 400, 300, 406, 340 };
    BorderGeometry g = border_geometry(tiny, tiny, 4, BORDER_ACCENT_BOTTOM, 2);

    CHECK(!g.has_ring, "there is no room to cut a hole");
    CHECK(g.visible && g.has_accent, "the accent is drawn anyway");
    CHECK(same(g.accent, (BorderRect){0, 38, 6, 40}),
          "the bar still lands on the bottom edge");
}

static void test_the_accent_never_swallows_the_window(void) {
    BorderRect shallow = { 400, 300, 900, 306 };
    BorderGeometry g = border_geometry(shallow, WORK, 0,
                                       BORDER_ACCENT_BOTTOM, 40);

    CHECK(g.accent.bottom - g.accent.top == shallow.bottom - shallow.top,
          "the bar is capped at the height of the frame");
}

static void test_a_missing_work_area_falls_back_to_the_frame(void) {
    BorderRect frame = { 400, 300, 900, 700 };
    BorderRect none  = { 0, 0, 0, 0 };
    BorderGeometry g = border_geometry(frame, none, 2, BORDER_ACCENT_NONE, 0);

    CHECK(same(g.bounds, frame),
          "without a work area the ring cannot grow past the frame");
}

int main(void) {
    test_a_floating_window_gets_a_ring_outside_its_frame();
    test_a_tiled_window_is_clamped_to_the_work_area();
    test_the_accent_sits_inside_the_frame();
    test_the_accent_can_ride_the_top_edge();
    test_the_accent_survives_a_window_flush_with_the_screen();
    test_an_accent_alone_is_enough_to_draw();
    test_nothing_is_drawn_when_nothing_is_configured();
    test_a_degenerate_frame_is_refused();
    test_a_window_too_small_for_a_ring_keeps_its_accent();
    test_the_accent_never_swallows_the_window();
    test_a_missing_work_area_falls_back_to_the_frame();
    return tests_report("border_math");
}
