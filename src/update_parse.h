#pragma once

#include <stdbool.h>
#include <stddef.h>

int update_version_cmp(const char *a, const char *b);

bool update_json_str(const char *start, const char *end, const char *key,
                     char *out, size_t cap);

bool update_find_asset(const char *json, const char *suffix,
                       char *name,   size_t name_cap,
                       char *url,    size_t url_cap,
                       char *digest, size_t digest_cap);
