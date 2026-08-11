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

int clamp_axis(int origin, int span, int pos, int size) {
    /* Same answer as center_axis for a box that cannot fit, and for the same
     * reason — see the header. Checked first so the two below cannot disagree
     * about which edge wins. */
    if (size >= span) return origin;

    if (pos < origin)                return origin;
    if (pos + size > origin + span)  return origin + span - size;
    return pos;                      /* already inside; do not move it */
}

int center_axis(int origin, int span, int size) {
    /* A box at least as big as the span cannot be centred inside it — the best
     * answer is the span's own start, which is also what keeps a float that is
     * wider than the monitor anchored to the left edge instead of hanging off
     * both. Covers span <= 0 too, so no display metric can produce a stray
     * coordinate here. */
    if (size >= span) return origin;
    return origin + (span - size) / 2;
}
