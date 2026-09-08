#include "mshell.h"
#include "keys.h"

static CRITICAL_SECTION g_kb_cs;
static bool             g_kb_cs_ready = false;
static HANDLE           g_kb_thread   = NULL;
static DWORD            g_kb_thread_id = 0;
static HANDLE           g_kb_ready_evt = NULL;

void kb_locks_init(void) {
    if (!g_kb_cs_ready) {
        InitializeCriticalSection(&g_kb_cs);
        g_kb_cs_ready = true;
    }
}
void kb_lock(void)   { if (g_kb_cs_ready) EnterCriticalSection(&g_kb_cs); }
void kb_unlock(void) { if (g_kb_cs_ready) LeaveCriticalSection(&g_kb_cs); }
static bool mod_lwin, mod_shift, mod_ctrl, mod_alt;

static bool mod_lshift, mod_rshift, mod_lctrl, mod_rctrl, mod_lalt, mod_ralt;

static int s_count;

static bool win_used;

static DWORD current_mods(void) {
    DWORD m = 0;
    if (mod_lwin)  m |= MOD_LWIN;
    if (mod_shift) m |= MOD_SHIFT;
    if (mod_ctrl)  m |= MOD_CONTROL;
    if (mod_alt)   m |= MOD_ALT;
    return m;
}

static KeyMap *g_wk_last_map = (KeyMap *)-1;
static void notify_submap(void) {
    KeyMap *m = (g.current_map && g.current_map != g.root_map) ? g.current_map : NULL;
    if (m == g_wk_last_map) return;
    g_wk_last_map = m;
    if (g.message_window)
        PostMessageW(g.message_window, WM_MSHELL_SUBMAP, 0, (LPARAM)m);
}

void kb_reset_state(void) {
    mod_lwin = mod_shift = mod_ctrl = mod_alt = false;
    mod_lshift = mod_rshift = mod_lctrl = mod_rctrl = false;
    mod_lalt = mod_ralt = false;
    win_used = false;
    kb_lock();
    s_count = 0;
    g.current_map = g.root_map;
    notify_submap();
    kb_unlock();
}

#define KB_PENDING_N 64

typedef struct {
    unsigned seq;
    unsigned gen;
    Action   action;
    int      arg;
    wchar_t  command[MAX_PATH];
    wchar_t  args[SPAWN_ARGS_MAX];
    wchar_t  cwd[MAX_PATH];
    int      count;
} PendingAction;

static void pend_copy(wchar_t *dst, size_t cap, const wchar_t *src) {
    if (!cap) return;
    if (!src) { dst[0] = L'\0'; return; }
    size_t n = wcslen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n * sizeof(wchar_t));
    dst[n] = L'\0';
}

static PendingAction s_pending[KB_PENDING_N];
static unsigned      s_pend_seq = 1;

static void dispatch(KeyBinding *b, DWORD vk, DWORD mods) {
    if (!g.message_window || !b) return;

    unsigned       seq = s_pend_seq++;
    PendingAction *p   = &s_pending[seq & (KB_PENDING_N - 1)];

    p->action = b->action;
    p->arg    = b->arg;
    p->gen    = g.config_gen;
    pend_copy(p->command, MAX_PATH,        b->command);
    pend_copy(p->args,    SPAWN_ARGS_MAX,  b->args);
    pend_copy(p->cwd,     MAX_PATH,        b->cwd);
    p->count  = s_count;
    s_count   = 0;
    p->seq = seq;

    PostMessageW(g.message_window, WM_MSHELL_ACTION,
                 (WPARAM)((vk & 0xFFFF) | ((mods & 0xFFFF) << 16)),
                 (LPARAM)seq);
}

