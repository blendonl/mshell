/* ===========================================================================
 * tweaks.c — registry tweaks that can actually be undone.
 *
 * THE PROBLEM WITH THE .reg FILES.
 *
 * mshell ships harden.reg / debloat.reg / services.reg with an `-undo`
 * counterpart each. Those undo files contain WINDOWS' DOCUMENTED DEFAULTS, not
 * a snapshot of what you had — debloat-undo.reg says so in its own header. So
 * three things were true:
 *
 *   - Reverting is lossy. If you had customised any of these, the undo silently
 *     replaced your value with Microsoft's.
 *   - It is all-or-nothing. A .reg file is imported whole; there is no way to
 *     take the keyboard-shortcut tweaks and leave the animation ones.
 *   - Nothing recorded what was applied. uninstall.bat reverts `harden` because
 *     install.bat applied it, and simply cannot know about the other two.
 *
 * This replaces that with a table: each tweak names its key, value and desired
 * data, and applying one READS THE EXISTING VALUE FIRST and files it in a
 * backup key under HKCU\Software\mshell\TweakBackup. Reverting restores exactly
 * what was there, including "the value did not exist", which is a state a .reg
 * file cannot express at all.
 *
 * The .reg files stay, generated from this same table by `make regs`, so the
 * two can no longer drift and someone who prefers double-clicking a file still
 * can.
 *
 * SCOPE. Only HKCU tweaks are applied in-process. The machine-wide service
 * changes in services.reg need administrator rights, and mshell is manifested
 * asInvoker and does not elevate itself — 0.8.0 made that a deliberate
 * property. Those stay a .reg file you import deliberately.
 * =========================================================================== */
#include "mshell.h"

#define TWEAK_BACKUP_KEY L"Software\\mshell\\TweakBackup"

typedef enum { TW_DWORD, TW_SZ } TweakType;

typedef struct {
    const wchar_t *group;   /* "input", "visual", … — what you enable         */
    const wchar_t *key;     /* under HKCU                                     */
    const wchar_t *value;
    TweakType      type;
    DWORD          dw;
    const wchar_t *sz;
    const wchar_t *why;     /* shown by --tweaks list; this is the whole point */
} Tweak;

/* Every tweak mshell knows how to apply. Grouped so a user can take the ones
 * that matter to a keyboard shell and leave the cosmetic ones. */
static const Tweak s_tweaks[] = {
    /* --- input: shortcuts the hook cannot reach ------------------------- */
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

    /* --- visual: the animations a tiling WM fights ---------------------- */
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

    /* --- apps: making applications survive being taken off the screen ---
     *
     * Chromium decides for itself whether anyone can see each of its windows —
     * "native window occlusion" — and throttles or stops presenting the ones it
     * believes are hidden. A tiling shell takes windows off the screen
     * constantly, and every mechanism there is says "hidden" to that
     * calculation: cloaked, SW_HIDE'd, moved off every display, or simply
     * covered by the window you are looking at.
     *
     * Coming back is where it breaks. Measured on Chrome 151, tiled full
     * screen, hidden and shown by desktop switches: the window comes back with
     * its frame painted and nothing inside it — the whole window one flat
     * #202020 — and stays that way. Not a repaint that was missed: an
     * asynchronous RedrawWindow, a synchronous one (RDW_UPDATENOW), a 1px move,
     * a real resize, SetForegroundWindow, SwitchToThisWindow and a full
     * minimise/restore were each tried on such a window and none of them
     * brought a single pixel back. The browser process, its GPU process and its
     * renderers were all alive and the window answered WM_NULL. Only restarting
     * the browser fixed it.
     *
     * So the fix is to stop it deciding. This is the same workaround every
     * other Windows tiling WM ends up recommending, and there is a policy for
     * it, which beats a command-line flag because it applies however the
     * browser was started.
     *
     * NEEDS AN ADMINISTRATOR PROMPT, and there is nothing mshell can do about
     * that: HKCU\Software\Policies is ACL'd read-only for the user who owns the
     * hive, exactly like the Explorer policy keys in the input group. Without
     * elevation this group is skipped and reported, and the no-admin route is
     * the flag — see INSTALL.md:
     *
     *     chrome.exe --disable-features=CalculateNativeWinOcclusion
     *
     * Electron apps have no policy at all and need that flag either way. */
    { L"apps", L"Software\\Policies\\Google\\Chrome",
      L"NativeWindowOcclusionEnabled", TW_DWORD, 0, NULL,
      L"Chrome stops drawing a window it thinks is hidden, and stays stopped" },
    { L"apps", L"Software\\Policies\\Chromium",
      L"NativeWindowOcclusionEnabled", TW_DWORD, 0, NULL,
      L"the same, for a Chromium build" },
    { L"apps", L"Software\\Policies\\Microsoft\\Edge",
      L"NativeWindowOcclusionEnabled", TW_DWORD, 0, NULL,
      L"the same, for Edge" },

    /* --- quiet: notifications and sounds a shell with no tray cannot show */
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

/* ---------------------------------------------------------------------------
 * Backup store. One value per tweak, named "<key>|<value>", holding what was
 * there before — or absent, which is how "there was nothing here" is recorded.
 * --------------------------------------------------------------------------- */
static void backup_name(const Tweak *t, wchar_t *out, size_t cap) {
    _snwprintf(out, cap, L"%ls|%ls", t->key, t->value);
    out[cap - 1] = L'\0';
}

/* Save the CURRENT value, once. Never overwrites an existing backup: applying a
 * tweak twice must not record the tweaked value as the original. */
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
        return;                              /* already backed up */
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
        /* Store the type alongside the bytes so the restore writes it back as
         * whatever it actually was — some of these are REG_SZ on one install
         * and REG_DWORD on another. */
        BYTE  rec[520];
        DWORD tt = type;
        memcpy(rec, &tt, sizeof(tt));
        memcpy(rec + sizeof(tt), buf, sz);
        RegSetValueExW(bk, name, 0, REG_BINARY, rec, sizeof(tt) + sz);
    } else {
        /* Zero-length marker: "this value did not exist", which is exactly the
         * state a .reg undo file cannot express. */
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
        return false;                        /* never applied */
    }

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, t->key, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &k, NULL)
            == ERROR_SUCCESS) {
        if (sz == 0) {
            RegDeleteValueW(k, t->value);    /* it did not exist before */
        } else {
            DWORD type;
            memcpy(&type, rec, sizeof(type));
            RegSetValueExW(k, t->value, 0, type, rec + sizeof(type),
                           sz - (DWORD)sizeof(type));
        }
        RegCloseKey(k);
    }

    RegDeleteValueW(bk, name);               /* no longer applied */
    RegCloseKey(bk);
    return true;
}

