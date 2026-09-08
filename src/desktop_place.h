#pragma once

#include <stdbool.h>

typedef enum {
    DESKTOP_SWITCH_ALREADY_HERE = 0,
    DESKTOP_SWITCH_SWAP,
    DESKTOP_SWITCH_REPLACE,
} DesktopSwitchKind;

typedef struct {
    DesktopSwitchKind kind;
    int               mon;
    int               prev_id;
    int               other;
    bool              needs_fill;
} DesktopSwitchPlan;

int desktop_visible_on(const int *monitor_desktop, int span, int id);

DesktopSwitchPlan desktop_switch_plan(const int *monitor_desktop, int span,
                                      int target_id, int preferred);

void desktop_switch_apply(int *monitor_desktop, int span,
                          const DesktopSwitchPlan *plan, int target_id);

int desktop_empty_monitor(const int *monitor_desktop, int span);
