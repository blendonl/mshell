#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#define SETTING_ID_PREFIX  L"SystemSettings_"
#define SETTING_TEXT_MAX   256

typedef enum {
    SETTING_CUSTOM = 0,
    SETTING_DISPLAY_STRING,
    SETTING_LABELED_STRING,
    SETTING_BOOLEAN,
    SETTING_RANGE,
    SETTING_STRING,
    SETTING_LIST,
    SETTING_ACTION,
    SETTING_COLLECTION,
    SETTING_UNKNOWN,
} SettingType;

typedef enum {
    SV_NONE = 0,
    SV_BOOL,
    SV_INT32,
    SV_UINT32,
    SV_INT64,
    SV_UINT64,
    SV_DOUBLE,
    SV_STRING,
} SettingValueKind;

typedef struct {
    SettingValueKind kind;
    bool               b;
    long long          i;
    unsigned long long u;
    double             d;
    wchar_t            s[SETTING_TEXT_MAX];
} SettingValue;

const wchar_t *settings_type_name(SettingType type);
bool           settings_type_writable(SettingType type);

bool settings_parse_value(SettingValueKind like, const wchar_t *text,
                          SettingValue *out, const char **error);
bool settings_value_equal(const SettingValue *a, const SettingValue *b);
bool settings_value_in_range(const SettingValue *v, const SettingValue *min,
                             const SettingValue *max);
void settings_format_value(const SettingValue *v, wchar_t *out, size_t cap);

size_t settings_append_arg(wchar_t *buf, size_t cap, size_t used,
                           const wchar_t *arg);
