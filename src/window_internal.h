#pragma once

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

void window_apply_flat(HWND hwnd);
void window_restore_flat(HWND hwnd);
void window_claim_saved_frame(ManagedWindow *mw);

bool rect_off_screen(RECT r);

bool rect_clamp_into_monitor(RECT *r, int mon);
void window_float_keep_reachable(ManagedWindow *mw);
void window_park_float_if_fullscreen(ManagedWindow *mw);
void window_place_float(ManagedWindow *mw);
void fs_forget_prev(ManagedWindow *mw);

bool window_set_band(HWND hwnd, HWND after, bool topmost);
bool window_sink_intact(void);
