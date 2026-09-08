#pragma once

int  resolve_target(Desktop *dt, int from, Action action, bool cycle_prev);
void focus_monitor(int delta);
void move_focused_to_monitor(int delta);
bool parse_desktop_monitor(const wchar_t *command, int arg,
                           int *slot, int *mon,
                           wchar_t *unknown, size_t unknown_cap);
void float_nudge(ManagedWindow *mw, Action action, bool resize);
void adjust_cfact(HWND focus, float delta);
