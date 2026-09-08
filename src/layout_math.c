#include "layout_math.h"

void split_span(int span, const float *facts, int n, int *out) {
    if (n <= 0) return;

    float total = 0.f;
    for (int i = 0; i < n; i++) {
        float c = facts[i];
        total += (c <= 0.f) ? 1.f : c;
    }
    if (total <= 0.f) total = (float)n;

    int used = 0;
    for (int i = 0; i < n; i++) {
        if (i == n - 1) {
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
    if (size >= span) return origin;

    if (pos < origin)                return origin;
    if (pos + size > origin + span)  return origin + span - size;
    return pos;
}

int center_axis(int origin, int span, int size) {
    if (size >= span) return origin;
    return origin + (span - size) / 2;
}
