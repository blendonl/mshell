#include "mshell.h"

#define TWEAK_BACKUP_KEY L"Software\\mshell\\TweakBackup"

typedef enum { TW_DWORD, TW_SZ } TweakType;

typedef struct {
    const wchar_t *group;
    const wchar_t *key;
    const wchar_t *value;
    TweakType      type;
    DWORD          dw;
    const wchar_t *sz;
    const wchar_t *why;
} Tweak;

static const Tweak s_tweaks[] = {
    { L"input", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
      L"NoWinKeys", TW_DWORD, 1, NULL,
      L"stop Explorer handling Win+* below our hook" },
    { L"input", L"Control Panel\\Desktop",
      L"LowLevelHooksTimeout", TW_DWORD, 1000, NULL,
      L"give the keyboard hook 1s before Windows drops it" },
    { L"input", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
      L"DisableLockWorkstation", TW_DWORD, 1, NULL,
      L"Win+L is detected below the hook and would latch the Win key" },
    { L"input", L"Control Panel\\Keyboard",
      L"PrintScreenKeyForSnippingEnabled", TW_DWORD, 0, NULL,
      L"give PrintScreen back so it can be bound" },
    { L"input", L"Control Panel\\Accessibility\\StickyKeys",
      L"Flags", TW_SZ, 0, L"506",
      L"no Sticky Keys prompt from five Shifts" },
    { L"input", L"Control Panel\\Accessibility\\Keyboard Response",
      L"Flags", TW_SZ, 0, L"122",
      L"no Filter Keys prompt from a held Shift" },
    { L"input", L"Control Panel\\Accessibility\\ToggleKeys",
      L"Flags", TW_SZ, 0, L"38",
      L"no Toggle Keys prompt" },

    { L"visual", L"Control Panel\\Desktop\\WindowMetrics",
      L"MinAnimate", TW_SZ, 0, L"0",
      L"no minimise/restore animation" },
    { L"visual", L"Control Panel\\Desktop",
      L"DragFullWindows", TW_SZ, 0, L"0", L"no drag-shadow repaint" },
    { L"visual", L"Control Panel\\Desktop",
      L"MenuShowDelay", TW_SZ, 0, L"0", L"menus open immediately" },
    { L"visual", L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
      L"TaskbarAnimations", TW_DWORD, 0, NULL, L"no taskbar animation" },
    { L"visual", L"Software\\Microsoft\\Windows\\DWM",
      L"EnableAeroPeek", TW_DWORD, 0, NULL, L"no Aero Peek" },
    { L"visual", L"Control Panel\\Desktop",
      L"WindowArrangementActive", TW_SZ, 0, L"0",
      L"no Aero Snap — mshell owns window placement" },

    { L"apps", L"Software\\Policies\\Google\\Chrome",
      L"NativeWindowOcclusionEnabled", TW_DWORD, 0, NULL,
      L"Chrome stops drawing a window it thinks is hidden, and stays stopped" },
    { L"apps", L"Software\\Policies\\Chromium",
      L"NativeWindowOcclusionEnabled", TW_DWORD, 0, NULL,
      L"the same, for a Chromium build" },
    { L"apps", L"Software\\Policies\\Microsoft\\Edge",
      L"NativeWindowOcclusionEnabled", TW_DWORD, 0, NULL,
      L"the same, for Edge" },

    { L"quiet", L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
      L"ToastEnabled", TW_DWORD, 0, NULL,
      L"there is no tray to show a toast in" },
    { L"quiet", L"Control Panel\\Sound",
      L"Beep", TW_SZ, 0, L"no",
      L"a shell that swallows keys would beep on every unbound Win+key" },
    { L"quiet", L"System\\GameConfigStore",
      L"GameDVR_Enabled", TW_DWORD, 0, NULL, L"Win+G is ours" },
};

#define TWEAK_COUNT ((int)(sizeof(s_tweaks) / sizeof(s_tweaks[0])))

static void backup_name(const Tweak *t, wchar_t *out, size_t cap) {
    _snwprintf(out, cap, L"%ls|%ls", t->key, t->value);
    out[cap - 1] = L'\0';
}

