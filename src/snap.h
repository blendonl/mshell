#pragma once

typedef enum {
    SNAP_KEEP_TRYING,
    SNAP_GIVE_UP_LOUDLY,
    SNAP_GIVE_UP_QUIETLY,
} SnapVerdict;

SnapVerdict snap_backoff(ManagedWindow *mw);
void        snap_backoff_reset(ManagedWindow *mw);
