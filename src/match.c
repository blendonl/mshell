#include "match.h"
#include <wctype.h>

static wchar_t fold_ch(wchar_t c) {
    return (c == L'/') ? L'\\' : (wchar_t)towlower(c);
}

bool wildcard_match(const wchar_t *pat, const wchar_t *str) {
    const wchar_t *star = NULL;
    const wchar_t *back = NULL;

    while (*str) {
        if (*pat == L'*') {
            star = pat++;
            back = str;
        } else if (*pat == L'?' || (*pat && fold_ch(*pat) == fold_ch(*str))) {
            pat++; str++;
        } else if (star) {
            pat = star + 1;
            str = ++back;
        } else {
            return false;
        }
    }

    while (*pat == L'*') pat++;
    return *pat == L'\0';
}
