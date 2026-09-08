#pragma once

#include <windows.h>

PSECURITY_DESCRIPTOR pipe_sd_for_current_user(wchar_t *sid_out, size_t sid_cap);
