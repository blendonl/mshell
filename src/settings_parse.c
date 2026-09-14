#include "settings_parse.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static const wchar_t *const TYPE_NAMES[] = {
    [SETTING_CUSTOM]         = L"Custom",
    [SETTING_DISPLAY_STRING] = L"DisplayString",
    [SETTING_LABELED_STRING] = L"LabeledString",
    [SETTING_BOOLEAN]        = L"Boolean",
    [SETTING_RANGE]          = L"Range",
    [SETTING_STRING]         = L"String",
    [SETTING_LIST]           = L"List",
    [SETTING_ACTION]         = L"Action",
    [SETTING_COLLECTION]     = L"SettingCollection",
};

static bool equal_fold(const wchar_t *a, const wchar_t *b) {
    for (; *a && *b; a++, b++)
        if (towlower(*a) != towlower(*b)) return false;
    return *a == *b;
}

const wchar_t *settings_type_name(SettingType type) {
    if (type < SETTING_CUSTOM || type >= SETTING_UNKNOWN) return L"Unknown";
    return TYPE_NAMES[type];
}

bool settings_type_writable(SettingType type) {
    return type == SETTING_BOOLEAN || type == SETTING_RANGE ||
           type == SETTING_STRING  || type == SETTING_LIST;
}

static const wchar_t *trim(const wchar_t *text, size_t *len) {
    while (*text && iswspace(*text)) text++;
    size_t n = wcslen(text);
    while (n > 0 && iswspace(text[n - 1])) n--;
    *len = n;
    return text;
}

static bool copy_trimmed(const wchar_t *text, wchar_t *out, size_t cap) {
    size_t n = 0;
    const wchar_t *start = trim(text, &n);
    if (n == 0 || n >= cap) return false;
    wmemcpy(out, start, n);
    out[n] = L'\0';
    return true;
}

static bool parse_bool(const wchar_t *text, bool *out) {
    static const wchar_t *const yes[] = { L"true", L"on", L"yes", L"1" };
    static const wchar_t *const no[]  = { L"false", L"off", L"no", L"0" };
    for (size_t k = 0; k < sizeof yes / sizeof yes[0]; k++) {
        if (equal_fold(text, yes[k])) { *out = true;  return true; }
        if (equal_fold(text, no[k]))  { *out = false; return true; }
    }
    return false;
}

static bool parse_signed(const wchar_t *text, long long lo, long long hi, long long *out) {
    wchar_t *end = NULL;
    errno = 0;
    long long v = wcstoll(text, &end, 10);
    if (end == text || *end || errno == ERANGE || v < lo || v > hi) return false;
    *out = v;
    return true;
}

static bool parse_unsigned(const wchar_t *text, unsigned long long hi, unsigned long long *out) {
    if (*text == L'-') return false;
    wchar_t *end = NULL;
    errno = 0;
    unsigned long long v = wcstoull(text, &end, 10);
    if (end == text || *end || errno == ERANGE || v > hi) return false;
    *out = v;
    return true;
}

bool settings_parse_value(SettingValueKind like, const wchar_t *text,
                          SettingValue *out, const char **error) {
    const char *why = NULL;
    wchar_t buf[SETTING_TEXT_MAX];

    memset(out, 0, sizeof *out);
    out->kind = like;

    if (!text || !copy_trimmed(text, buf, SETTING_TEXT_MAX)) {
        why = (text && *text) ? "value is too long" : "value is empty";
        goto fail;
    }

    switch (like) {
    case SV_BOOL:
        if (!parse_bool(buf, &out->b)) why = "expected true or false";
        break;
    case SV_INT32:
        if (!parse_signed(buf, INT32_MIN, INT32_MAX, &out->i)) why = "expected a whole number";
        break;
    case SV_INT64:
        if (!parse_signed(buf, INT64_MIN, INT64_MAX, &out->i)) why = "expected a whole number";
        break;
    case SV_UINT32:
        if (!parse_unsigned(buf, UINT32_MAX, &out->u)) why = "expected a whole number of 0 or more";
        break;
    case SV_UINT64:
        if (!parse_unsigned(buf, UINT64_MAX, &out->u)) why = "expected a whole number of 0 or more";
        break;
    case SV_DOUBLE: {
        wchar_t *end = NULL;
        errno = 0;
        out->d = wcstod(buf, &end);
        if (end == buf || *end || errno == ERANGE || !isfinite(out->d)) why = "expected a number";
        break;
    }
    case SV_STRING:
        wcscpy(out->s, buf);
        break;
    default:
        why = "this setting's value has a type mshell cannot write";
        break;
    }

    if (!why) return true;
fail:
    if (error) *error = why;
    return false;
}

