#pragma once

typedef enum {
    SINK_SLOT_OTHER = 0,
    SINK_SLOT_SUNK,
    SINK_SLOT_SHOWN,
    SINK_SLOT_BACKDROP,
} SinkSlot;

typedef struct {
    int backdrop_below;
    int surfaced_from;
} SinkOrderPlan;

SinkOrderPlan sink_order_plan(const SinkSlot *bottom_up, int count);
