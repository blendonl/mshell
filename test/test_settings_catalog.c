#include "../src/settings_catalog.h"
#include "tests.h"

#include <ctype.h>
#include <stdlib.h>

static bool is_identifier(const char *s, size_t len) {
    if (len == 0 || !islower((unsigned char)s[0])) return false;
    for (size_t k = 0; k < len; k++)
        if (!islower((unsigned char)s[k]) && !isdigit((unsigned char)s[k]) && s[k] != '_') return false;
    return true;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static void test_rows_are_well_formed(void) {
    CHECK(settings_catalog_count() > 0, "catalog is not empty");
    CHECK(settings_catalog_at(-1) == NULL, "negative index");
    CHECK(settings_catalog_at(settings_catalog_count()) == NULL, "index past the end");

    for (int k = 0; k < settings_catalog_count(); k++) {
        const SettingField *f = settings_catalog_at(k);
        const char *dot = strchr(f->path, '.');
        CHECK(dot && strchr(dot + 1, '.') == NULL, "%s: path is namespace.field", f->path);
        if (!dot) continue;
        CHECK(is_identifier(f->path, (size_t)(dot - f->path)), "%s: namespace is an identifier", f->path);
        CHECK(is_identifier(dot + 1, strlen(dot + 1)), "%s: field is an identifier", f->path);
        CHECK(wcsncmp(f->id, SETTING_ID_PREFIX, wcslen(SETTING_ID_PREFIX)) == 0,
              "%s: handler id carries the SystemSettings_ prefix", f->path);
        CHECK(f->doc && f->doc[0], "%s: documented", f->path);
        CHECK(settings_catalog_find(f->path) == f, "%s: found by path", f->path);

        for (int j = k + 1; j < settings_catalog_count(); j++) {
            const SettingField *other = settings_catalog_at(j);
            CHECK(strcmp(f->path, other->path) != 0, "%s: path is unique", f->path);
            CHECK(wcscmp(f->id, other->id) != 0, "%s and %s share a handler id", f->path, other->path);
        }

        switch (f->kind) {
        case FIELD_INT:
            CHECK(f->min <= f->max && f->choices == NULL, "%s: sane bounds", f->path);
            break;
        case FIELD_BOOL:
            CHECK(f->choices == NULL && f->choice_count == 0, "%s: no choices", f->path);
            break;
        case FIELD_TOGGLE:
            CHECK(f->choice_count == 2 && !strcmp(f->choices[0].word, "false") &&
                  !strcmp(f->choices[1].word, "true"), "%s: toggle maps false then true", f->path);
            break;
        case FIELD_CHOICE:
            CHECK(f->choice_count >= 2, "%s: at least two choices", f->path);
            for (int c = 0; c < f->choice_count; c++) {
                CHECK(is_identifier(f->choices[c].word, strlen(f->choices[c].word)),
                      "%s: choice word %s is lowercase", f->path, f->choices[c].word);
                CHECK(f->choices[c].option && f->choices[c].option[0], "%s: choice has an option", f->path);
                for (int d = c + 1; d < f->choice_count; d++)
                    CHECK(strcmp(f->choices[c].word, f->choices[d].word) != 0,
                          "%s: choice words are unique", f->path);
            }
            break;
        }
    }
}

static const SettingField *must(const char *path) {
    const SettingField *f = settings_catalog_find(path);
    CHECK(f != NULL, "catalog has %s", path);
    return f;
}

static void test_find(void) {
    CHECK(settings_catalog_find("mouse.scroll_lines") != NULL, "plain path");
    CHECK(settings_catalog_find("mshell.mouse.scroll_lines") == settings_catalog_find("mouse.scroll_lines"),
          "mshell. prefix accepted");
    CHECK(settings_catalog_find("mouse.nope") == NULL, "unknown field");
    CHECK(settings_catalog_find(NULL) == NULL, "null path");

    const SettingField *f = must("keyboard.repeat_rate");
    if (!f) return;
    CHECK(!strcmp(settings_field_leaf(f), "repeat_rate"), "leaf");
    CHECK(settings_field_in_namespace(f, "keyboard"), "in its namespace");
    CHECK(!settings_field_in_namespace(f, "key"), "namespace is matched whole");
}

static void test_from_input(void) {
    char text[64], err[160];
    const SettingField *lines  = must("mouse.scroll_lines");
    const SettingField *by     = must("mouse.scroll_by");
    const SettingField *flag   = must("mouse.pointer_shadow");
    const SettingField *toggle = must("mouse.reverse_scroll");
    if (!lines || !by || !flag || !toggle) return;

    FieldInput in = { .tag = FIELD_IN_INT, .i = 5 };
    CHECK(settings_field_from_input(lines, &in, text, sizeof text, err, sizeof err) &&
          !strcmp(text, "5"), "integer in range");
    in.i = 101;
    CHECK(!settings_field_from_input(lines, &in, text, sizeof text, err, sizeof err) &&
          strstr(err, "1 to 100"), "integer above range names the range: %s", err);
    in.tag = FIELD_IN_NUMBER;
    CHECK(!settings_field_from_input(lines, &in, text, sizeof text, err, sizeof err), "fraction rejected");
    in.tag = FIELD_IN_STRING; in.s = "5";
    CHECK(!settings_field_from_input(lines, &in, text, sizeof text, err, sizeof err), "string for an integer");

    in.tag = FIELD_IN_BOOL; in.b = true;
    CHECK(settings_field_from_input(flag, &in, text, sizeof text, err, sizeof err) &&
          !strcmp(text, "true"), "boolean");
    in.tag = FIELD_IN_INT; in.i = 1;
    CHECK(!settings_field_from_input(flag, &in, text, sizeof text, err, sizeof err) &&
          strstr(err, "true or false"), "number for a boolean");

    in.tag = FIELD_IN_BOOL; in.b = false;
    CHECK(settings_field_from_input(toggle, &in, text, sizeof text, err, sizeof err) &&
          !strcmp(text, "false"), "toggle takes a boolean");

    in.tag = FIELD_IN_STRING; in.s = "Screen";
    CHECK(settings_field_from_input(by, &in, text, sizeof text, err, sizeof err) &&
          !strcmp(text, "screen"), "choice folds case to the word");
    in.s = "page";
    CHECK(!settings_field_from_input(by, &in, text, sizeof text, err, sizeof err) &&
          strstr(err, "lines, screen"), "unknown choice lists the words: %s", err);
    in.tag = FIELD_IN_BOOL;
    CHECK(!settings_field_from_input(by, &in, text, sizeof text, err, sizeof err), "boolean for a choice");
}

static void test_engine_text(void) {
    wchar_t out[128];
    char err[160];
    const SettingField *lines  = must("mouse.scroll_lines");
    const SettingField *by     = must("mouse.scroll_by");
    const SettingField *toggle = must("mouse.reverse_scroll");
    const SettingField *mode   = must("theme.mode");
    if (!lines || !by || !toggle || !mode) return;

    CHECK(settings_field_engine_text(lines, "7", out, 128, err, sizeof err) && !wcscmp(out, L"7"), "int");
    CHECK(!settings_field_engine_text(lines, "0", out, 128, err, sizeof err), "int below range");
    CHECK(!settings_field_engine_text(lines, "7x", out, 128, err, sizeof err), "int with junk");
    CHECK(!settings_field_engine_text(lines, "99999999999", out, 128, err, sizeof err), "int overflow");
    CHECK(settings_field_engine_text(by, "SCREEN", out, 128, err, sizeof err) &&
          !wcscmp(out, L"SystemSettings_Input_Mouse_Page"), "choice becomes its option key");
    CHECK(settings_field_engine_text(toggle, "on", out, 128, err, sizeof err) &&
          !wcscmp(out, L"SystemSettings_Input_Mouse_Wheel_Reversed"), "toggle on");
    CHECK(settings_field_engine_text(toggle, "false", out, 128, err, sizeof err) &&
          !wcscmp(out, L"SystemSettings_Input_Mouse_Wheel_Regular"), "toggle off");
    CHECK(!settings_field_engine_text(toggle, "reversed", out, 128, err, sizeof err), "toggle wants a boolean");
    CHECK(settings_field_engine_text(mode, "dark", out, 128, err, sizeof err) && !wcscmp(out, L"Dark"),
          "theme mode");
}

static void test_word_of(void) {
    char word[64];
    SettingValue raw;
    const SettingField *lines  = must("mouse.scroll_lines");
    const SettingField *by     = must("mouse.scroll_by");
    const SettingField *toggle = must("mouse.reverse_scroll");
    const SettingField *flag   = must("mouse.pointer_shadow");
    if (!lines || !by || !toggle || !flag) return;

    memset(&raw, 0, sizeof raw);
    raw.kind = SV_UINT32; raw.u = 3;
    CHECK(settings_field_word_of(lines, &raw, word, sizeof word) && !strcmp(word, "3"), "uint word");
    raw.kind = SV_INT32; raw.i = -2;
    CHECK(settings_field_word_of(lines, &raw, word, sizeof word) && !strcmp(word, "-2"), "int word");

    raw.kind = SV_STRING;
    wcscpy(raw.s, L"SystemSettings_Input_Mouse_Line");
    CHECK(settings_field_word_of(by, &raw, word, sizeof word) && !strcmp(word, "lines"), "option to word");
    wcscpy(raw.s, L"SystemSettings_Input_Mouse_Wheel_Reversed");
    CHECK(settings_field_word_of(toggle, &raw, word, sizeof word) && !strcmp(word, "true"), "toggle to true");
    wcscpy(raw.s, L"Something_Else");
    CHECK(!settings_field_word_of(by, &raw, word, sizeof word), "unknown option");

    raw.kind = SV_BOOL; raw.b = true;
    CHECK(settings_field_word_of(flag, &raw, word, sizeof word) && !strcmp(word, "true"), "bool word");
    CHECK(!settings_field_word_of(lines, &raw, word, sizeof word), "bool for an int field");
}

static void test_describe(void) {
    char out[64];
    const SettingField *lines = must("mouse.scroll_lines");
    const SettingField *mode  = must("theme.mode");
    const SettingField *flag  = must("gaming.game_mode");
    if (!lines || !mode || !flag) return;
    settings_field_describe(lines, out, sizeof out);
    CHECK(!strcmp(out, "1..100"), "range: %s", out);
    settings_field_describe(mode, out, sizeof out);
    CHECK(!strcmp(out, "light|dark|custom"), "choices: %s", out);
    settings_field_describe(flag, out, sizeof out);
    CHECK(!strcmp(out, "true|false"), "boolean: %s", out);
}

static void test_documented_in_meta(void) {
    char *types = read_file("meta/types.lua");
    char *spec  = read_file("src/api_spec.c");
    CHECK(types != NULL, "meta/types.lua readable from the repo root");
    CHECK(spec != NULL, "src/api_spec.c readable from the repo root");
    if (!types || !spec) { free(types); free(spec); return; }

    for (int k = 0; k < settings_catalog_count(); k++) {
        const SettingField *f = settings_catalog_at(k);
        size_t ns_len = (size_t)(strchr(f->path, '.') - f->path);

        char header[96], field[96], setup[96];
        snprintf(header, sizeof header, "---@class mshell.%c%.*sOpts\n",
                 toupper((unsigned char)f->path[0]), (int)ns_len - 1, f->path + 1);
        snprintf(field, sizeof field, "\n---@field %s ", settings_field_leaf(f));
        snprintf(setup, sizeof setup, "{ \"%.*s.setup\",", (int)ns_len, f->path);

        const char *cls = strstr(types, header);
        CHECK(cls != NULL, "%s: types.lua declares %.*s", f->path, (int)strlen(header) - 1, header);
        CHECK(strstr(spec, setup) != NULL, "%s: api_spec has %.*s.setup", f->path, (int)ns_len, f->path);
        if (!cls) continue;

        const char *end = strstr(cls + strlen(header), "---@class ");
        const char *hit = strstr(cls, field);
        CHECK(hit && (!end || hit < end), "%s: types.lua documents the field in its class", f->path);
    }
    free(types);
    free(spec);
}

int main(void) {
    test_rows_are_well_formed();
    test_find();
    test_from_input();
    test_engine_text();
    test_word_of();
    test_describe();
    test_documented_in_meta();
    return tests_report("settings_catalog");
}
