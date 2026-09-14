#include "focus_pick.h"

#include <stdlib.h>

static bool reachable(const FocusCandidate *c) {
    return c->focusable && c->on_screen;
}

static bool lies_toward(long dx, long dy, FocusDirection dir) {
    switch (dir) {
    case FOCUS_LEFT:  return dx < 0 && labs(dx) >= labs(dy);
    case FOCUS_RIGHT: return dx > 0 && labs(dx) >= labs(dy);
    case FOCUS_UP:    return dy < 0 && labs(dy) >= labs(dx);
    case FOCUS_DOWN:  return dy > 0 && labs(dy) >= labs(dx);
    }
    return false;
}

int focus_pick_neighbor(const FocusCandidate *cands, int count, int from,
                        FocusDirection dir) {
    if (!cands || from < 0 || from >= count || !reachable(&cands[from]))
        return -1;

    const FocusCandidate *origin = &cands[from];
    int  best       = -1;
    long best_score = 0;
    for (int i = 0; i < count; i++) {
        if (i == from || !reachable(&cands[i])) continue;

        long dx = cands[i].x - origin->x;
        long dy = cands[i].y - origin->y;
        if (!lies_toward(dx, dy, dir)) continue;

        long score = dx * dx + dy * dy;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

int focus_pick_cycle(const FocusCandidate *cands, int count, int from,
                     bool prev) {
    if (!cands || count <= 0) return -1;

    int step = prev ? -1 : 1;
    for (int n = 1; n <= count; n++) {
        int i = ((from + step * n) % count + count) % count;
        if (i == from) continue;
        if (cands[i].focusable) return i;
    }
    return -1;
}
