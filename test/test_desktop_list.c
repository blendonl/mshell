#include "tests.h"
#include "../src/desktop_list.h"

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
    CHECK(desktop_list_name_ok(L"web", 64),  "'web' is a usable name");
    CHECK(desktop_list_name_ok(L"1", 64),    "'1' is a usable name");
    CHECK(desktop_list_name_ok(L"game-2", 64), "'game-2' is a usable name");

    CHECK(!desktop_list_name_ok(NULL, 64),   "NULL is not a name");
    CHECK(!desktop_list_name_ok(L"", 64),    "the empty string is not a name");
    CHECK(!desktop_list_name_ok(L"my desktop", 64), "spaces are refused");
    CHECK(!desktop_list_name_ok(L"tab\there", 64),  "tabs are refused");
    CHECK(!desktop_list_name_ok(L" web", 64),       "a leading space is refused");
    CHECK(!desktop_list_name_ok(L"web ", 64),       "a trailing space is refused");

    CHECK(desktop_list_name_ok(L"abc", 4),  "3 chars fit in a cap of 4");
    CHECK(!desktop_list_name_ok(L"abcd", 4), "4 chars do not fit in a cap of 4");
    CHECK(!desktop_list_name_ok(L"a", 0),   "a cap of 0 admits nothing");

    CHECK(desktop_name_eq(L"web", L"web"),   "identical names are equal");
    CHECK(desktop_name_eq(L"Web", L"web"),   "case is ignored");
    CHECK(desktop_name_eq(L"WEB", L"wEb"),   "case is ignored both ways");
    CHECK(desktop_name_eq(L"", L""),         "two empty names are equal");

    CHECK(!desktop_name_eq(L"web", L"webb"), "a prefix is not the same desktop");
    CHECK(!desktop_name_eq(L"webb", L"web"), "...in either direction");
    CHECK(!desktop_name_eq(L"web", L""),     "a name is not the empty name");
    CHECK(!desktop_name_eq(NULL, L"web"),    "NULL matches nothing");
    CHECK(!desktop_name_eq(NULL, NULL),      "...not even NULL");

    BEFORE("1", "2");
    BEFORE("2", "10");
    BEFORE("10", "chat");
    BEFORE("chat", "web");
    BEFORE("9", "chat");

    BEFORE("2", "2b");
    BEFORE("100", "2b");
    BEFORE("1", "-1");
    BEFORE("-1", "chat");
    SAME_ORDER("007", "7");
    SAME_ORDER("web", "WEB");
    SAME_ORDER("chat", "chat");

    CHECK(desktop_name_cmp(L"web", L"web") == 0, "a name ties with itself");
    CHECK(desktop_name_cmp(L"1", L"1") == 0,     "a number ties with itself");

    CHECK(desktop_attach_index(ATTACH_END, 0, 3) == 3, "END appends");
    CHECK(desktop_attach_index(ATTACH_END, 2, 3) == 3, "END ignores focus");
    CHECK(desktop_attach_index(ATTACH_END, 0, 0) == 0, "END on an empty list");

    CHECK(desktop_attach_index(ATTACH_MASTER, 2, 3) == 0, "MASTER takes slot 0");
    CHECK(desktop_attach_index(ATTACH_MASTER, 0, 0) == 0, "MASTER on an empty list");

    CHECK(desktop_attach_index(ATTACH_AFTER, 0, 3) == 1, "AFTER the first");
    CHECK(desktop_attach_index(ATTACH_AFTER, 1, 3) == 2, "AFTER the middle");
    CHECK(desktop_attach_index(ATTACH_AFTER, 2, 3) == 3, "AFTER the last is the end");
    CHECK(desktop_attach_index(ATTACH_AFTER, -1, 3) == 3, "AFTER with no focus appends");
    CHECK(desktop_attach_index(ATTACH_AFTER, 9, 3) == 3,  "AFTER a stale index appends");
    CHECK(desktop_attach_index(ATTACH_AFTER, 0, 0) == 0,  "AFTER on an empty list");

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

    CHECK(desktop_focus_after_remove(2, 0, 3) == 1, "removing below pulls focus down");
    CHECK(desktop_focus_after_remove(3, 1, 3) == 2, "...from any index below");
    CHECK(desktop_focus_after_remove(1, 0, 3) == 0, "...including down to 0");

    CHECK(desktop_focus_after_remove(0, 1, 3) == 0, "removing above leaves focus alone");
    CHECK(desktop_focus_after_remove(1, 2, 3) == 1, "...wherever above it is");

    CHECK(desktop_focus_after_remove(1, 1, 3) == 1, "removing the focused window stays put");
    CHECK(desktop_focus_after_remove(0, 0, 3) == 0, "...at the head too");
    CHECK(desktop_focus_after_remove(3, 3, 3) == 2, "removing the last focused window steps back");

    CHECK(desktop_focus_after_remove(2, 0, 0) == 0, "an empty list answers 0");
    CHECK(desktop_focus_after_remove(0, 0, 0) == 0, "an empty list answers 0 from 0");
    CHECK(desktop_focus_after_remove(-1, 0, 3) == 0, "a negative focus answers 0");
    CHECK(desktop_focus_after_remove(1, -1, 3) == 1, "a negative removal shifts nothing");
    CHECK(desktop_focus_after_remove(1, 9, 3) == 1, "a removal past the end shifts nothing");

    for (int count = 0; count <= 5; count++) {
        for (int focused = -2; focused <= 6; focused++) {
            for (int removed = -1; removed <= 6; removed++) {
                int f = desktop_focus_after_remove(focused, removed, count);
                CHECK(f >= 0 && (count == 0 ? f == 0 : f < count),
                      "focus %d not indexable in %d (focused %d, removed %d)",
                      f, count, focused, removed);
                if (focused >= 0)
                    CHECK(f <= focused,
                          "a removal moved the focus UP (%d -> %d, removed %d, "
                          "count %d)", focused, f, removed, count);
                if (focused >= 0 && focused < count && removed > focused)
                    CHECK(f == focused,
                          "a removal above the focus moved it (%d -> %d, "
                          "removed %d, count %d)", focused, f, removed, count);
            }
        }
    }

    {
        int f = 3;
        f = desktop_focus_after_remove(f, 1, 3);
        CHECK(f == 2, "after B leaves, D is at 2");
        f = desktop_focus_after_remove(f, 0, 2);
        CHECK(f == 1, "after A leaves, D is at 1");
    }

    {
        int n;

        CHECK(desktop_hist_shift(0, -1, 4, &n) == 0, "empty history shifts from 0");
        CHECK(n == 1, "...and the history now holds 1");

        CHECK(desktop_hist_shift(2, -1, 4, &n) == 2, "a new entry shifts from n");
        CHECK(n == 3, "...and the history grows");

        CHECK(desktop_hist_shift(4, 2, 4, &n) == 2, "an existing entry shifts from itself");
        CHECK(n == 4, "...and the history does not grow");
        CHECK(desktop_hist_shift(3, 1, 4, &n) == 1, "an existing entry, mid-list");
        CHECK(n == 3, "...still does not grow");

        CHECK(desktop_hist_shift(4, -1, 4, &n) == 3, "a full history shifts from cap-1");
        CHECK(n == 4, "...and stays full rather than growing past cap");

        CHECK(desktop_hist_shift(1, -1, 1, &n) == 0, "cap 1: always slot 0");
        CHECK(n == 1, "cap 1: length stays 1");

        CHECK(desktop_hist_shift(0, -1, 0, &n) == 0, "cap 0 answers 0");
        CHECK(n == 0, "cap 0 keeps the length at 0");
        CHECK(desktop_hist_shift(-3, -1, 4, &n) == 0, "a negative length answers 0");
        CHECK(desktop_hist_shift(9, -1, 4, &n) == 3, "a length past cap is clamped");

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
