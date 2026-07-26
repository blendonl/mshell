#pragma once

/*
 * layout_math.h — the arithmetic behind tiling, with no Windows in it.
 *
 * Split out of tiling.c for the same reason as match.c: it is pure, it is where
 * an off-by-one silently produces a one-pixel seam or a one-pixel overlap
 * between adjacent windows, and neither of those is something you notice by
 * looking. `make test` covers it on the host.
 */

/*
 * Divide `span` pixels among `n` items weighted by `facts`, writing each item's
 * size to `out`.
 *
 * The guarantee that matters: the sizes sum to EXACTLY `span`. Proportional
 * division rounds, and rounding n cells independently leaves a few pixels
 * unaccounted for — which is a visible seam at the edge of the last window, or
 * a visible overlap. The last item therefore absorbs the whole remainder rather
 * than each item absorbing a share of it.
 *
 * A non-positive factor is treated as 1.0, matching what a window with an unset
 * or nonsensical cfact should do. `n <= 0` writes nothing.
 */
void split_span(int span, const float *facts, int n, int *out);