static void tweak_backup(const Tweak *t) {
    wchar_t name[512];
    backup_name(t, name, 512);

    HKEY bk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, TWEAK_BACKUP_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        NULL, &bk, NULL) != ERROR_SUCCESS)
        return;

    if (RegQueryValueExW(bk, name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        RegCloseKey(bk);
        return;
    }

    HKEY src;
    BYTE  buf[512];
    DWORD sz = sizeof(buf), type = 0;
    bool  had = false;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, t->key, 0, KEY_QUERY_VALUE, &src)
            == ERROR_SUCCESS) {
        had = RegQueryValueExW(src, t->value, NULL, &type, buf, &sz)
                == ERROR_SUCCESS;
        RegCloseKey(src);
    }

    if (had) {
        BYTE  rec[520];
        DWORD tt = type;
        memcpy(rec, &tt, sizeof(tt));
        memcpy(rec + sizeof(tt), buf, sz);
        RegSetValueExW(bk, name, 0, REG_BINARY, rec, sizeof(tt) + sz);
    } else {
        RegSetValueExW(bk, name, 0, REG_BINARY, NULL, 0);
    }
    RegCloseKey(bk);
}

static bool tweak_write(const Tweak *t) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, t->key, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &k, NULL)
            != ERROR_SUCCESS)
        return false;

    LONG r;
    if (t->type == TW_DWORD)
        r = RegSetValueExW(k, t->value, 0, REG_DWORD,
                           (const BYTE *)&t->dw, sizeof(t->dw));
    else
        r = RegSetValueExW(k, t->value, 0, REG_SZ, (const BYTE *)t->sz,
                           (DWORD)((wcslen(t->sz) + 1) * sizeof(wchar_t)));

    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static bool tweak_is_set(const Tweak *t) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, t->key, 0, KEY_QUERY_VALUE, &k)
            != ERROR_SUCCESS)
        return false;

    BYTE  buf[512];
    DWORD sz = sizeof(buf), type = 0;
    bool  ok = RegQueryValueExW(k, t->value, NULL, &type, buf, &sz)
                 == ERROR_SUCCESS;
    RegCloseKey(k);
    if (!ok) return false;

    if (t->type == TW_DWORD)
        return type == REG_DWORD && sz == sizeof(DWORD) &&
               *(const DWORD *)buf == t->dw;

    if (type != REG_SZ) return false;
    buf[sizeof(buf) - 2] = 0;
    buf[sizeof(buf) - 1] = 0;
    return wcscmp((const wchar_t *)buf, t->sz) == 0;
}

static bool tweak_has_backup(const Tweak *t) {
    wchar_t name[512];
    backup_name(t, name, 512);

    HKEY bk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TWEAK_BACKUP_KEY, 0, KEY_READ, &bk)
            != ERROR_SUCCESS)
        return false;

    bool have = RegQueryValueExW(bk, name, NULL, NULL, NULL, NULL)
                  == ERROR_SUCCESS;
    RegCloseKey(bk);
    return have;
}

static void tweak_backup_forget(const Tweak *t) {
    wchar_t name[512];
    backup_name(t, name, 512);

    HKEY bk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TWEAK_BACKUP_KEY, 0,
                      KEY_READ | KEY_WRITE, &bk) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(bk, name);
    RegCloseKey(bk);
}

static bool tweak_restore(const Tweak *t) {
    wchar_t name[512];
    backup_name(t, name, 512);

    HKEY bk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TWEAK_BACKUP_KEY, 0,
                      KEY_READ | KEY_WRITE, &bk) != ERROR_SUCCESS)
        return false;

    BYTE  rec[520];
    DWORD sz = sizeof(rec);
    if (RegQueryValueExW(bk, name, NULL, NULL, rec, &sz) != ERROR_SUCCESS) {
        RegCloseKey(bk);
        return false;
    }

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, t->key, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &k, NULL)
            == ERROR_SUCCESS) {
        if (sz == 0) {
            RegDeleteValueW(k, t->value);
        } else {
            DWORD type;
            memcpy(&type, rec, sizeof(type));
            RegSetValueExW(k, t->value, 0, type, rec + sizeof(type),
                           sz - (DWORD)sizeof(type));
        }
        RegCloseKey(k);
    }

    RegDeleteValueW(bk, name);
    RegCloseKey(bk);
    return true;
}

