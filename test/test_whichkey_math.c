#include "tests.h"
#include "../src/whichkey_math.h"

static WkMetrics base(void) {
    WkMetrics m = {
        .count     = 8,
        .max_rows  = 12,
        .key_w     = 40,
        .label_w   = 100,
        .header_w  = 120,
        .row_h     = 24,
        .header_h  = 28,
        .pad       = 14,
        .key_gap   = 10,
        .col_gap   = 30,
        .min_label = 48,
        .max_w     = 1000,
        .max_h     = 800,
    };
    return m;
}

static void invariants(const WkMetrics *m, const WkLayout *l, const char *what) {
    int floor_w = m->pad * 2 + 1;
    CHECK(l->w <= m->max_w || m->max_w < floor_w,
          "%s: width %d exceeds max_w %d", what, l->w, m->max_w);
    CHECK(l->h <= m->max_h, "%s: height %d exceeds max_h %d",
          what, l->h, m->max_h);
    CHECK(l->shown <= m->count, "%s: showed %d of %d rows",
          what, l->shown, m->count);
    CHECK(l->shown <= l->cols * l->per_col,
          "%s: %d rows in a %dx%d grid", what, l->shown, l->cols, l->per_col);
    CHECK(l->cols >= 1 && l->per_col >= 1,
          "%s: degenerate grid %dx%d", what, l->cols, l->per_col);
    CHECK(l->label_w >= 1, "%s: label column collapsed to %d",
          what, l->label_w);
}

int main(void) {
    WkLayout l;

    {
        WkMetrics m = base();
        wk_layout(&m, &l);
        invariants(&m, &l, "fits");
        CHECK(l.cols == 1 && l.per_col == 8, "expected 1x8, got %dx%d",
              l.cols, l.per_col);
        CHECK(l.shown == 8, "showed %d of 8", l.shown);
        CHECK(l.label_w == 100, "label narrowed to %d with room to spare",
              l.label_w);
        CHECK(l.w == 40 + 10 + 100 + 28, "width %d", l.w);
        CHECK(l.h == 28 + 8 * 24 + 28, "height %d", l.h);
    }

    {
        WkMetrics m = base();
        m.count = 20;
        wk_layout(&m, &l);
        invariants(&m, &l, "wrap");
        CHECK(l.cols == 2 && l.per_col == 10,
              "20 rows over a 12-row cap gave %dx%d", l.cols, l.per_col);
        CHECK(l.shown == 20, "showed %d of 20", l.shown);
    }

    {
        WkMetrics m = base();
        m.count = 13;
        wk_layout(&m, &l);
        invariants(&m, &l, "even");
        CHECK(l.cols == 2 && l.per_col == 7, "13 rows gave %dx%d",
              l.cols, l.per_col);
    }

    {
        WkMetrics m = base();
        m.count = 20;
        m.max_h = 200;
        wk_layout(&m, &l);
        invariants(&m, &l, "max_h");
        CHECK(l.per_col == 5 && l.cols == 4,
              "a 6-row budget gave %dx%d", l.cols, l.per_col);
        CHECK(l.shown == 20, "showed %d of 20", l.shown);
    }

    {
        WkMetrics m = base();
        m.max_h = 10;
        wk_layout(&m, &l);
        invariants(&m, &l, "max_h tiny");
        CHECK(l.per_col == 1, "per_col %d under an impossible max_h", l.per_col);
    }

    {
        WkMetrics m = base();
        m.max_w = 150;
        wk_layout(&m, &l);
        invariants(&m, &l, "narrow");
        CHECK(l.cols == 1, "cols %d", l.cols);
        CHECK(l.label_w == 122 - 40 - 10, "label narrowed to %d", l.label_w);
        CHECK(l.shown == 8, "narrowing a label dropped rows: %d of 8", l.shown);
        CHECK(l.w == 150, "width %d should sit exactly on max_w", l.w);
    }

    {
        WkMetrics m = base();
        m.count = 20;
        m.max_w = 200;
        wk_layout(&m, &l);
        invariants(&m, &l, "drop column");
        CHECK(l.cols == 1, "expected the second column to go, got %d cols",
              l.cols);
        CHECK(l.per_col == 12, "per_col %d should return to the max_rows cap",
              l.per_col);
        CHECK(l.shown == 12, "showed %d of 20 — the caller must be told", l.shown);
    }

    {
        WkMetrics m = base();
        m.count    = 2;
        m.header_w = 900;
        wk_layout(&m, &l);
        invariants(&m, &l, "wide header");
        CHECK(l.w == 900 + 28, "header should set the width; got %d", l.w);

        m.max_w = 500;
        wk_layout(&m, &l);
        invariants(&m, &l, "wide header, capped");
        CHECK(l.w == 500, "max_w should cap the header; got %d", l.w);
    }

    {
        WkMetrics m = base();
        m.count = 0;
        wk_layout(&m, &l);
        CHECK(l.shown == 0, "showed %d rows of nothing", l.shown);
        CHECK(l.cols >= 1 && l.per_col >= 1, "grid %dx%d", l.cols, l.per_col);

        m = base();
        m.row_h = 0;
        wk_layout(&m, &l);
        CHECK(l.cols >= 1 && l.per_col >= 1, "zero row height gave %dx%d",
              l.cols, l.per_col);

        m = base();
        m.max_rows = 0;
        wk_layout(&m, &l);
        CHECK(l.per_col == 1, "max_rows 0 gave per_col %d", l.per_col);

        m = base();
        m.max_w = 1;
        m.max_h = 1;
        wk_layout(&m, &l);
        CHECK(l.label_w >= 1 && l.cols == 1,
              "an impossible box gave %d cols, label %d", l.cols, l.label_w);
    }

    {
        const int mx = 1920, my = 0, mw = 2560, mh = 1440;
        const int w = 400, h = 200, margin = 50;
        int x, y;

        wk_anchor(1, 2, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 1920 + (2560 - 400) / 2 && y == 1440 - 200 - 50,
              "bottom centre gave %d,%d", x, y);

        wk_anchor(1, 0, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 3000 && y == 50, "top centre gave %d,%d", x, y);

        wk_anchor(1, 1, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 3000 && y == (1440 - 200) / 2,
              "centre gave %d,%d — a centred axis ignores the margin", x, y);

        wk_anchor(0, 0, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 1970 && y == 50, "top left gave %d,%d", x, y);

        wk_anchor(2, 0, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 1920 + 2560 - 400 - 50 && y == 50,
              "top right gave %d,%d", x, y);

        wk_anchor(0, 2, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 1970 && y == 1190, "bottom left gave %d,%d", x, y);

        wk_anchor(2, 2, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 4030 && y == 1190, "bottom right gave %d,%d", x, y);

        wk_anchor(0, 1, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 1970 && y == 620, "left gave %d,%d", x, y);

        wk_anchor(2, 1, mx, my, mw, mh, w, h, margin, &x, &y);
        CHECK(x == 4030 && y == 620, "right gave %d,%d", x, y);

        wk_anchor(1, 1, mx, my, mw, mh, 3000, 2000, margin, &x, &y);
        CHECK(x == mx && y == my, "oversized panel placed at %d,%d", x, y);
    }

    return tests_report("whichkey_math");
}
