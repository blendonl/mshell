#pragma once

#include <stdbool.h>
#include <wchar.h>

typedef enum {
    ATTACH_END = 0,
    ATTACH_MASTER,
    ATTACH_AFTER,
} AttachPolicy;

bool desktop_list_name_ok(const wchar_t *name, size_t cap);

bool desktop_name_eq(const wchar_t *a, const wchar_t *b);

int desktop_name_cmp(const wchar_t *a, const wchar_t *b);

int desktop_attach_index(AttachPolicy policy, int focused, int count);

int desktop_focus_after_remove(int focused, int removed, int count);

int desktop_hist_shift(int n, int found, int cap, int *n_out);
