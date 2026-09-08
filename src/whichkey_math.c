#include "whichkey_math.h"

void wk_layout(const WkMetrics *m, WkLayout *out) {
    int count = m->count > 0 ? m->count : 0;
    int row_h = m->row_h > 0 ? m->row_h : 1;

    int per_cap  = m->max_rows > 0 ? m->max_rows : 1;
    int rows_fit = (m->max_h - m->header_h - m->pad * 2) / row_h;
    if (rows_fit < 1)     rows_fit = 1;
    if (per_cap > rows_fit) per_cap = rows_fit;

    int cols = (count + per_cap - 1) / per_cap;
    if (cols < 1) cols = 1;

    int inner_max = m->max_w - m->pad * 2;
    if (inner_max < 1) inner_max = 1;
    while (cols > 1) {
        int cell = (inner_max - (cols - 1) * m->col_gap) / cols;
        if (cell - m->key_w - m->key_gap >= m->min_label) break;
        cols--;
    }

    int per_col = (count + cols - 1) / cols;
    if (per_col > per_cap) per_col = per_cap;
    if (per_col < 1)       per_col = 1;

    int label_w  = m->label_w;
    int fit_cell = (inner_max - (cols - 1) * m->col_gap) / cols;
    if (m->key_w + m->key_gap + label_w > fit_cell) {
        label_w = fit_cell - m->key_w - m->key_gap;
        if (label_w < 1) label_w = 1;
    }

    int cell_w  = m->key_w + m->key_gap + label_w;
    int inner_w = cols * cell_w + (cols - 1) * m->col_gap;
    if (m->header_w > inner_w) inner_w = m->header_w;
    if (inner_w > inner_max)   inner_w = inner_max;

    int shown = cols * per_col;
    if (shown > count) shown = count;

    out->cols    = cols;
    out->per_col = per_col;
    out->label_w = label_w;
    out->shown   = shown;
    out->w       = inner_w + m->pad * 2;
    out->h       = m->header_h + per_col * row_h + m->pad * 2;
    if (out->h > m->max_h) out->h = m->max_h;
}

void wk_anchor(int halign, int valign, int mon_x, int mon_y,
               int mon_w, int mon_h, int w, int h, int margin,
               int *out_x, int *out_y) {
    int x, y;

    if      (halign == 0) x = mon_x + margin;
    else if (halign == 2) x = mon_x + mon_w - w - margin;
    else                  x = mon_x + (mon_w - w) / 2;

    if      (valign == 0) y = mon_y + margin;
    else if (valign == 2) y = mon_y + mon_h - h - margin;
    else                  y = mon_y + (mon_h - h) / 2;

    if (x < mon_x) x = mon_x;
    if (y < mon_y) y = mon_y;

    *out_x = x;
    *out_y = y;
}
