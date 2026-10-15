#include "settings_catalog.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

#define CHOICES(list) (list), (int)(sizeof(list) / sizeof((list)[0]))

static const FieldChoice SCROLL_BY[] = {
    { "lines",  L"SystemSettings_Input_Mouse_Line" },
    { "screen", L"SystemSettings_Input_Mouse_Page" },
};

static const FieldChoice WHEEL_DIRECTION[] = {
    { "false", L"SystemSettings_Input_Mouse_Wheel_Regular" },
    { "true",  L"SystemSettings_Input_Mouse_Wheel_Reversed" },
};

static const FieldChoice COLOR_MODE[] = {
    { "light",  L"Light" },
    { "dark",   L"Dark" },
    { "custom", L"Custom" },
};

static const FieldChoice LIGHT_DARK[] = {
    { "light", L"Light" },
    { "dark",  L"Dark" },
};

static const FieldChoice BUTTON_ACTIONS[] = {
    { "nothing",     L"0" },
    { "sleep",       L"1" },
    { "hibernate",   L"2" },
    { "shutdown",    L"3" },
    { "display_off", L"4" },
};

static const FieldChoice LID_ACTIONS[] = {
    { "nothing",   L"0" },
    { "sleep",     L"1" },
    { "hibernate", L"2" },
    { "shutdown",  L"3" },
};

#define SUB_VIDEO      L"{7516b95f-f776-4464-8c53-06167f40cc99}"
#define SUB_SLEEP      L"{238c9fa8-0aad-41ed-83f4-97be242c8f20}"
#define SUB_BUTTONS    L"{4f971e89-eebd-4455-a8de-9e59040e7347}"
#define VIDEO_IDLE     L"{3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e}"
#define STANDBY_IDLE   L"{29f6c1db-86da-48c5-9fdb-f2b67b1f44da}"
#define HIBERNATE_IDLE L"{9d7815a6-7ee4-497e-8888-515a05f02364}"
#define POWER_BUTTON   L"{7648efa3-dd9c-4e3e-b566-50f929386280}"
#define SLEEP_BUTTON   L"{96996bc0-ad50-47ec-923b-6f41874dd9eb}"
#define LID_CLOSE      L"{5ca83367-6e45-459f-a27b-476b1d01c936}"

#define NO_POWER NULL, NULL, false, 0

#define POWER_TIMEOUT(path, battery, sub, setting, doc) \
    { path, NULL, FIELD_INT, 0, 1440, NULL, 0, doc, sub, setting, battery, 60 }

#define POWER_ACTION(path, battery, setting, list, doc) \
    { path, NULL, FIELD_CHOICE, 0, 0, CHOICES(list), doc, SUB_BUTTONS, setting, battery, 1 }