static bool as_double(const SettingValue *v, double *out) {
    switch (v->kind) {
    case SV_INT32: case SV_INT64:   *out = (double)v->i; return true;
    case SV_UINT32: case SV_UINT64: *out = (double)v->u; return true;
    case SV_DOUBLE:                 *out = v->d;         return true;
    default:                        return false;
    }
}

bool settings_value_equal(const SettingValue *a, const SettingValue *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case SV_BOOL:                   return a->b == b->b;
    case SV_INT32: case SV_INT64:   return a->i == b->i;
    case SV_UINT32: case SV_UINT64: return a->u == b->u;
    case SV_DOUBLE:                 return a->d == b->d;
    case SV_STRING:                 return wcscmp(a->s, b->s) == 0;
    default:                        return false;
    }
}

bool settings_value_in_range(const SettingValue *v, const SettingValue *min,
                             const SettingValue *max) {
    double x, bound;
    if (!as_double(v, &x)) return true;
    if (min && as_double(min, &bound) && x < bound) return false;
    if (max && as_double(max, &bound) && x > bound) return false;
    return true;
}

void settings_format_value(const SettingValue *v, wchar_t *out, size_t cap) {
    if (!out || cap == 0) return;
    switch (v->kind) {
    case SV_BOOL:                   swprintf(out, cap, L"%ls", v->b ? L"true" : L"false"); break;
    case SV_INT32: case SV_INT64:   swprintf(out, cap, L"%lld", v->i); break;
    case SV_UINT32: case SV_UINT64: swprintf(out, cap, L"%llu", v->u); break;
    case SV_DOUBLE:                 swprintf(out, cap, L"%.15g", v->d); break;
    case SV_STRING:                 swprintf(out, cap, L"%ls", v->s); break;
    default:                        swprintf(out, cap, L"%ls", L"(none)"); break;
    }
    out[cap - 1] = L'\0';
}

static bool put(wchar_t *buf, size_t cap, size_t *used, wchar_t c) {
    if (*used + 1 >= cap) return false;
    buf[(*used)++] = c;
    return true;
}

size_t settings_append_arg(wchar_t *buf, size_t cap, size_t used,
                           const wchar_t *arg) {
    if (!buf || cap == 0 || used >= cap) return 0;

    bool quote = !*arg || wcspbrk(arg, L" \t\n\v\"") != NULL;

    if (used > 0 && !put(buf, cap, &used, L' ')) return 0;
    if (quote && !put(buf, cap, &used, L'"')) return 0;

    for (const wchar_t *p = arg; ; p++) {
        size_t slashes = 0;
        while (*p == L'\\') { slashes++; p++; }

        bool   before_quote = (!*p && quote) || *p == L'"';
        size_t doubled      = before_quote ? slashes * 2 : slashes;
        for (size_t k = 0; k < doubled; k++)
            if (!put(buf, cap, &used, L'\\')) return 0;

        if (!*p) break;
        if (*p == L'"' && !put(buf, cap, &used, L'\\')) return 0;
        if (!put(buf, cap, &used, *p)) return 0;
    }

    if (quote && !put(buf, cap, &used, L'"')) return 0;
    buf[used] = L'\0';
    return used;
}
