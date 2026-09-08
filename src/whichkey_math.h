#pragma once

typedef struct {
    int count;
    int max_rows;
    int key_w;
    int label_w;
    int header_w;
    int row_h;
    int header_h;
    int pad;
    int key_gap;
    int col_gap;
    int min_label;
    int max_w;
    int max_h;
} WkMetrics;

typedef struct {
    int cols;
    int per_col;
    int label_w;
    int shown;
    int w, h;
} WkLayout;

void wk_layout(const WkMetrics *m, WkLayout *out);

void wk_anchor(int halign, int valign, int mon_x, int mon_y,
               int mon_w, int mon_h, int w, int h, int margin,
               int *out_x, int *out_y);
