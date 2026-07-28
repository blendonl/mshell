/* ===========================================================================
 * update_parse.c — the Windows-free half of update.c. See update_parse.h.
 * =========================================================================== */
#include "update_parse.h"

#include <stdio.h>
#include <string.h>

/* Local rather than _stricmp/strcasecmp: this file is compiled by both
 * mingw-w64 and the host compiler, and the two disagree about which of those
 * spellings exists. ASCII is all these fields contain. */
static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (lower_ascii(*a) != lower_ascii(*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

int update_version_cmp(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;

    while (*a || *b) {
        int va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') va = va * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9') vb = vb * 10 + (*b++ - '0');
        if (va != vb) return va - vb;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
        /* A non-numeric suffix ("-rc1") stops the comparison rather than being
         * guessed at. */
        if ((*a && (*a < '0' || *a > '9')) ||
            (*b && (*b < '0' || *b > '9'))) break;
    }
    return 0;
}

/* `p` points at the opening quote; returns the character after the closing
 * one, honouring backslash escapes. */
static const char *json_skip_string(const char *p, const char *end) {
    p++;
    while (p < end && *p) {
        if (*p == '\\' && p + 1 < end && p[1]) { p += 2; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

bool update_json_str(const char *start, const char *end, const char *key,
                     char *out, size_t cap) {
    if (!start || !end || !key || !out || cap == 0) return false;

    size_t keylen = strlen(key);

    for (const char *p = start; p < end && *p; ) {
        if (*p != '"') { p++; continue; }

        /* A quote here opens either the key we want or a string to step over.
         * Matching the closing quote too is what stops "url" from hitting
         * inside "browser_download_url". */
        if ((size_t)(end - p) > keylen + 1 &&
            strncmp(p + 1, key, keylen) == 0 && p[1 + keylen] == '"') {

            const char *q = p + 2 + keylen;
            while (q < end && (*q == ' ' || *q == '\t' ||
                               *q == '\n' || *q == '\r' || *q == ':')) q++;
            if (q >= end || *q != '"') return false;   /* not a string value */

            q++;
            size_t n = 0;
            while (q < end && *q != '"' && n < cap - 1) {
                /* The fields read here (versions, URLs, hex digests) contain
                 * no escapes; an unescaped copy keeps this honest about what
                 * it supports rather than silently mangling one. */
                if (*q == '\\') return false;
                out[n++] = *q++;
            }
            out[n] = '\0';
            return n > 0;
        }

        p = json_skip_string(p, end);
    }
    return false;
}

bool update_find_asset(const char *json, const char *suffix,
                       char *name,   size_t name_cap,
                       char *url,    size_t url_cap,
                       char *digest, size_t digest_cap) {
    if (!json || !suffix || !name || !url || !digest) return false;
    if (name_cap == 0 || url_cap == 0 || digest_cap == 0) return false;

    const char *end = json + strlen(json);

    const char *a = strstr(json, "\"assets\"");
    if (!a) return false;
    a = strchr(a, '[');
    if (!a) return false;
    a++;

    size_t suffix_len = strlen(suffix);

    while (a < end) {
        while (a < end && *a != '{' && *a != ']') a++;
        if (a >= end || *a == ']') return false;

        /* Bound this asset object, stepping over strings so that a brace
         * inside one cannot move the boundary. The uploader's "{owner}{repo}"
         * templates are balanced and would survive naive counting; a lone "{"
         * in an asset's free-text label would not, and would swallow every
         * asset after this one. */
        const char *obj = a;
        int depth = 0;
        while (a < end) {
            if (*a == '"') { a = json_skip_string(a, end); continue; }
            if (*a == '{') { depth++; a++; continue; }
            if (*a == '}') {
                depth--;
                a++;
                if (depth <= 0) break;
                continue;
            }
            a++;
        }

        char n[256];
        if (update_json_str(obj, a, "name", n, sizeof(n))) {
            size_t nl = strlen(n);
            if (nl > suffix_len && ci_equal(n + nl - suffix_len, suffix)) {
                if (!update_json_str(obj, a, "browser_download_url",
                                     url, url_cap))
                    return false;

                snprintf(name, name_cap, "%s", n);

                /* Optional — absence costs the integrity check, not the
                 * update. */
                if (!update_json_str(obj, a, "digest", digest, digest_cap))
                    digest[0] = '\0';

                return true;
            }
        }
    }
    return false;
}