bool kb_take_pending(unsigned seq, Action *action, int *arg,
                     wchar_t *cmd, size_t cmd_cap,
                     wchar_t *args, size_t args_cap,
                     wchar_t *cwd, size_t cwd_cap, int *count) {
    bool ok = false;

    kb_lock();
    PendingAction *p = &s_pending[seq & (KB_PENDING_N - 1)];
    bool stale_lua = false;
    if (p->seq == seq) {
        if (p->action == ACTION_LUA_CALL && p->gen != g.config_gen) {
            stale_lua = true;
        } else {
            *action = p->action;
            *arg    = p->arg;
            pend_copy(cmd,  cmd_cap,  p->command);
            pend_copy(args, args_cap, p->args);
            pend_copy(cwd,  cwd_cap,  p->cwd);
            *count = p->count;
            ok = true;
        }
    }
    kb_unlock();

    if (stale_lua)
        log_w(L"input: dropped a Lua keybind queued before the config reload");
    else if (!ok)
        log_err(L"input: dropped a keybind action — more than %d were queued "
                L"before the main thread could run them", KB_PENDING_N);
    return ok;
}

LRESULT CALLBACK kb_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
    DWORD vk = kb->vkCode;

    if (kb->dwExtraInfo == MSHELL_INPUT_TAG)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    if (g.panicked)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    if (g.launcher_open) {
        if (!down) return 1;

        switch (vk) {
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
        case VK_LMENU: case VK_RMENU: case VK_MENU:
            break;
        default: {
            BYTE   ks[256];
            wchar_t buf[8] = {0};
            wchar_t ch = 0;
            if (GetKeyboardState(ks)) {
                ks[VK_SHIFT] = mod_shift ? 0x80 : 0;
                int n = ToUnicodeEx(vk, (UINT)((lParam >> 16) & 0xFF), ks,
                                    buf, 8, 0, GetKeyboardLayout(0));
                if (n == 1) ch = buf[0];
            }
            PostMessageW(g.message_window, WM_MSHELL_CAPTURE_KEY,
                         (WPARAM)vk, (LPARAM)ch);
            return 1;
        }
        }
    }

    if (down && mod_lwin && vk != VK_LWIN && vk != VK_RWIN)
        win_used = true;

    switch (vk) {
    case VK_LWIN: case VK_RWIN:
        if (down) {
            if (!mod_lwin) win_used = false;
            mod_lwin = true;
            return 1;
        }
        mod_lwin = false;
        kb_lock();
        if (!win_used) {
            if (g.current_map == g.root_map) {
                if (g.leader_map) g.current_map = g.leader_map;
            } else {
                g.current_map = g.root_map;
            }
            s_count = 0;
        }
        notify_submap();
        kb_unlock();
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_LSHIFT:   mod_lshift = down; mod_shift = mod_lshift || mod_rshift;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_RSHIFT:   mod_rshift = down; mod_shift = mod_lshift || mod_rshift;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_SHIFT:    mod_shift = down;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);

    case VK_LCONTROL: mod_lctrl = down; mod_ctrl = mod_lctrl || mod_rctrl;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_RCONTROL: mod_rctrl = down; mod_ctrl = mod_lctrl || mod_rctrl;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_CONTROL:  mod_ctrl = down;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);

    case VK_LMENU:    mod_lalt = down; mod_alt = mod_lalt || mod_ralt;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_RMENU:    mod_ralt = down; mod_alt = mod_lalt || mod_ralt;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_MENU:     mod_alt = down;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    if (!down)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    DWORD mods = current_mods();

    LRESULT result = 1;
    bool    pass   = false;

    kb_lock();

    KeyMap *map   = g.current_map ? g.current_map : g.root_map;
    bool    modal = (map != g.root_map);

    if (modal) {
        DWORD kmods   = mods & ~MOD_LWIN;
        DWORD exit_vk = map->exit_vk ? map->exit_vk : VK_ESCAPE;
        KeyBinding *b = keymap_find(map, kmods, vk);

        if (!b && kmods == 0 && vk >= '0' && vk <= '9' &&
            (s_count > 0 || vk > '0')) {
            int d = (int)(vk - '0');
            s_count = (s_count > 99) ? 999 : s_count * 10 + d;
            goto done;
        }

        if (map->persist) {
            if (vk == exit_vk) {
                g.current_map = g.root_map;
                s_count = 0;
            } else if (b) {
                if (b->action == ACTION_ENTER_SUBMAP) {
                    g.current_map = b->submap;
                } else {
                    dispatch(b, vk, mods);
                    if (b->terminal) g.current_map = g.root_map;
                }
            }
        } else {
            if (b && b->action == ACTION_ENTER_SUBMAP) {
                g.current_map = b->submap;
            } else {
                if (b) dispatch(b, vk, mods);
                g.current_map = g.root_map;
            }
        }
        goto done;
    }

    if (g.block_system_keys) {
        bool ctrl_shift_esc = (vk == VK_ESCAPE) && mod_ctrl && mod_shift;
        if (!ctrl_shift_esc) {
            if (mod_alt && (vk == VK_TAB ||
                            vk == VK_ESCAPE ||
                            vk == VK_SPACE))
                goto done;
            if (mod_ctrl && vk == VK_ESCAPE)
                goto done;
        }
    }

    if (!mod_lwin) { pass = true; goto done; }

    {
        KeyBinding *b = keymap_find(map, mods, vk);
        if (b) {
            if (b->action == ACTION_ENTER_SUBMAP) {
                g.current_map = b->submap;
            } else {
                dispatch(b, vk, mods);
                if (b->terminal) g.current_map = g.root_map;
            }
        }
    }