static bool group_matches(const wchar_t *want, const wchar_t *group) {
    return !want || !want[0] || _wcsicmp(want, L"all") == 0 ||
           _wcsicmp(want, group) == 0;
}

int tweaks_apply(const wchar_t *group) {
    int n = 0;
    for (int i = 0; i < TWEAK_COUNT; i++) {
        const Tweak *t = &s_tweaks[i];
        if (!group_matches(group, t->group)) continue;

        bool kept = tweak_has_backup(t);
        tweak_backup(t);
        if (tweak_write(t)) { n++; continue; }

        if (!kept) tweak_backup_forget(t);
        log_msg(LOG_WARN, L"tweak: could not set %ls\\%ls — needs an "
                          L"administrator prompt", t->key, t->value);
    }
    log_msg(LOG_INFO, L"tweaks: applied %d", n);
    return n;
}

int tweaks_revert(const wchar_t *group) {
    int n = 0;
    for (int i = 0; i < TWEAK_COUNT; i++) {
        const Tweak *t = &s_tweaks[i];
        if (!group_matches(group, t->group)) continue;
        if (tweak_restore(t)) n++;
    }
    log_msg(LOG_INFO, L"tweaks: reverted %d", n);
    return n;
}

void tweaks_list(void) {
    console_print("group    state    setting\n");
    console_print("-------- -------- ---------------------------------------\n");

    int failed = 0;

    for (int i = 0; i < TWEAK_COUNT; i++) {
        const Tweak *t = &s_tweaks[i];

        const char *state = "-";
        if (tweak_is_set(t))          state = "applied";
        else if (tweak_has_backup(t)) { state = "failed"; failed++; }

        char line[512];
        char why[256];
        WideCharToMultiByte(CP_UTF8, 0, t->why, -1, why, 256, NULL, NULL);
        char grp[32];
        WideCharToMultiByte(CP_UTF8, 0, t->group, -1, grp, 32, NULL, NULL);

        _snprintf(line, 512, "%-8s %-8s %s\n", grp, state, why);
        line[511] = '\0';
        console_print(line);
    }

    if (failed)
        console_print("\n'failed' means mshell recorded the old value but could "
                      "not write the new one.\nRe-run --tweaks apply from an "
                      "administrator prompt.\n");
}

void tweaks_emit_reg(const wchar_t *group, bool undo) {
    console_print("Windows Registry Editor Version 5.00\r\n\r\n");

    for (int i = 0; i < TWEAK_COUNT; i++) {
        const Tweak *t = &s_tweaks[i];
        if (!group_matches(group, t->group)) continue;

        char key[512], val[128], why[256];
        WideCharToMultiByte(CP_UTF8, 0, t->key,   -1, key, 512, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, t->value, -1, val, 128, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, t->why,   -1, why, 256, NULL, NULL);

        char buf[1200];
        if (undo) {
            _snprintf(buf, 1200, "; %s\r\n[HKEY_CURRENT_USER\\%s]\r\n"
                                 "\"%s\"=-\r\n\r\n", why, key, val);
        } else if (t->type == TW_DWORD) {
            _snprintf(buf, 1200, "; %s\r\n[HKEY_CURRENT_USER\\%s]\r\n"
                                 "\"%s\"=dword:%08lx\r\n\r\n",
                      why, key, val, (unsigned long)t->dw);
        } else {
            char sz[128];
            WideCharToMultiByte(CP_UTF8, 0, t->sz, -1, sz, 128, NULL, NULL);
            _snprintf(buf, 1200, "; %s\r\n[HKEY_CURRENT_USER\\%s]\r\n"
                                 "\"%s\"=\"%s\"\r\n\r\n", why, key, val, sz);
        }
        buf[1199] = '\0';
        console_print(buf);
    }
}
