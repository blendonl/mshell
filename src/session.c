/*
 * session.c — remember per-desktop settings across a restart.
 *
 * WHAT IS SAVED, and why not more. Layouts, master ratios and master counts are
 * keyed by desktop NAME, which survives anything: a desktop is its name, and
 * the same name means the same desktop next time.
 *
 * Window→desktop assignments are deliberately NOT saved. A window is an HWND,
 * which is meaningless across a restart, so restoring the arrangement would
 * mean guessing from process names and titles — and guessing wrong puts your
 * windows somewhere you did not leave them, which is worse than putting them
 * where they open. Better to do the reliable part well.
 *
 * WHEN IT IS SAVED matters as much as what. Writing only at shutdown would miss
 * the case this exists for: install.bat upgrades by `taskkill /F`, which gives
 * the old process no chance to run anything. So it saves whenever a value
 * changes — all user-speed events writing a few hundred bytes — and a
 * kill -9 costs nothing.
 *
 * The format is one record per line, parsed by hand. Not Lua: this file is
 * written by mshell, and handing a program's own state back through an
 * interpreter that can execute arbitrary code is a needless liability.
 */

#include "mshell.h"

/* Remembered settings, keyed by name. Applied in desktop_apply_rules AFTER the
 * config's rules, so a value you changed at runtime beats the rule's default —
 * but a rule you edit still wins on the next reload, because reloading rewrites
 * the rules and re-applies them over this. */
typedef struct {
    wchar_t name[DESKTOP_NAME_MAX];
    Layout  layout;
    float   master_ratio;
    int     n_master;
} SessionDesktop;

static SessionDesktop s_saved[MAX_DESKTOPS];
static int            s_saved_count;
static wchar_t        s_saved_current[DESKTOP_NAME_MAX];
static bool           s_loaded;

/* %APPDATA%\mshell\session.txt — beside the config, which is where the rest of
 * mshell's per-user state already lives. */
static bool session_path(wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH];
    const wchar_t *slash = wcsrchr(g.config_path, L'\\');
    if (!slash) return false;

    size_t len = (size_t)(slash - g.config_path);
    if (len >= MAX_PATH) return false;
    memcpy(dir, g.config_path, len * sizeof(wchar_t));
    dir[len] = L'\0';

    int n = _snwprintf(out, cap, L"%ls\\%ls", dir, SESSION_FILE);
    return n > 0 && (size_t)n < cap;
}

/* ===========================================================================
 * Load
 * =========================================================================== */
void session_load(void) {
    s_saved_count      = 0;
    s_saved_current[0] = L'\0';
    s_loaded           = true;

    wchar_t path[MAX_PATH];
    if (!session_path(path, MAX_PATH)) return;

    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return;   /* first run — nothing to restore, not an error */

    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        wchar_t name[DESKTOP_NAME_MAX] = {0}, layout[32] = {0};
        int     ratio_pct = 0, nmaster = 0;

        if (swscanf(line, L"current %63ls", name) == 1) {
            wcsncpy(s_saved_current, name, DESKTOP_NAME_MAX - 1);
            s_saved_current[DESKTOP_NAME_MAX - 1] = L'\0';
            continue;
        }

        if (swscanf(line, L"desktop %63ls %31ls %d %d",
                    name, layout, &ratio_pct, &nmaster) == 4) {
            if (s_saved_count >= MAX_DESKTOPS) continue;
            if (!desktop_name_ok(name))        continue;

            SessionDesktop *d = &s_saved[s_saved_count];
            wcsncpy(d->name, name, DESKTOP_NAME_MAX - 1);
            d->name[DESKTOP_NAME_MAX - 1] = L'\0';

            /* Layout names go through the same table the config uses, so a
             * file written by a future version naming a layout this build does
             * not have is ignored rather than misread as layout 0. */
            char layout_u8[32];
            WideCharToMultiByte(CP_UTF8, 0, layout, -1, layout_u8, 32, NULL, NULL);
            d->layout = LAYOUT_COUNT;
            for (int l = 0; l < LAYOUT_COUNT; l++)
                if (strcmp(layout_u8, layout_to_name((Layout)l)) == 0)
                    d->layout = (Layout)l;
            if (d->layout == LAYOUT_COUNT) continue;

            d->master_ratio = clamp_f((float)ratio_pct / 100.f, 0.2f, 0.9f);
            d->n_master     = clamp_i(nmaster, 1, 20);
            s_saved_count++;
        }
    }
    fclose(f);

    log_w(L"session: restored settings for %d desktop(s)", s_saved_count);
}

/* Apply anything remembered for this desktop. Called from desktop_apply_rules,
 * after the config's rules have set the baseline. */
void session_apply(Desktop *dt) {
    if (!s_loaded || !dt) return;

    for (int i = 0; i < s_saved_count; i++) {
        if (_wcsicmp(s_saved[i].name, dt->name) != 0) continue;
        dt->layout       = s_saved[i].layout;
        dt->master_ratio = s_saved[i].master_ratio;
        dt->n_master     = s_saved[i].n_master;
        return;
    }
}

/* The desktop to start on: the one you were last on, if it was recorded.
 * Returns NULL when there is nothing to restore, and the caller falls back to
 * the desktop rule that claimed `default`. Outranked by `default = "always"`,
 * which is desktop_init's call to make, not ours — the session is recorded the
 * same either way. */
const wchar_t *session_start_desktop(void) {
    return s_saved_current[0] ? s_saved_current : NULL;
}

/* ===========================================================================
 * Save
 *
 * Called from every place a saved value can change. Each is a user-speed event
 * writing a few hundred bytes, so there is no debouncing to get wrong — and
 * saving eagerly is the whole point, since the upgrade path kills the process
 * outright.
 * =========================================================================== */
void session_save(void) {
    if (!s_loaded) return;   /* don't write before we've read */

    wchar_t path[MAX_PATH];
    if (!session_path(path, MAX_PATH)) return;

    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return;

    fwprintf(f, L"# mshell session state — written automatically, safe to delete\n");

    const Desktop *cur = desktop_current();
    if (cur && cur->name[0]) fwprintf(f, L"current %ls\n", cur->name);

    /* Live desktops first... */
    for (int i = 0; i < g.desktop_count; i++) {
        const Desktop *d = &g.desktops[i];
        fwprintf(f, L"desktop %ls %hs %d %d\n", d->name,
                 layout_to_name(d->layout),
                 (int)(d->master_ratio * 100.f + 0.5f), d->n_master);
    }

    /* ...then anything we remembered that isn't alive right now. A desktop you
     * are not currently using has not forgotten its layout; dropping it here
     * would quietly erase settings just because the desktop was empty when you
     * happened to shut down. */
    for (int i = 0; i < s_saved_count; i++) {
        if (desktop_slot_by_name(s_saved[i].name) >= 0) continue;
        fwprintf(f, L"desktop %ls %hs %d %d\n", s_saved[i].name,
                 layout_to_name(s_saved[i].layout),
                 (int)(s_saved[i].master_ratio * 100.f + 0.5f),
                 s_saved[i].n_master);
    }

    fclose(f);
}
