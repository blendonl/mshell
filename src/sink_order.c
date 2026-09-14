#include "sink_order.h"

#include <stdbool.h>

SinkOrderPlan sink_order_plan(const SinkSlot *bottom_up, int count) {
    SinkOrderPlan plan = { -1, count > 0 ? count : 0 };
    if (!bottom_up || count <= 0) return plan;

    int lowest_shown = count;
    for (int i = 0; i < count; i++)
        if (bottom_up[i] == SINK_SLOT_SHOWN) { lowest_shown = i; break; }

    int top_sunk = -1, backdrop = -1;
    for (int i = 0; i < count; i++) {
        if (bottom_up[i] == SINK_SLOT_BACKDROP) backdrop = i;
        if (bottom_up[i] == SINK_SLOT_SUNK && i < lowest_shown) top_sunk = i;
    }

    plan.surfaced_from = lowest_shown;

    bool placed = backdrop > top_sunk && backdrop < lowest_shown;
    if (placed) return plan;

    if (top_sunk >= 0)              plan.backdrop_below = top_sunk + 1;
    else if (lowest_shown < count)  plan.backdrop_below = lowest_shown;
    return plan;
}
