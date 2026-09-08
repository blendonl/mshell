#pragma once

#include <stdbool.h>
#include <wchar.h>

bool wildcard_match(const wchar_t *pat, const wchar_t *str);
