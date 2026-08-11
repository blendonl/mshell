/*
 * test_desktop_list.c — the index and name arithmetic behind virtual desktops.
 *
 * Every function here is one a desktop's window list leans on while the list is
 * being memmove'd under it. None of them can crash: get the attach index wrong
 * and a window opens in the wrong slot, get the focus clamp wrong and Win+h
 * walks from a window that is no longer there, get the history shift wrong and
 * "the window I was just in" is either duplicated or lost. All silent, all
 * noticed days later, all trivially checkable here.
 */

#include "tests.h"
#include "../src/desktop_list.h"

/* Sort helpers: assert a strict ordering, and that it is antisymmetric — a
 * comparator that answers "less" in both directions makes the insertion sort in
 * desktop_sort_in() depend on insertion order, which is the whole thing it
 * exists to prevent. */
#define BEFORE(a, b)                                                          \
    do {                                                                      \
        CHECK(desktop_name_cmp(L##a, L##b) < 0, "'%ls' should sort before '%ls'",  \
              L##a, L##b);                                                    \
        CHECK(desktop_name_cmp(L##b, L##a) > 0, "'%ls' should sort after '%ls'",   \
              L##b, L##a);                                                    \
    } while (0)

#define SAME_ORDER(a, b)                                                      \
    CHECK(desktop_name_cmp(L##a, L##b) == 0, "'%ls' and '%ls' should tie",    \
          L##a, L##b)

int main(void) {
    /* ==================================================================
     * desktop_list_name_ok
     * ================================================================== */
    CHECK(desktop_list_name_ok(L"web", 64),  "'web' is a usable name");
    CHECK(desktop_list_name_ok(L"1", 64),    "'1' is a usable name");
    CHECK(desktop_list_name_ok(L"game-2", 64), "'game-2' is a usable name");

    CHECK(!desktop_list_name_ok(NULL, 64),   "NULL is not a name");
    CHECK(!desktop_list_name_ok(L"", 64),    "the empty string is not a name");
    /* Whitespace: a name you cannot pick out of a log line. */
    CHECK(!desktop_list_name_ok(L"my desktop", 64), "spaces are refused");
    CHECK(!desktop_list_name_ok(L"tab\there", 64),  "tabs are refused");
    CHECK(!desktop_list_name_ok(L" web", 64),       "a leading space is refused");
    CHECK(!desktop_list_name_ok(L"web ", 64),       "a trailing space is refused");

    /* `cap` counts the NUL, so a name of exactly cap-1 characters fits and one
     * of cap does not. Off by one here and a name that fits is rejected, or one
     * that does not is copied into a fixed buffer. */
    CHECK(desktop_list_name_ok(L"abc", 4),  "3 chars fit in a cap of 4");
    CHECK(!desktop_list_name_ok(L"abcd", 4), "4 chars do not fit in a cap of 4");
    CHECK(!desktop_list_name_ok(L"a", 0),   "a cap of 0 admits nothing");

    /* ==================================================================
     * desktop_name_eq — "the same desktop"
     * ================================================================== */
    CHECK(desktop_name_eq(L"web", L"web"),   "identical names are equal");
    CHECK(desktop_name_eq(L"Web", L"web"),   "case is ignored");
    CHECK(desktop_name_eq(L"WEB", L"wEb"),   "case is ignored both ways");
    CHECK(desktop_name_eq(L"", L""),         "two empty names are equal");

    CHECK(!desktop_name_eq(L"web", L"webb"), "a prefix is not the same desktop");
    CHECK(!desktop_name_eq(L"webb", L"web"), "...in either direction");
    CHECK(!desktop_name_eq(L"web", L""),     "a name is not the empty name");
    CHECK(!desktop_name_eq(NULL, L"web"),    "NULL matches nothing");
    CHECK(!desktop_name_eq(NULL, NULL),      "...not even NULL");

    /* ==================================================================
     * desktop_name_cmp — the order the bar shows and next/prev walks
     *
     * The documented answer is 1, 2, 10, chat, web.
     * ================================================================== */
    BEFORE("1", "2");
    BEFORE("2", "10");            /* numeric, not lexicographic: "10" > "2" */
    BEFORE("10", "chat");         /* every number before every word */
    BEFORE("chat", "web");
    BEFORE("9", "chat");

    /* Digits-only is what makes a name a number. A partial parse is a word. */
    BEFORE("2", "2b");            /* "2b" is a word, so the number leads */
    BEFORE("100", "2b");
    /* A leading '-' makes it a word, not a negative number — wcstol would parse
     * one, and the `v < 0` guard is what rejects it. So this sorts with the
     * words, AFTER every number, even though '-' is below every digit in code
     * point order. */
    BEFORE("1", "-1");
    BEFORE("-1", "chat");
    SAME_ORDER("007", "7");       /* same number, however it is spelled */
    SAME_ORDER("web", "WEB");     /* words tie case-insensitively */
    SAME_ORDER("chat", "chat");

    /* A comparator that is not reflexive breaks the insertion sort outright. */
    CHECK(desktop_name_cmp(L"web", L"web") == 0, "a name ties with itself");
    CHECK(desktop_name_cmp(L"1", L"1") == 0,     "a number ties with itself");

    /* ==================================================================
     * desktop_attach_index — where a new window lands
     * ================================================================== */
    /* ATTACH_END: append. */
    CHECK(desktop_attach_index(ATTACH_END, 0, 3) == 3, "END appends");
    CHECK(desktop_attach_index(ATTACH_END, 2, 3) == 3, "END ignores focus");
    CHECK(desktop_attach_index(ATTACH_END, 0, 0) == 0, "END on an empty list");

    /* ATTACH_MASTER: always slot 0, dwm-style. */
    CHECK(desktop_attach_index(ATTACH_MASTER, 2, 3) == 0, "MASTER takes slot 0");
    CHECK(desktop_attach_index(ATTACH_MASTER, 0, 0) == 0, "MASTER on an empty list");

    /* ATTACH_AFTER: right after the focused window. */
    CHECK(desktop_attach_index(ATTACH_AFTER, 0, 3) == 1, "AFTER the first");
    CHECK(desktop_attach_index(ATTACH_AFTER, 1, 3) == 2, "AFTER the middle");
    /* Focused is the LAST window: "after" it is the end, which must still be a
     * valid insertion point rather than one past the array. */
    CHECK(desktop_attach_index(ATTACH_AFTER, 2, 3) == 3, "AFTER the last is the end");
    /* No focused window, or a stale index: fall back to appending, not to 1. */
    CHECK(desktop_attach_index(ATTACH_AFTER, -1, 3) == 3, "AFTER with no focus appends");
    CHECK(desktop_attach_index(ATTACH_AFTER, 9, 3) == 3,  "AFTER a stale index appends");
    CHECK(desktop_attach_index(ATTACH_AFTER, 0, 0) == 0,  "AFTER on an empty list");

    /* Whatever the policy, the answer is always somewhere you can memmove to. */
    for (int policy = 0; policy <= ATTACH_AFTER; policy++) {
        for (int count = 0; count <= 4; count++) {
            for (int focused = -1; focused <= 5; focused++) {
                int idx = desktop_attach_index((AttachPolicy)policy, focused, count);
                CHECK(idx >= 0 && idx <= count,
                      "attach index %d out of [0,%d] (policy %d, focused %d)",
                      idx, count, policy, focused);
            }
        }
    }

    /* ==================================================================
     * desktop_focus_clamp — where `focused` lands after a removal
     * ================================================================== */
    /* Still valid: left exactly alone. Removing a window AFTER the focused one
     * must not move the focus. */
    CHECK(desktop_focus_clamp(0, 3) == 0, "0 of 3 is untouched");
    CHECK(desktop_focus_clamp(1, 3) == 1, "1 of 3 is untouched");
    CHECK(desktop_focus_clamp(2, 3) == 2, "the last index is untouched");

    /* Past the end: pulled back to the last window. */
    CHECK(desktop_focus_clamp(3, 3) == 2, "one past the end clamps to the last");
    CHECK(desktop_focus_clamp(9, 3) == 2, "far past the end clamps to the last");

    /* Degenerate inputs still answer something indexable. */
    CHECK(desktop_focus_clamp(2, 0) == 0, "an empty list answers 0");
    CHECK(desktop_focus_clamp(0, 0) == 0, "an empty list answers 0 from 0");
    CHECK(desktop_focus_clamp(-1, 3) == 0, "a negative index answers 0");

    for (int count = 0; count <= 4; count++) {
        for (int focused = -2; focused <= 6; focused++) {
            int f = desktop_focus_clamp(focused, count);
            CHECK(f >= 0 && (count == 0 ? f == 0 : f < count),
                  "clamped focus %d not indexable in %d", f, count);
        }
    }

    /* ==================================================================
     * desktop_hist_shift — the focus-history ring
     *
     * `from` is the slot the shift starts at: entries [0, from) each move up
     * one and the window takes slot 0.
     * ================================================================== */
    {
        int n;

        /* A brand new window on an empty history. */
        CHECK(desktop_hist_shift(0, -1, 4, &n) == 0, "empty history shifts from 0");
        CHECK(n == 1, "...and the history now holds 1");

        /* A new window with room to spare: shift the whole list down one. */
        CHECK(desktop_hist_shift(2, -1, 4, &n) == 2, "a new entry shifts from n");
        CHECK(n == 3, "...and the history grows");

        /* A window already in the list MOVES rather than being duplicated, so
         * the shift starts where it currently sits and the length is unchanged.
         * Getting this wrong is how the same window appears twice and
         * last_window starts answering with the window you are already in. */
        CHECK(desktop_hist_shift(4, 2, 4, &n) == 2, "an existing entry shifts from itself");
        CHECK(n == 4, "...and the history does not grow");
        CHECK(desktop_hist_shift(3, 1, 4, &n) == 1, "an existing entry, mid-list");
        CHECK(n == 3, "...still does not grow");

        /* The saturating case, which is the line this module exists for: a full
         * history has no free slot, so the shift starts at the LAST index and
         * the oldest entry falls off the end. Starting at `cap` instead would
         * write one past the array. */
        CHECK(desktop_hist_shift(4, -1, 4, &n) == 3, "a full history shifts from cap-1");
        CHECK(n == 4, "...and stays full rather than growing past cap");

        /* A cap of 1 is the degenerate version of the same thing. */
        CHECK(desktop_hist_shift(1, -1, 1, &n) == 0, "cap 1: always slot 0");
        CHECK(n == 1, "cap 1: length stays 1");

        /* Defensive: nonsense in, something indexable out. */
        CHECK(desktop_hist_shift(0, -1, 0, &n) == 0, "cap 0 answers 0");
        CHECK(n == 0, "cap 0 keeps the length at 0");
        CHECK(desktop_hist_shift(-3, -1, 4, &n) == 0, "a negative length answers 0");
        CHECK(desktop_hist_shift(9, -1, 4, &n) == 3, "a length past cap is clamped");

        /* Whatever comes in, the result must be a slot the shift loop can write
         * and the new length must stay inside the ring. */
        for (int cap = 1; cap <= 4; cap++) {
            for (int len = 0; len <= cap; len++) {
                for (int found = -1; found < len; found++) {
                    int from = desktop_hist_shift(len, found, cap, &n);
                    CHECK(from >= 0 && from < cap,
                          "shift origin %d out of [0,%d) (len %d, found %d)",
                          from, cap, len, found);
                    CHECK(n >= 0 && n <= cap,
                          "history length %d out of [0,%d]", n, cap);
                    CHECK(n >= len, "the history must never shrink on a push");
                }
            }
        }
    }

    return tests_report("desktop_list");
}
