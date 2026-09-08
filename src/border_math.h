#pragma once

#include <stdbool.h>

typedef enum {
    BORDER_ACCENT_NONE = 0,
    BORDER_ACCENT_TOP,
    BORDER_ACCENT_BOTTOM,
} BorderAccentEdge;

typedef struct {
    int left, top, right, bottom;
} BorderRect;

typedef struct {
    bool       visible;
    BorderRect bounds;
    bool       has_ring;
    BorderRect hole;
    bool       has_accent;
    BorderRect accent;
} BorderGeometry;

BorderGeometry border_geometry(BorderRect frame, BorderRect work_area,
                               int width, BorderAccentEdge edge, int accent);
