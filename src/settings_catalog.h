#pragma once

#include "settings_parse.h"

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef enum {
    FIELD_BOOL,
    FIELD_INT,
    FIELD_CHOICE,
    FIELD_TOGGLE,
} FieldKind;

typedef struct {
    const char    *word;
    const wchar_t *option;
} FieldChoice;

typedef struct {
    const char        *path;
    const wchar_t     *id;
    FieldKind          kind;
    int                min;
    int                max;
    const FieldChoice *choices;
    int                choice_count;
    const char        *doc;
} SettingField;

typedef enum {
    FIELD_IN_BOOL,
    FIELD_IN_INT,
    FIELD_IN_NUMBER,
    FIELD_IN_STRING,
    FIELD_IN_OTHER,
} FieldInputTag;

typedef struct {
    FieldInputTag tag;
    bool          b;
    long long     i;
    const char   *s;
} FieldInput;

#define SETTING_FIELD_TEXT_MAX  128

int                 settings_catalog_count(void);
const SettingField *settings_catalog_at(int index);
const SettingField *settings_catalog_find(const char *path);

const char *settings_field_leaf(const SettingField *f);
bool        settings_field_in_namespace(const SettingField *f, const char *ns);

bool settings_field_from_input(const SettingField *f, const FieldInput *in,
                               char *text, size_t cap, char *error, size_t error_cap);

bool settings_field_engine_text(const SettingField *f, const char *text,
                                wchar_t *out, size_t cap, char *error, size_t error_cap);

bool settings_field_word_of(const SettingField *f, const SettingValue *raw,
                            char *out, size_t cap);

void settings_field_describe(const SettingField *f, char *out, size_t cap);
