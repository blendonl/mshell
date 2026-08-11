#pragma once

/*
 * desktop_list.h — the arithmetic behind a desktop's window list, with no
 * Windows in it.
 *
 * Split out of desktop.c for the same reason as match.c and layout_math.c: it
 * is pure, and it is where an off-by-one does its damage silently. A desktop's
 * windows[] is memmove'd on every insert and removal, its `focused` is an index
 * into an array that shifts under it, and its focus history is a fixed ring
 * that saturates rather than grows. None of that produces a crash when it is
 * wrong — it produces a keybinding that walks from the wrong window, a desktop
 * that will not go away, or a window nothing can reach. `make test` covers it
 * on the host.
 *
 * The HWNDs themselves stay in desktop.c. What lives here is every decision
 * ABOUT an index: where a new window goes, where `focused` lands after a
 * removal, how far the history shifts, and how two desktop names compare. That
 * is the cut layout_math.c already makes — it deals in ints, not in RECTs.
 */

#include <stdbool.h>
#include <wchar.h>

/* ---------------------------------------------------------------------------
 * Where new windows land in a desktop's order.
 *
 * Here rather than in mshell.h because desktop_attach_index() is the code that
 * acts on it, and a header that owns the enum it switches on can be tested
 * without dragging Windows in.
 *
 *   ATTACH_END    : append (becomes last stack window) — the historical default
 *   ATTACH_MASTER : insert at index 0 (becomes master, dwm-style)
 *   ATTACH_AFTER  : insert right after the focused window
 * --------------------------------------------------------------------------- */
typedef enum {
    ATTACH_END = 0,
    ATTACH_MASTER,
    ATTACH_AFTER,
} AttachPolicy;

/* ---------------------------------------------------------------------------
 * Names
 *
 * A desktop IS its name, so these three are what "the same desktop" means.
 * Case-insensitive throughout (`switch_desktop "Web"` and "web" are one
 * desktop), folded with towlower so the module stays host-portable — _wcsicmp
 * is not standard C, and match.c already sets this precedent.
 * --------------------------------------------------------------------------- */

/* Usable as a desktop name: non-empty, shorter than `cap` including the NUL,
 * and free of whitespace — which would make a name impossible to tell apart in
 * a log line and is never what the config meant.
 *
 * desktop.c wraps this as desktop_name_ok(), binding cap to DESKTOP_NAME_MAX;
 * that wrapper is what the rest of mshell calls. */
bool desktop_list_name_ok(const wchar_t *name, size_t cap);

/* The same desktop? Case-insensitive; two NULLs are not equal to anything. */
bool desktop_name_eq(const wchar_t *a, const wchar_t *b);

/* Sort order for the live desktop set: numeric names first, in NUMERIC order,
 * then the rest alphabetically — 1, 2, 10, chat, web. Read off a list, that is
 * the order you expect, and it is what makes next_desktop/prev_desktop STABLE:
 * with desktops appearing and vanishing under you, "the next one" has to mean
 * the same thing every time, and creation order does not survive a desktop
 * being destroyed and re-created.
 *
 * A name counts as numeric only if it is ALL digits and non-negative, so "10"
 * sorts after "2" but "2b" is a word. Returns <0, 0 or >0 like strcmp. */
int desktop_name_cmp(const wchar_t *a, const wchar_t *b);

/* ---------------------------------------------------------------------------
 * Indices
 * --------------------------------------------------------------------------- */

/* Where a new window is inserted into a list of `count`, under `policy`.
 * `focused` is the current focused index, ignored when it is out of range.
 * Always in [0, count], so it is safe to memmove from. */
int desktop_attach_index(AttachPolicy policy, int focused, int count);

/* Where `focused` should point after the list has shrunk to `count`. Only ever
 * pulls it back inside the array — an index that is still valid is left alone,
 * so removing a window AFTER the focused one does not move the focus. Answers
 * 0 for an empty list, matching the struct's zero-initialised state. */
int desktop_focus_clamp(int focused, int count);

/* The shift origin for pushing a window to the front of a focus history of
 * length `n` holding at most `cap` entries. `found` is the window's current
 * index in that history, or -1 when it is not in it yet.
 *
 * Entries [0, result) each move up one slot and the window takes slot 0, so a
 * window already in the list MOVES rather than being duplicated, and a full
 * list drops its oldest entry instead of growing. The new length is written to
 * *n_out.
 *
 * Callers handle "already at the front" themselves — that is a no-op, not a
 * shift of zero entries, and keeping it out of here keeps this total. */
int desktop_hist_shift(int n, int found, int cap, int *n_out);