static const SettingField CATALOG[] = {
    { "mouse.scroll_lines", L"SystemSettings_Input_Mouse_SetScrollLines",
      FIELD_INT, 1, 100, NULL, 0,
      "Lines one notch of the wheel scrolls.", NO_POWER },
    { "mouse.scroll_by", L"SystemSettings_Input_Mouse_SetScrollPage",
      FIELD_CHOICE, 0, 0, CHOICES(SCROLL_BY),
      "Whether a notch scrolls lines or a whole screen.", NO_POWER },
    { "mouse.scroll_inactive", L"SystemSettings_Input_Mouse_WheelRouting",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Scroll the window under the pointer even when it is not focused.", NO_POWER },
    { "mouse.reverse_scroll", L"SystemSettings_Input_Mouse_ReverseWheelDirection",
      FIELD_TOGGLE, 0, 0, CHOICES(WHEEL_DIRECTION),
      "Scroll the other way: rolling down moves the content up.", NO_POWER },
    { "mouse.cursor_size", L"SystemSettings_Accessibility_MouseCursorSize",
      FIELD_INT, 1, 15, NULL, 0,
      "Pointer size, 1 being Windows' normal size.", NO_POWER },
    { "mouse.double_click_speed", L"SystemSettings_Accessibility_MouseDblClickSpeed",
      FIELD_INT, 1, 11, NULL, 0,
      "How fast a double-click must be, 1 slowest to 11 fastest.", NO_POWER },
    { "mouse.hide_while_typing", L"SystemSettings_Accessibility_MouseHideMouseWhileTyping",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Hide the pointer while you type.", NO_POWER },
    { "mouse.pointer_shadow", L"SystemSettings_Accessibility_MousePointerShadow",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Draw a shadow under the pointer.", NO_POWER },
    { "mouse.trails", L"SystemSettings_Accessibility_MouseTrails",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Leave a trail behind the pointer.", NO_POWER },
    { "mouse.trails_length", L"SystemSettings_Accessibility_MouseTrailsLength",
      FIELD_INT, 2, 7, NULL, 0,
      "Length of that trail. Only takes effect with trails on.", NO_POWER },
    { "mouse.locate_with_ctrl", L"SystemSettings_Accessibility_MouseSonar",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Pressing Ctrl rings the pointer so you can find it.", NO_POWER },
    { "mouse.click_lock", L"SystemSettings_Accessibility_MouseClickLock",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Holding the button briefly locks it down, so a drag needs no held button.", NO_POWER },
    { "mouse.snap_to_default", L"SystemSettings_Accessibility_MouseSnapToDefaultButton",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Move the pointer onto a dialog's default button when it opens.", NO_POWER },

    { "keyboard.repeat_delay", L"SystemSettings_Accessibility_Keyboard_CharacterRepeatDelay",
      FIELD_INT, 0, 3, NULL, 0,
      "Wait before a held key repeats, 0 shortest (about 250 ms) to 3 longest (about 1 s).", NO_POWER },
    { "keyboard.repeat_rate", L"SystemSettings_Accessibility_Keyboard_CharacterRepeatRate",
      FIELD_INT, 0, 31, NULL, 0,
      "How fast a held key repeats, 0 slowest to 31 fastest.", NO_POWER },
    { "keyboard.sticky_keys", L"SystemSettings_Accessibility_Keyboard_IsStickyKeysEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Sticky Keys: press modifiers one at a time.", NO_POWER },
    { "keyboard.sticky_keys_shortcut", L"SystemSettings_Accessibility_Keyboard_StickyShortcutEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Pressing Shift five times turns Sticky Keys on.", NO_POWER },
    { "keyboard.filter_keys", L"SystemSettings_Accessibility_Keyboard_IsFilterKeysEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Filter Keys: ignore brief or repeated keystrokes.", NO_POWER },
    { "keyboard.filter_keys_shortcut", L"SystemSettings_Accessibility_Keyboard_FilterShortcutEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Holding right Shift for eight seconds turns Filter Keys on.", NO_POWER },
    { "keyboard.toggle_keys", L"SystemSettings_Accessibility_Keyboard_IsToggleKeysEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Toggle Keys: beep on Caps, Num and Scroll Lock.", NO_POWER },
    { "keyboard.toggle_keys_shortcut", L"SystemSettings_Accessibility_Keyboard_ToggleShortcutEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Holding Num Lock for five seconds turns Toggle Keys on.", NO_POWER },
    { "keyboard.underline_access_keys", L"SystemSettings_Accessibility_Keyboard_IsUnderlineShortcutEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Always underline the access key in menus and dialogs.", NO_POWER },

    { "theme.mode", L"SystemSettings_Personalize_Color_ColorMode",
      FIELD_CHOICE, 0, 0, CHOICES(COLOR_MODE),
      "Light or dark everywhere, or custom to pick apps and system separately.", NO_POWER },
    { "theme.apps", L"SystemSettings_Personalize_Color_AppsTheme",
      FIELD_CHOICE, 0, 0, CHOICES(LIGHT_DARK),
      "Theme apps use when mode is custom.", NO_POWER },
    { "theme.system", L"SystemSettings_Personalize_Color_SystemTheme",
      FIELD_CHOICE, 0, 0, CHOICES(LIGHT_DARK),
      "Theme Windows' own surfaces use when mode is custom.", NO_POWER },
    { "theme.transparency", L"SystemSettings_Personalize_Color_EnableTransparency",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Translucent window and menu backgrounds.", NO_POWER },
    { "theme.accent_title_bars", L"SystemSettings_Personalize_Color_ColorPrevalenceTitleBar",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Paint title bars and window borders in the accent colour.", NO_POWER },
    { "theme.animations", L"SystemSettings_EaseOfAccess_IsAnimationsEnabled",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Windows' own animation effects.", NO_POWER },
    { "theme.always_show_scrollbars", L"SystemSettings_EaseOfAccess_AlwaysShowScrollbars",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Keep scrollbars visible instead of collapsing them.", NO_POWER },

    { "gaming.game_mode", L"SystemSettings_Gaming_GameMode_Toggle",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Game Mode: prioritise the game in the foreground.", NO_POWER },
    { "gaming.game_bar", L"SystemSettings_Gaming_GameBar_Toggle",
      FIELD_BOOL, 0, 0, NULL, 0,
      "Let a controller's Xbox button open Game Bar.", NO_POWER },

    POWER_TIMEOUT("power.screen_off", false, SUB_VIDEO, VIDEO_IDLE,
                  "Minutes idle before the screen turns off when plugged in; 0 never."),
    POWER_TIMEOUT("power.screen_off_battery", true, SUB_VIDEO, VIDEO_IDLE,
                  "Minutes idle before the screen turns off on battery; 0 never."),
    POWER_TIMEOUT("power.sleep", false, SUB_SLEEP, STANDBY_IDLE,
                  "Minutes idle before the PC sleeps when plugged in; 0 never."),
    POWER_TIMEOUT("power.sleep_battery", true, SUB_SLEEP, STANDBY_IDLE,
                  "Minutes idle before the PC sleeps on battery; 0 never."),
    POWER_TIMEOUT("power.hibernate", false, SUB_SLEEP, HIBERNATE_IDLE,
                  "Minutes idle before the PC hibernates when plugged in; 0 never."),
    POWER_TIMEOUT("power.hibernate_battery", true, SUB_SLEEP, HIBERNATE_IDLE,
                  "Minutes idle before the PC hibernates on battery; 0 never."),
    POWER_ACTION("power.power_button", false, POWER_BUTTON, BUTTON_ACTIONS,
                 "What the power button does when plugged in."),
    POWER_ACTION("power.power_button_battery", true, POWER_BUTTON, BUTTON_ACTIONS,
                 "What the power button does on battery."),
    POWER_ACTION("power.sleep_button", false, SLEEP_BUTTON, BUTTON_ACTIONS,
                 "What the sleep button does when plugged in."),
    POWER_ACTION("power.sleep_button_battery", true, SLEEP_BUTTON, BUTTON_ACTIONS,
                 "What the sleep button does on battery."),
    POWER_ACTION("power.lid", false, LID_CLOSE, LID_ACTIONS,
                 "What closing the lid does when plugged in."),
    POWER_ACTION("power.lid_battery", true, LID_CLOSE, LID_ACTIONS,
                 "What closing the lid does on battery."),
};

#define CATALOG_COUNT ((int)(sizeof CATALOG / sizeof CATALOG[0]))

int settings_catalog_count(void) {
    return CATALOG_COUNT;
}

const SettingField *settings_catalog_at(int index) {
    return (index >= 0 && index < CATALOG_COUNT) ? &CATALOG[index] : NULL;
}

const SettingField *settings_catalog_find(const char *path) {
    if (!path) return NULL;
    if (strncmp(path, "mshell.", 7) == 0) path += 7;
    for (int k = 0; k < CATALOG_COUNT; k++)
        if (strcmp(CATALOG[k].path, path) == 0) return &CATALOG[k];
    return NULL;
}

const char *settings_field_leaf(const SettingField *f) {
    const char *dot = strrchr(f->path, '.');
    return dot ? dot + 1 : f->path;
}

bool settings_field_in_namespace(const SettingField *f, const char *ns) {
    size_t n = strlen(ns);
    return strncmp(f->path, ns, n) == 0 && f->path[n] == '.';
}

bool settings_field_is_power(const SettingField *f) {
    return f->power_setting != NULL;
}

static void set_error(char *error, size_t cap, const char *fmt, ...) {
    if (!error || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error, cap, fmt, ap);
    va_end(ap);
}

static void choice_words(const SettingField *f, char *out, size_t cap) {
    size_t used = 0;
    out[0] = '\0';
    for (int k = 0; k < f->choice_count && used + 1 < cap; k++) {
        int n = snprintf(out + used, cap - used, "%s%s", k ? ", " : "", f->choices[k].word);
        if (n < 0) break;
        used += (size_t)n;
    }
}

static bool equal_fold_ascii(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

static int choice_index(const SettingField *f, const char *word) {
    for (int k = 0; k < f->choice_count; k++)
        if (equal_fold_ascii(word, f->choices[k].word)) return k;
    return -1;
}

static bool parse_bool_text(const char *text, bool *out) {
    static const char *const yes[] = { "true", "on", "yes", "1" };
    static const char *const no[]  = { "false", "off", "no", "0" };
    for (size_t k = 0; k < sizeof yes / sizeof yes[0]; k++) {
        if (equal_fold_ascii(text, yes[k])) { *out = true;  return true; }
        if (equal_fold_ascii(text, no[k]))  { *out = false; return true; }
    }
    return false;
}

static bool parse_int_text(const char *text, long long *out) {
    const char *p = text;
    if (*p == '-' || *p == '+') p++;
    if (!isdigit((unsigned char)*p)) return false;
    long long v = 0;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        if (v > 100000000LL) return false;
        v = v * 10 + (*p - '0');
    }
    *out = (text[0] == '-') ? -v : v;
    return true;
}

bool settings_field_from_input(const SettingField *f, const FieldInput *in,
                               char *text, size_t cap, char *error, size_t error_cap) {
    const char *leaf = settings_field_leaf(f);
    char words[256];

    switch (f->kind) {
    case FIELD_BOOL:
    case FIELD_TOGGLE:
        if (in->tag != FIELD_IN_BOOL) {
            set_error(error, error_cap, "%s must be true or false", leaf);
            return false;
        }
        snprintf(text, cap, "%s", in->b ? "true" : "false");
        return true;

    case FIELD_INT:
        if (in->tag != FIELD_IN_INT || in->i < f->min || in->i > f->max) {
            set_error(error, error_cap, "%s must be a whole number from %d to %d",
                      leaf, f->min, f->max);
            return false;
        }
        snprintf(text, cap, "%lld", in->i);
        return true;

    case FIELD_CHOICE: {
        int pick = (in->tag == FIELD_IN_STRING && in->s) ? choice_index(f, in->s) : -1;
        if (pick < 0) {
            choice_words(f, words, sizeof words);
            set_error(error, error_cap, "%s must be one of: %s", leaf, words);
            return false;
        }
        snprintf(text, cap, "%s", f->choices[pick].word);
        return true;
    }
    }
    set_error(error, error_cap, "%s has a kind mshell does not know", leaf);
    return false;
}

bool settings_field_engine_text(const SettingField *f, const char *text,
                                wchar_t *out, size_t cap, char *error, size_t error_cap) {
    FieldInput in = { .tag = FIELD_IN_OTHER };
    bool b = false;
    long long i = 0;

    if (f->kind == FIELD_BOOL || f->kind == FIELD_TOGGLE) {
        if (parse_bool_text(text, &b)) { in.tag = FIELD_IN_BOOL; in.b = b; }
    } else if (f->kind == FIELD_INT) {
        if (parse_int_text(text, &i)) { in.tag = FIELD_IN_INT; in.i = i; }
    } else {
        in.tag = FIELD_IN_STRING;
        in.s   = text;
    }

    char canonical[SETTING_FIELD_TEXT_MAX];
    if (!settings_field_from_input(f, &in, canonical, sizeof canonical, error, error_cap))
        return false;

    switch (f->kind) {
    case FIELD_BOOL:
        swprintf(out, cap, L"%ls", in.b ? L"true" : L"false");
        return true;
    case FIELD_INT:
        swprintf(out, cap, L"%lld", in.i);
        return true;
    case FIELD_TOGGLE:
        swprintf(out, cap, L"%ls", f->choices[in.b ? 1 : 0].option);
        return true;
    case FIELD_CHOICE:
        swprintf(out, cap, L"%ls", f->choices[choice_index(f, canonical)].option);
        return true;
    }
    return false;
}

static bool wide_equal_fold(const wchar_t *a, const wchar_t *b) {
    for (; *a && *b; a++, b++)
        if (towlower(*a) != towlower(*b)) return false;
    return *a == *b;
}

bool settings_field_word_of(const SettingField *f, const SettingValue *raw,
                            char *out, size_t cap) {
    switch (f->kind) {
    case FIELD_BOOL:
        if (raw->kind != SV_BOOL) return false;
        snprintf(out, cap, "%s", raw->b ? "true" : "false");
        return true;
    case FIELD_INT:
        if (raw->kind == SV_INT32 || raw->kind == SV_INT64) { snprintf(out, cap, "%lld", raw->i); return true; }
        if (raw->kind == SV_UINT32 || raw->kind == SV_UINT64) { snprintf(out, cap, "%llu", raw->u); return true; }
        return false;
    case FIELD_CHOICE:
    case FIELD_TOGGLE:
        if (raw->kind != SV_STRING) return false;
        for (int k = 0; k < f->choice_count; k++) {
            if (!wide_equal_fold(raw->s, f->choices[k].option)) continue;
            snprintf(out, cap, "%s", f->choices[k].word);
            return true;
        }
        return false;
    }
    return false;
}

void settings_field_describe(const SettingField *f, char *out, size_t cap) {
    switch (f->kind) {
    case FIELD_BOOL:
    case FIELD_TOGGLE:
        snprintf(out, cap, "true|false");
        return;
    case FIELD_INT:
        snprintf(out, cap, "%d..%d", f->min, f->max);
        return;
    case FIELD_CHOICE: {
        size_t used = 0;
        out[0] = '\0';
        for (int k = 0; k < f->choice_count && used + 1 < cap; k++) {
            int n = snprintf(out + used, cap - used, "%s%s", k ? "|" : "", f->choices[k].word);
            if (n < 0) break;
            used += (size_t)n;
        }
        return;
    }
    }
    snprintf(out, cap, "?");
}