/* ---------------------------------------------------------------------------
 * Public surface, driven by --tweaks
 * --------------------------------------------------------------------------- */
static bool group_matches(const wchar_t *want, const wchar_t *group) {
    return !want || !want[0] || _wcsicmp(want, L"all") == 0 ||
           _wcsicmp(want, group) == 0;
}

int tweaks_apply(const wchar_t *group) {
    int n = 0;
    for (int i = 0; i < TWEAK_COUNT; i++) {
        const Tweak *t = &s_tweaks[i];
        if (!group_matches(group, t->group)) continue;
        tweak_backup(t);                     /* read BEFORE write */
        if (tweak_write(t)) n++;
        else log_msg(LOG_WARN, L"tweak: could not set %ls\\%ls",
                     t->key, t->value);
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

/* What is applied right now, which nothing previously recorded. */
void tweaks_list(void) {
    HKEY bk;
    bool have_backup = RegOpenKeyExW(HKEY_CURRENT_USER, TWEAK_BACKUP_KEY, 0,
                                     KEY_READ, &bk) == ERROR_SUCCESS;

    console_print("group    state    setting\n");
    console_print("-------- -------- ---------------------------------------\n");

    for (int i = 0; i < TWEAK_COUNT; i++) {
        const Tweak *t = &s_tweaks[i];

        bool applied = false;
        if (have_backup) {
            wchar_t name[512];
            backup_name(t, name, 512);
            applied = RegQueryValueExW(bk, name, NULL, NULL, NULL, NULL)
                        == ERROR_SUCCESS;
        }

        char line[512];
        char why[256];
        WideCharToMultiByte(CP_UTF8, 0, t->why, -1, why, 256, NULL, NULL);
        char grp[32];
        WideCharToMultiByte(CP_UTF8, 0, t->group, -1, grp, 32, NULL, NULL);

        _snprintf(line, 512, "%-8s %-8s %s\n", grp,
                  applied ? "applied" : "-", why);
        line[511] = '\0';
        console_print(line);
    }

    if (have_backup) RegCloseKey(bk);
}

/* Emit the .reg files from this same table, so the shipped files and the
 * in-process implementation cannot drift. Driven by `make regs`. */
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
            /* Deleting the value is the honest undo for a generated file: it
             * restores whatever Windows defaults to rather than asserting a
             * value the user may never have had. The in-process revert does
             * better, because it has the backup. */
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
