/*
 * match.c — case-insensitive wildcard matching (see match.h).
 *
 * Iterative with a single backtrack point rather than recursive: the greedy `*`
 * remembers where it started swallowing, and a later mismatch resumes from
 * there having eaten one more character. That keeps a pathological pattern from
 * blowing the stack, which matters because the patterns come from a user config
 * and are matched against every window that appears on the system.
 */

#include "match.h"
#include <wctype.h>

static wchar_t fold_ch(wchar_t c) {
    return (c == L'/') ? L'\\' : (wchar_t)towlower(c);
}

bool wildcard_match(const wchar_t *pat, const wchar_t *str) {
    const wchar_t *star = NULL;   /* last '*' in the pattern, if any     */
    const wchar_t *back = NULL;   /* where that '*' resumes from in str  */

    while (*str) {
        if (*pat == L'*') {
            star = pat++;         /* match zero characters, for now */
            back = str;
        } else if (*pat == L'?' || (*pat && fold_ch(*pat) == fold_ch(*str))) {
            pat++; str++;
        } else if (star) {
            pat = star + 1;       /* backtrack: let the '*' eat one more */
            str = ++back;
        } else {
            return false;
        }
    }

    while (*pat == L'*') pat++;   /* trailing '*'s can match nothing */
    return *pat == L'\0';
}
