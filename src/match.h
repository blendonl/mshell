#pragma once

/*
 * match.h — pattern matching for window and desktop rules.
 *
 * Split out of window.c so it can be compiled and tested on the host without
 * Windows: this file deliberately depends on nothing but the C library. It is
 * the one piece of hand-written parsing in mshell, it decides which rule a
 * window gets, and a bug in it shows up as "my rule doesn't work" rather than
 * as a crash — exactly the shape of bug that needs tests.
 */

#include <stdbool.h>
#include <wchar.h>

/* Case-insensitive wildcard match:
 *   `*`  matches any run of characters (including none)
 *   `?`  matches exactly one character
 *   `/` and `\` compare equal, so a path can be written either way
 *
 * A pattern containing no wildcard is therefore just a case-insensitive exact
 * match. An empty pattern matches only an empty string — callers treat "no
 * pattern given" as "matches anything" before getting here.
 */
bool wildcard_match(const wchar_t *pat, const wchar_t *str);
