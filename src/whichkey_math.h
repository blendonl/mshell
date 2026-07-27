#pragma once

/*
 * whichkey_math.h — the geometry of the which-key panel, with no Windows in it.
 *
 * Split out for the same reason as layout_math.c: it is pure, and it is where
 * an off-by-one turns into a column that doesn't fit, a binding that is never
 * drawn, or a panel hanging off the edge of a monitor. None of those are
 * visible by reading the code, and all of them `make test` can check on the
 * host — which matters more here than usual, because the panel only exists on
 * a screen nobody is looking at while the config that shapes it is written.
 *
 * Everything is DEVICE pixels: the caller has already scaled its design pixels
 * for the monitor's DPI and measured its text with the real font.
 */

/* What the panel is made of. */
typedef struct {
    int count;      /* rows to place                                        */
    int max_rows;   /* rows in a column before a new column starts          */
    int key_w;      /* widest key name                                      */
    int label_w;    /* widest label                                         */
    int header_w;   /* the header's natural width                           */
    int row_h;      /* one row, its vertical gap included                   */
    int header_h;   /* the header, its gap included                         */
    int pad;        /* panel inner padding                                  */
    int key_gap;    /* between a key and its label                          */
    int col_gap;    /* between columns                                      */
    int min_label;  /* narrowest label still worth ellipsizing into         */
    int max_w;      /* the box the panel must fit inside                    */
    int max_h;
} WkMetrics;

/* Where it all ended up. */
typedef struct {
    int cols;       /* columns actually used                               */
    int per_col;    /* rows per column                                     */
    int label_w;    /* label column, narrowed if it had to be              */
    int shown;      /* rows that fit; below `count` the rest were dropped  */
    int w, h;       /* panel size                                          */
} WkLayout;

/*
 * Shape the grid and size the panel.
 *
 * The order the constraints give way in is the point: height caps the rows per
 * column first, then an over-wide panel gives up the label column's slack
 * (an ellipsized label still says roughly what a key does), and only once a
 * label would be too narrow to read does a whole column go. Dropping a column
 * costs capacity, which is what `shown` reports.
 */
void wk_layout(const WkMetrics *m, WkLayout *out);

/*
 * Anchor a w×h panel inside a monitor at (mon_x, mon_y, mon_w, mon_h).
 * `halign` / `valign`: 0 = start (left/top), 1 = centre, 2 = end
 * (right/bottom). `margin` is the gap to the edge the panel is anchored to; a
 * centred axis ignores it. The result is never left of, or above, the
 * monitor's origin — an oversized panel is clipped at the far edge instead,
 * where the ellipsized text already is.
 */
void wk_anchor(int halign, int valign, int mon_x, int mon_y,
               int mon_w, int mon_h, int w, int h, int margin,
               int *out_x, int *out_y);
