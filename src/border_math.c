#include "border_math.h"

static int rect_width(BorderRect r)  { return r.right - r.left; }
static int rect_height(BorderRect r) { return r.bottom - r.top; }

static BorderRect rect_union(BorderRect a, BorderRect b) {
    BorderRect u;
    u.left   = a.left   < b.left   ? a.left   : b.left;
    u.top    = a.top    < b.top    ? a.top    : b.top;
    u.right  = a.right  > b.right  ? a.right  : b.right;
    u.bottom = a.bottom > b.bottom ? a.bottom : b.bottom;
    return u;
}

BorderGeometry border_geometry(BorderRect frame, BorderRect work_area,
                               int width, BorderAccentEdge edge, int accent) {
    BorderGeometry out;
    out.visible    = false;
    out.has_ring   = false;
    out.has_accent = false;
    out.bounds = out.hole = out.accent = (BorderRect){0, 0, 0, 0};

    if (rect_width(frame) <= 0 || rect_height(frame) <= 0) return out;

    if (width  < 0) width  = 0;
    if (accent < 0) accent = 0;
    if (edge != BORDER_ACCENT_TOP && edge != BORDER_ACCENT_BOTTOM) accent = 0;
    if (accent > rect_height(frame)) accent = rect_height(frame);
    if (width == 0 && accent == 0) return out;

    BorderRect limit = frame;
    if (rect_width(work_area) > 0 && rect_height(work_area) > 0)
        limit = rect_union(work_area, frame);

    BorderRect bounds = {
        frame.left  - width, frame.top    - width,
        frame.right + width, frame.bottom + width,
    };
    if (bounds.left   < limit.left)   bounds.left   = limit.left;
    if (bounds.top    < limit.top)    bounds.top    = limit.top;
    if (bounds.right  > limit.right)  bounds.right  = limit.right;
    if (bounds.bottom > limit.bottom) bounds.bottom = limit.bottom;

    int w = rect_width(bounds);
    int h = rect_height(bounds);
    if (w <= 0 || h <= 0) return out;

    out.bounds = bounds;

    if (width > 0 && w > width * 2 && h > width * 2) {
        out.has_ring = true;
        out.hole = (BorderRect){ width, width, w - width, h - width };
    }

    if (accent > 0) {
        BorderRect bar;
        bar.left  = frame.left  - bounds.left;
        bar.right = frame.right - bounds.left;
        if (bar.left  < 0) bar.left  = 0;
        if (bar.right > w) bar.right = w;

        if (edge == BORDER_ACCENT_TOP) {
            bar.top    = frame.top - bounds.top;
            bar.bottom = bar.top + accent;
        } else {
            bar.bottom = frame.bottom - bounds.top;
            bar.top    = bar.bottom - accent;
        }
        if (bar.top    < 0) bar.top    = 0;
        if (bar.bottom > h) bar.bottom = h;

        if (rect_width(bar) > 0 && rect_height(bar) > 0) {
            out.has_accent = true;
            out.accent = bar;
        }
    }

    out.visible = out.has_ring || out.has_accent;
    return out;
}
