/*
 * test_layout_math.c — split_span().
 *
 * The invariant under test is that the cells sum to EXACTLY the span. Getting
 * that wrong by one pixel is a seam or an overlap between adjacent tiled
 * windows — visible, annoying, and very hard to attribute to arithmetic when
 * you are looking at a screen rather than at numbers.
 */

#include "tests.h"
#include "../src/layout_math.h"

/* Every test asserts the invariant, so assert it in one place. */
static int sum(const int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

static void check_exact(int span, const float *facts, int n, const char *what) {
    int out[64];
    split_span(span, facts, n, out);
    tests_run++;
    if (sum(out, n) != span) {
        tests_failed++;
        printf("  FAIL  %s: %d cells over span %d summed to %d\n",
               what, n, span, sum(out, n));
    }
}

int main(void) {
    int   out[64];
    float f[64];

    /* --- equal weights divide evenly when they can --- */
    for (int i = 0; i < 4; i++) f[i] = 1.0f;
    split_span(800, f, 4, out);
    CHECK(out[0] == 200 && out[1] == 200 && out[2] == 200 && out[3] == 200,
          "800/4 gave %d,%d,%d,%d", out[0], out[1], out[2], out[3]);

    /* --- and the remainder lands on the LAST cell, not scattered --- */
    split_span(801, f, 4, out);
    CHECK(out[0] == 200 && out[1] == 200 && out[2] == 200 && out[3] == 201,
          "801/4 gave %d,%d,%d,%d", out[0], out[1], out[2], out[3]);

    split_span(1079, f, 3, out);
    CHECK(sum(out, 3) == 1079, "1079/3 summed to %d", sum(out, 3));

    /* --- a single cell takes everything --- */
    f[0] = 1.0f;
    split_span(1234, f, 1, out);
    CHECK(out[0] == 1234, "single cell got %d, expected 1234", out[0]);

    /* --- weights are respected: 2:1 over 900 is 600/300 --- */
    f[0] = 2.0f; f[1] = 1.0f;
    split_span(900, f, 2, out);
    CHECK(out[0] == 600 && out[1] == 300, "2:1 of 900 gave %d,%d",
          out[0], out[1]);

    /* --- cfact clamps at 0.25..4.0, so check the extremes of that range --- */
    f[0] = 4.0f; f[1] = 0.25f;
    split_span(850, f, 2, out);
    CHECK(sum(out, 2) == 850, "4:0.25 summed to %d", sum(out, 2));
    CHECK(out[0] > out[1], "the 4.0 cell (%d) should exceed the 0.25 cell (%d)",
          out[0], out[1]);

    /* --- a non-positive factor is treated as 1.0, not as zero or a divide by
     *     zero. A ManagedWindow zeroed by memset has cfact 0. --- */
    f[0] = 0.0f; f[1] = 0.0f;
    split_span(600, f, 2, out);
    CHECK(out[0] == 300 && out[1] == 300, "zero factors gave %d,%d",
          out[0], out[1]);

    f[0] = -5.0f; f[1] = 1.0f;
    split_span(400, f, 2, out);
    CHECK(sum(out, 2) == 400, "negative factor summed to %d", sum(out, 2));

    /* --- degenerate inputs must not write or crash --- */
    out[0] = 0xdead;
    split_span(500, f, 0, out);
    CHECK(out[0] == 0xdead, "n=0 wrote to out[]");
    split_span(500, f, -1, out);
    CHECK(out[0] == 0xdead, "n<0 wrote to out[]");

    /* A zero-width span still has to partition exactly. */
    f[0] = f[1] = f[2] = 1.0f;
    split_span(0, f, 3, out);
    CHECK(sum(out, 3) == 0, "zero span summed to %d", sum(out, 3));

    /* --- the invariant, swept across realistic screen sizes and counts --- */
    const int spans[] = { 1080, 1440, 2160, 1366, 1050, 997, 1, 7 };
    for (size_t s = 0; s < sizeof spans / sizeof spans[0]; s++) {
        for (int n = 1; n <= 16; n++) {
            for (int i = 0; i < n; i++) f[i] = 1.0f;
            check_exact(spans[s], f, n, "uniform");

            /* mixed weights, still exact */
            for (int i = 0; i < n; i++) f[i] = 0.25f + (float)(i % 8) * 0.5f;
            check_exact(spans[s], f, n, "mixed");
        }
    }

    /* --- center_axis(): where a floating window sits on its monitor ------- */

    /* The ordinary case, on a monitor that does not start at zero — a second
     * display's origin is its position in the virtual desktop, and forgetting
     * to add it is how a "centred" window lands on the primary instead. */
    CHECK(center_axis(0, 1920, 800) == 560, "1920/800 gave %d, expected 560",
          center_axis(0, 1920, 800));
    CHECK(center_axis(1920, 1920, 800) == 2480,
          "second monitor gave %d, expected 2480", center_axis(1920, 1920, 800));
    CHECK(center_axis(-1080, 1080, 500) == -790,
          "monitor left of the primary gave %d, expected -790",
          center_axis(-1080, 1080, 500));

    /* An odd leftover goes left/up, and the whole box stays inside the span. */
    CHECK(center_axis(0, 1001, 500) == 250, "odd leftover gave %d, expected 250",
          center_axis(0, 1001, 500));
    CHECK(center_axis(0, 1000, 999) == 0, "one spare pixel gave %d, expected 0",
          center_axis(0, 1000, 999));

    /* Symmetry: the gap left of the box equals the gap right of it, or is one
     * pixel smaller. This is the property centring actually promises. */
    for (int span = 1; span <= 400; span++) {
        for (int size = 0; size <= span; size++) {
            int x = center_axis(100, span, size);
            int before = x - 100, after = (100 + span) - (x + size);
            CHECK(before >= 0 && after >= 0 && after - before >= 0 &&
                  after - before <= 1,
                  "span %d size %d: %d before, %d after", span, size,
                  before, after);
        }
    }

    /* A box bigger than the span is pinned to the origin, not centred off the
     * left edge — a window wider than the monitor, or a monitor we have no
     * metrics for at all. */
    CHECK(center_axis(0, 1920, 2400) == 0, "oversized gave %d, expected 0",
          center_axis(0, 1920, 2400));
    CHECK(center_axis(50, 0, 0) == 50, "empty span gave %d, expected 50",
          center_axis(50, 0, 0));
    CHECK(center_axis(50, -10, 100) == 50, "negative span gave %d, expected 50",
          center_axis(50, -10, 100));

    /* ---- clamp_axis: putting a window back where it can be reached ----
     * The recovery counterpart of center_axis. A window already inside must not
     * be nudged at all — this runs on every display change, and a rescue that
     * moves windows nobody asked it to move is worse than the stranding it is
     * meant to fix. */
    CHECK(clamp_axis(0, 1920, 400, 800) == 400,
          "already inside moved to %d, expected 400",
          clamp_axis(0, 1920, 400, 800));
    CHECK(clamp_axis(0, 1920, 0, 1920) == 0,
          "exactly filling the span moved to %d, expected 0",
          clamp_axis(0, 1920, 0, 1920));
    CHECK(clamp_axis(0, 1920, 1120, 800) == 1120,
          "flush against the far edge moved to %d, expected 1120",
          clamp_axis(0, 1920, 1120, 800));

    /* Off each end: moved the least distance that gets it fully back in. */
    CHECK(clamp_axis(0, 1920, -500, 800) == 0,
          "off the near edge gave %d, expected 0",
          clamp_axis(0, 1920, -500, 800));
    CHECK(clamp_axis(0, 1920, 5920, 800) == 1120,
          "off the far edge gave %d, expected 1120",
          clamp_axis(0, 1920, 5920, 800));

    /* The case this exists for: a window stashed 4000px clear of the desktop,
     * or left on a monitor that has been unplugged. */
    CHECK(clamp_axis(0, 1920, 25920, 1280) == 640,
          "a stashed window came back to %d, expected 640",
          clamp_axis(0, 1920, 25920, 1280));

    /* A monitor that is not at the origin — the second display, and one to the
     * left of the primary, where the coordinates go negative. */
    CHECK(clamp_axis(1920, 1920, 100, 800) == 1920,
          "second monitor gave %d, expected 1920",
          clamp_axis(1920, 1920, 100, 800));
    CHECK(clamp_axis(-1080, 1080, -5000, 500) == -1080,
          "monitor left of the primary gave %d, expected -1080",
          clamp_axis(-1080, 1080, -5000, 500));

    /* Too big to fit is pinned to the origin, like center_axis — never centred
     * off the near edge, and never left hanging off the far one. */
    CHECK(clamp_axis(0, 1920, -500, 2400) == 0,
          "oversized gave %d, expected 0", clamp_axis(0, 1920, -500, 2400));
    CHECK(clamp_axis(50, 0, 900, 0) == 50, "empty span gave %d, expected 50",
          clamp_axis(50, 0, 900, 0));
    CHECK(clamp_axis(50, -10, 900, 100) == 50,
          "negative span gave %d, expected 50", clamp_axis(50, -10, 900, 100));

    /* The property clamping actually promises: wherever the box started, it
     * ends up fully inside the span, and it never moves when it need not. */
    for (int span = 1; span <= 200; span++) {
        for (int size = 0; size <= span; size++) {
            for (int pos = -250; pos <= 450; pos += 7) {
                int x = clamp_axis(100, span, pos, size);
                CHECK(x >= 100 && x + size <= 100 + span,
                      "span %d size %d pos %d escaped to %d", span, size,
                      pos, x);
                if (pos >= 100 && pos + size <= 100 + span)
                    CHECK(x == pos, "span %d size %d pos %d was already inside "
                          "but moved to %d", span, size, pos, x);
            }
        }
    }

    return tests_report("layout_math");
}