done:
    notify_submap();
    kb_unlock();
    if (pass)
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    return result;
}
static LRESULT CALLBACK mouse_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    if (!g.mod_drag_hwnd && !mod_lwin)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;
    if (mouse_mod_drag_event(wParam, ms->pt, mod_lwin))
        return 1;

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static void mouse_sync_hook_here(void) {
    if (g.mouse_mod_drag && !g.mouse_hook) {
        g.mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc,
                                         g.hinst, 0);
        if (g.mouse_hook) log_msg(LOG_INFO, L"mouse: Mod+drag on");
        else log_msg(LOG_WARN, L"SetWindowsHookEx(WH_MOUSE_LL) failed: %lu",
                     GetLastError());
    } else if (!g.mouse_mod_drag && g.mouse_hook) {
        UnhookWindowsHookEx(g.mouse_hook);
        g.mouse_hook    = NULL;
        g.mod_drag_hwnd = NULL;
        log_msg(LOG_INFO, L"mouse: Mod+drag off");
    }
}

void mouse_sync_hook(void) {
    if (g_kb_thread_id) PostThreadMessageW(g_kb_thread_id, WM_MSHELL_MOUSE, 0, 0);
}

static DWORD WINAPI kb_thread_proc(LPVOID param) {
    (void)param;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    g.kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook_proc, g.hinst, 0);

    if (g_kb_ready_evt) SetEvent(g_kb_ready_evt);

    if (!g.kb_hook) {
        log_w(L"SetWindowsHookEx(WH_KEYBOARD_LL) failed: %lu", GetLastError());
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == WM_MSHELL_MOUSE) {
            mouse_sync_hook_here();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.mouse_hook) {
        UnhookWindowsHookEx(g.mouse_hook);
        g.mouse_hook = NULL;
    }
    if (g.kb_hook) {
        UnhookWindowsHookEx(g.kb_hook);
        g.kb_hook = NULL;
    }
    return 0;
}

bool kb_init(void) {
    kb_locks_init();

    g_kb_ready_evt = CreateEventW(NULL, TRUE, FALSE, NULL);

    g_kb_thread = CreateThread(NULL, 0, kb_thread_proc, NULL, 0, &g_kb_thread_id);
    if (!g_kb_thread) {
        log_w(L"kb_init: CreateThread failed: %lu", GetLastError());
        return false;
    }

    if (g_kb_ready_evt)
        WaitForSingleObject(g_kb_ready_evt, 5000);

    return g.kb_hook != NULL;
}

void kb_shutdown(void) {
    if (g_kb_thread) {
        PostThreadMessageW(g_kb_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(g_kb_thread, 2000);
        CloseHandle(g_kb_thread);
        g_kb_thread = NULL;
    }
    if (g.kb_hook) {
        UnhookWindowsHookEx(g.kb_hook);
        g.kb_hook = NULL;
    }
    if (g_kb_ready_evt) {
        CloseHandle(g_kb_ready_evt);
        g_kb_ready_evt = NULL;
    }
    if (g_kb_cs_ready) {
        DeleteCriticalSection(&g_kb_cs);
        g_kb_cs_ready = false;
    }
}
