/*
 * desktop_list.c — see desktop_list.h.
 *
 * Deliberately depends on nothing but the C library, so `make test` can compile
 * and run it natively while the rest of mshell cross-compiles to Windows.
 */

#include "desktop_list.h"

#include <wctype.h>

/* ===========================================================================
 * Names
 * =========================================================================== */
bool desktop_list_name_ok(const wchar_t *name, size_t cap) {
    if (!name || !name[0]) return false;
    if (cap == 0 || wcslen(name) >= cap) return false;
    for (const wchar_t *p = name; *p; p++)
        if (iswspace(*p)) return false;
    return true;
}

bool desktop_name_eq(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return false;
    for (; *a && *b; a++, b++)
        if (towlower(*a) != towlower(*b)) return false;
    return *a == *b;
}

/* A name made only of digits sorts as the number it spells. wcstol rather than
 * a hand-rolled loop so "007" and "7" agree, and `end` is what rejects "2b":
 * a partial parse is a word, not a number. */
static bool name_is_number(const wchar_t *name, long *out) {
    wchar_t *end = NULL;
    long     v   = wcstol(name, &end, 10);
    if (!end || end == name || *end != L'\0' || v < 0) return false;
    if (out) *out = v;
    return true;
}

int desktop_name_cmp(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);

    long na, nb;
    bool ia = name_is_number(a, &na);
    bool ib = name_is_number(b, &nb);

    if (ia && ib) return (na < nb) ? -1 : (na > nb) ? 1 : 0;
    if (ia != ib) return ia ? -1 : 1;          /* numbers before words */

    for (; *a && *b; a++, b++) {
        wint_t ca = towlower(*a), cb = towlower(*b);
        if (ca != cb) return (ca < cb) ? -1 : 1;
    }
    return (*a == *b) ? 0 : (*a ? 1 : -1);
}

/* ===========================================================================
 * Indices
 * =========================================================================== */
int desktop_attach_index(AttachPolicy policy, int focused, int count) {
    if (count < 0) count = 0;

    switch (policy) {
    case ATTACH_MASTER:
        return 0;
    case ATTACH_AFTER:
        /* Out-of-range `focused` falls back to appending rather than to slot 1:
         * "after the focused window" has no answer when there isn't one, and
         * the end is where a window with no relationship to the order belongs. */
        return (focused >= 0 && focused < count) ? focused + 1 : count;
    case ATTACH_END:
    default:
        return count;
    }
}

int desktop_focus_clamp(int focused, int count) {
    if (count <= 0)     return 0;
    if (focused < 0)    return 0;
    if (focused < count) return focused;   /* still valid — leave it alone */
    return count - 1;
}

int desktop_hist_shift(int n, int found, int cap, int *n_out) {
    if (cap <= 0) { if (n_out) *n_out = 0; return 0; }
    if (n < 0)   n = 0;
    if (n > cap) n = cap;

    /* Not in the list yet: the shift starts past the end, which is what makes
     * room for a new entry. Already in it: start at where it currently sits, so
     * everything above it moves down one and it is MOVED, not duplicated. */
    int from = (found >= 0 && found < n) ? found : n;

    /* A full list has no free slot to shift into, so the oldest entry falls off
     * the end instead. This is the line the whole module exists for. */
    if (from >= cap) from = cap - 1;

    if (n_out) *n_out = (found >= 0 && found < n) ? n
                      : (n < cap)                 ? n + 1
                                                  : n;
    return from;
}
