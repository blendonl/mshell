#include "mshell.h"
#include "keys.h"

typedef struct {
    const char *name;
    DWORD       vk;
} KeyNameEntry;

static const KeyNameEntry key_names[] = {
    {"a", 'A'}, {"b", 'B'}, {"c", 'C'}, {"d", 'D'}, {"e", 'E'},
    {"f", 'F'}, {"g", 'G'}, {"h", 'H'}, {"i", 'I'}, {"j", 'J'},
    {"k", 'K'}, {"l", 'L'}, {"m", 'M'}, {"n", 'N'}, {"o", 'O'},
    {"p", 'P'}, {"q", 'Q'}, {"r", 'R'}, {"s", 'S'}, {"t", 'T'},
    {"u", 'U'}, {"v", 'V'}, {"w", 'W'}, {"x", 'X'}, {"y", 'Y'}, {"z", 'Z'},

    {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
    {"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},

    {"`",  VK_OEM_3},     {"~",  VK_OEM_3},
    {"-",  VK_OEM_MINUS}, {"_",  VK_OEM_MINUS},
    {"=",  VK_OEM_PLUS},  {"+",  VK_OEM_PLUS},
    {"[",  VK_OEM_4},     {"{",  VK_OEM_4},
    {"]",  VK_OEM_6},     {"}",  VK_OEM_6},
    {"\\", VK_OEM_5},     {"|",  VK_OEM_5},
    {";",  VK_OEM_1},     {":",  VK_OEM_1},
    {"'",  VK_OEM_7},     {"\"", VK_OEM_7},
    {",",  VK_OEM_COMMA}, {"<",  VK_OEM_COMMA},
    {".",  VK_OEM_PERIOD},{">",  VK_OEM_PERIOD},
    {"/",  VK_OEM_2},     {"?",  VK_OEM_2},

    {"Left",      VK_LEFT},
    {"Right",     VK_RIGHT},
    {"Up",        VK_UP},
    {"Down",      VK_DOWN},
    {"Home",      VK_HOME},
    {"End",       VK_END},
    {"PageUp",    VK_PRIOR},
    {"PageDown",  VK_NEXT},

    {"Return",    VK_RETURN},
    {"Enter",     VK_RETURN},
    {"Space",     VK_SPACE},
    {"Tab",       VK_TAB},
    {"Escape",    VK_ESCAPE},
    {"Esc",       VK_ESCAPE},
    {"Backspace", VK_BACK},
    {"Delete",    VK_DELETE},
    {"Insert",    VK_INSERT},
    {"PrintScreen", VK_SNAPSHOT},
    {"Pause",     VK_PAUSE},
    {"CapsLock",  VK_CAPITAL},

    {"VolumeUp",   VK_VOLUME_UP},
    {"VolumeDown", VK_VOLUME_DOWN},
    {"VolumeMute", VK_VOLUME_MUTE},
    {"MediaPlay",  VK_MEDIA_PLAY_PAUSE},
    {"MediaNext",  VK_MEDIA_NEXT_TRACK},
    {"MediaPrev",  VK_MEDIA_PREV_TRACK},
    {"MediaStop",  VK_MEDIA_STOP},
    {"BrowserBack",    VK_BROWSER_BACK},
    {"BrowserForward", VK_BROWSER_FORWARD},
    {"BrowserRefresh", VK_BROWSER_REFRESH},
    {"BrowserHome",    VK_BROWSER_HOME},
    {"NumLock",   VK_NUMLOCK},
    {"ScrollLock", VK_SCROLL},

    {"F1",  VK_F1},  {"F2",  VK_F2},  {"F3",  VK_F3},  {"F4",  VK_F4},
    {"F5",  VK_F5},  {"F6",  VK_F6},  {"F7",  VK_F7},  {"F8",  VK_F8},
    {"F9",  VK_F9},  {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},

    {"Numpad0", VK_NUMPAD0}, {"Numpad1", VK_NUMPAD1}, {"Numpad2", VK_NUMPAD2},
    {"Numpad3", VK_NUMPAD3}, {"Numpad4", VK_NUMPAD4}, {"Numpad5", VK_NUMPAD5},
    {"Numpad6", VK_NUMPAD6}, {"Numpad7", VK_NUMPAD7}, {"Numpad8", VK_NUMPAD8},
    {"Numpad9", VK_NUMPAD9},

    {NULL, 0}
};

DWORD key_name_to_vk(const char *name) {
    for (const KeyNameEntry *e = key_names; e->name; e++) {
        if (_stricmp(e->name, name) == 0) return e->vk;
    }
    if (name[0] && !name[1]) {
        return (DWORD)toupper((unsigned char)name[0]);
    }
    return 0;
}

const char *vk_to_key_name(DWORD vk) {
    for (const KeyNameEntry *e = key_names; e->name; e++) {
        if (e->vk == vk) return e->name;
    }
    return NULL;
}

Action action_name_to_enum(const char *name) {
    return api_action_from_name(name);
}

const char *action_enum_to_name(Action action) {
    return api_action_path(action);
}

DWORD mod_name_to_flag(const char *name) {
    if (_stricmp(name, "LWin")  == 0) return MOD_LWIN;
    if (_stricmp(name, "RWin")  == 0) return MOD_LWIN;
    if (_stricmp(name, "Win")   == 0) return MOD_LWIN;
    if (_stricmp(name, "Shift") == 0) return MOD_SHIFT;
    if (_stricmp(name, "Ctrl")  == 0) return MOD_CONTROL;
    if (_stricmp(name, "Alt")   == 0) return MOD_ALT;
    return 0;
}

KeyMap *keymap_new(const wchar_t *name, bool persist) {
    if (g.cfg.keymaps->count >= MAX_KEYMAPS) return NULL;

    KeyMap *km = &g.cfg.keymaps->maps[g.cfg.keymaps->count++];
    km->name     = _wcsdup(name);
    km->capacity = 64;
    km->bindings = (KeyBinding *)calloc((size_t)km->capacity, sizeof(KeyBinding));
    km->count    = 0;
    km->persist  = persist;
    km->exit_vk  = 0;
    return km;
}

void keymap_add_binding(KeyMap *map, DWORD mods, DWORD vk,
                        Action action, int arg, KeyMap *submap,
                        const wchar_t *command, const wchar_t *args,
                        const wchar_t *cwd, const wchar_t *desc,
                        bool terminal) {
    if (!map) return;

    if (map->count >= map->capacity) {
        int new_cap = map->capacity * 2;
        KeyBinding *grown = (KeyBinding *)realloc(
            map->bindings, (size_t)new_cap * sizeof(KeyBinding));
        if (!grown) return;
        map->bindings = grown;
        map->capacity = new_cap;
    }

    for (int i = 0; i < map->count; i++) {
        if (map->bindings[i].vk == vk && map->bindings[i].mod_flags == mods) {
            log_err(L"config: %ls: mods=0x%X vk=0x%02X is already bound — the "
                    L"later binding is SHADOWED and will never fire",
                    map->name ? map->name : L"?", (unsigned)mods, (unsigned)vk);
            break;
        }
    }

    KeyBinding *kb = &map->bindings[map->count++];
    kb->mod_flags = mods;
    kb->vk        = vk;
    kb->action    = action;
    kb->arg       = arg;
    kb->submap    = submap;
    kb->command   = command ? _wcsdup(command) : NULL;
    kb->args      = (args && args[0]) ? _wcsdup(args) : NULL;
    kb->cwd       = (cwd  && cwd[0])  ? _wcsdup(cwd)  : NULL;
    kb->desc      = (desc && desc[0]) ? _wcsdup(desc) : NULL;
    kb->terminal  = terminal;
}

KeyBinding *keymap_find(KeyMap *map, DWORD mods, DWORD vk) {
    if (!map) return NULL;
    for (int i = 0; i < map->count; i++) {
        KeyBinding *kb = &map->bindings[i];
        if (kb->vk == vk && kb->mod_flags == mods) return kb;
    }
    return NULL;
}
