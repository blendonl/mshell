/*
 * layout_math.c — proportional division for the tiling layouts (see the header).
 */

#include "layout_math.h"

void split_span(int span, const float *facts, int n, int *out) {
    if (n <= 0) return;

    float total = 0.f;
    for (int i = 0; i < n; i++) {
        float c = facts[i];
        total += (c <= 0.f) ? 1.f : c;
    }
    if (total <= 0.f) total = (float)n;   /* can't happen; keeps the divide safe */

    int used = 0;
    for (int i = 0; i < n; i++) {
        if (i == n - 1) {
            /* The last cell takes whatever is left, so the sizes sum to exactly
             * `span` however the divisions above rounded. */
            out[i] = span - used;
        } else {
            float c = facts[i];
            if (c <= 0.f) c = 1.f;
            out[i] = (int)((float)span * (c / total));
            used  += out[i];
        }
    }
}
