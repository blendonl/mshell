#include "desktop_place.h"

int desktop_visible_on(const int *monitor_desktop, int span, int id) {
    if (!monitor_desktop || id <= 0) return -1;
    for (int m = 0; m < span; m++)
        if (monitor_desktop[m] == id) return m;
    return -1;
}

int desktop_empty_monitor(const int *monitor_desktop, int span) {
    if (!monitor_desktop) return -1;
    for (int m = 0; m < span; m++)
        if (monitor_desktop[m] <= 0) return m;
    return -1;
}

DesktopSwitchPlan desktop_switch_plan(const int *monitor_desktop, int span,
                                      int target_id, int preferred) {
    DesktopSwitchPlan p = {DESKTOP_SWITCH_REPLACE, 0, 0, -1, false};

    int shown = desktop_visible_on(monitor_desktop, span, target_id);
    int mon   = (shown >= 0) ? shown : preferred;
    if (mon < 0 || mon >= span) mon = 0;

    p.mon     = mon;
    p.prev_id = (monitor_desktop && span > 0) ? monitor_desktop[mon] : 0;
    p.other   = shown;

    if (p.prev_id == target_id) {
        p.kind = DESKTOP_SWITCH_ALREADY_HERE;
        return p;
    }

    if (shown >= 0) {
        p.kind       = DESKTOP_SWITCH_SWAP;
        p.needs_fill = (p.prev_id <= 0);
        return p;
    }

    p.kind  = DESKTOP_SWITCH_REPLACE;
    p.other = -1;
    return p;
}

void desktop_switch_apply(int *monitor_desktop, int span,
                          const DesktopSwitchPlan *plan, int target_id) {
    if (!monitor_desktop || !plan) return;
    if (plan->mon < 0 || plan->mon >= span) return;

    switch (plan->kind) {
    case DESKTOP_SWITCH_ALREADY_HERE:
        return;

    case DESKTOP_SWITCH_SWAP:
        if (plan->other < 0 || plan->other >= span) return;
        monitor_desktop[plan->other] = plan->prev_id;
        monitor_desktop[plan->mon]   = target_id;
        return;

    case DESKTOP_SWITCH_REPLACE:
        monitor_desktop[plan->mon] = target_id;
        return;
    }
}
