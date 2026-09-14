#pragma once

#include <stdbool.h>

typedef enum {
    FOCUS_LEFT = 0,
    FOCUS_RIGHT,
    FOCUS_UP,
    FOCUS_DOWN,
} FocusDirection;

typedef struct {
    long x, y;
    bool focusable;
    bool on_screen;
} FocusCandidate;

int focus_pick_neighbor(const FocusCandidate *cands, int count, int from,
                        FocusDirection dir);

int focus_pick_cycle(const FocusCandidate *cands, int count, int from,
                     bool prev);
