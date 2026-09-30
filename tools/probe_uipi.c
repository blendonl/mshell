#include <windows.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

#define PROBE_TAG ((ULONG_PTR)0x75697069)
#define RING      2048
#define LINE      320
#define MAX_PIDS  128
#define MAX_HK    96

typedef struct {
    DWORD pid;
    DWORD rid;
    DWORD uiaccess;
    DWORD err;
} PidInfo;

typedef struct {
    int    id;
    UINT   mods;
    UINT   vk;
} Hotkey;

enum { MODE_MSHELL, MODE_PASS };

static CRITICAL_SECTION g_log_cs;
static HANDLE           g_log_evt;
static wchar_t          g_ring[RING][LINE];
static unsigned         g_head, g_tail;
static volatile LONG    g_log_done;
static LARGE_INTEGER    g_freq, g_t0;

static PidInfo g_pids[MAX_PIDS];
static int     g_npids;
static DWORD   g_self_rid;

static int    g_mode = MODE_MSHELL;
static bool   g_stall;
static bool   g_win_down;
static bool   g_in_submap;
static DWORD  g_main_tid;
static Hotkey g_root[8];
static int    g_nroot;
static Hotkey g_sub[MAX_HK];
static int    g_nsub;

static int g_hk_blind, g_hk_visible, g_hook_downs, g_hook_swallowed;
static int g_win_downs_seen, g_win_ups_seen;

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void emit(const wchar_t *fmt, ...) {
    wchar_t buf[LINE];
    int n = _snwprintf(buf, LINE, L"%10.3f  ", now_ms());
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf + n, LINE - n, fmt, ap);
    va_end(ap);
    buf[LINE - 1] = L'\0';

    EnterCriticalSection(&g_log_cs);
    if (g_head - g_tail < RING) {
        wcscpy(g_ring[g_head % RING], buf);
        g_head++;
    }
    LeaveCriticalSection(&g_log_cs);
    SetEvent(g_log_evt);
}

static bool drain_one(wchar_t *out) {
    bool have = false;
    EnterCriticalSection(&g_log_cs);
    if (g_tail != g_head) {
        wcscpy(out, g_ring[g_tail % RING]);
        g_tail++;
        have = true;
    }
    LeaveCriticalSection(&g_log_cs);
    return have;
}

static DWORD WINAPI printer_thread(LPVOID unused) {
    (void)unused;
    wchar_t line[LINE];
    for (;;) {
        WaitForSingleObject(g_log_evt, 200);
        while (drain_one(line)) wprintf(L"%ls\n", line);
        fflush(stdout);
        if (g_log_done) {
            while (drain_one(line)) wprintf(L"%ls\n", line);
            fflush(stdout);
            return 0;
        }
    }
}

static const wchar_t *rid_name(DWORD rid) {
    if (rid == (DWORD)-1)  return L"?";
    if (rid >= 0x4000)     return L"System";
    if (rid >= 0x3000)     return L"High";
    if (rid >  0x2000)     return L"Medium+";
    if (rid == 0x2000)     return L"Medium";
    if (rid >= 0x1000)     return L"Low";
    return L"Untrusted";
}

static void token_query(HANDLE proc, DWORD *rid, DWORD *uiaccess, DWORD *err) {
    *rid = (DWORD)-1;
    *uiaccess = 0;
    *err = 0;

    HANDLE tok = NULL;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
        *err = GetLastError();
        return;
    }

    BYTE  buf[128];
    DWORD len = 0;
    if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof buf, &len)) {
        TOKEN_MANDATORY_LABEL *tml = (TOKEN_MANDATORY_LABEL *)buf;
        PUCHAR cnt = GetSidSubAuthorityCount(tml->Label.Sid);
        *rid = *GetSidSubAuthority(tml->Label.Sid, (DWORD)(*cnt - 1));
    } else {
        *err = GetLastError();
    }

    DWORD ui = 0;
    if (GetTokenInformation(tok, TokenUIAccess, &ui, sizeof ui, &len))
        *uiaccess = ui;

    CloseHandle(tok);
}

static PidInfo pid_info(DWORD pid) {
    for (int i = 0; i < g_npids; i++)
        if (g_pids[i].pid == pid) return g_pids[i];

    PidInfo pi = { pid, (DWORD)-1, 0, 0 };
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        token_query(h, &pi.rid, &pi.uiaccess, &pi.err);
        CloseHandle(h);
    } else {
        pi.err = GetLastError();
    }

    if (g_npids < MAX_PIDS) g_pids[g_npids++] = pi;
    return pi;
}

static bool window_blind(HWND hwnd, PidInfo *out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    PidInfo pi = pid_info(pid);
    if (out) *out = pi;
    if (pi.rid == (DWORD)-1) return pi.err == ERROR_ACCESS_DENIED;
    return pi.rid > g_self_rid;
}

static void describe_window(HWND hwnd, wchar_t *out, size_t cap) {
    if (!hwnd) {
        _snwprintf(out, cap, L"(none)");
        out[cap - 1] = L'\0';
        return;
    }
    wchar_t cls[64] = L"";
    GetClassNameW(hwnd, cls, 64);
    PidInfo pi;
    bool blind = window_blind(hwnd, &pi);
    _snwprintf(out, cap, L"%ls[%ls%ls%ls]", cls, rid_name(pi.rid),
               pi.uiaccess ? L",uiAccess" : L"", blind ? L",BLIND" : L"");
    out[cap - 1] = L'\0';
}

static const wchar_t *exe_base(DWORD pid, wchar_t *buf, DWORD cap) {
    buf[0] = L'\0';
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        DWORD n = cap;
        if (!QueryFullProcessImageNameW(h, 0, buf, &n)) buf[0] = L'\0';
        CloseHandle(h);
    }
    const wchar_t *s = wcsrchr(buf, L'\\');
    return s ? s + 1 : buf;
}

static DWORD try_setpos_noop(HWND hwnd) {
    SetLastError(0);
    if (SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER))
        return 0;
    DWORD e = GetLastError();
    return e ? e : (DWORD)-1;
}

static DWORD try_post(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SetLastError(0);
    if (PostMessageW(hwnd, msg, wp, lp)) return 0;
    DWORD e = GetLastError();
    return e ? e : (DWORD)-1;
}

static void self_report(void) {
    DWORD rid, ui, err;
    token_query(GetCurrentProcess(), &rid, &ui, &err);
    g_self_rid = rid == (DWORD)-1 ? 0x2000 : rid;

    HANDLE tok = NULL;
    TOKEN_ELEVATION elev = {0};
    DWORD len = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &elev, sizeof elev, &len))
            elevated = elev.TokenIsElevated != 0;
        CloseHandle(tok);
    }

    typedef LONG (WINAPI *RtlGetVersionFn)(RTL_OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    RTL_OSVERSIONINFOW vi = { .dwOSVersionInfoSize = sizeof vi };
    if (fn && fn(&vi) == 0)
        wprintf(L"windows   %lu.%lu build %lu\n",
                vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);

    wprintf(L"self      pid=%lu  elevated=%d  integrity=%ls (0x%04lX)  uiAccess=%lu\n\n",
            GetCurrentProcessId(), elevated, rid_name(rid), rid, ui);
}

static BOOL CALLBACK integrity_proc(HWND hwnd, LPARAM lp) {
    (void)lp;
    DWORD pid = 0;
    wchar_t title[48] = L"", cls[48] = L"", exe[MAX_PATH];

    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (!GetWindowTextW(hwnd, title, 48) || !title[0]) return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    GetClassNameW(hwnd, cls, 48);

    PidInfo pi = pid_info(pid);
    DWORD setpos = IsHungAppWindow(hwnd) ? (DWORD)-2 : try_setpos_noop(hwnd);
    DWORD post   = try_post(hwnd, WM_NULL, 0, 0);

    wchar_t rid[24];
    if (pi.rid == (DWORD)-1) _snwprintf(rid, 24, L"err %lu", pi.err);
    else                     _snwprintf(rid, 24, L"%ls", rid_name(pi.rid));
    rid[23] = L'\0';

    wprintf(L"%p  %-18.18ls %-26.26ls %-9ls ui=%lu  setpos=%-5ld post_null=%-5ld %ls\n",
            (void *)hwnd, exe_base(pid, exe, MAX_PATH), cls, rid, pi.uiaccess,
            (long)setpos, (long)post, title);
    return TRUE;
}

static int cmd_integrity(void) {
    wprintf(L"[1] every visible, unowned, titled window of another process\n"
            L"    setpos = error from a no-op SetWindowPos (0 = allowed, 5 = UIPI)\n"
            L"    post_null = error from PostMessage(WM_NULL)\n\n");
    EnumWindows(integrity_proc, 0);
    wprintf(L"\nVERDICT  read the table: a window is UIPI-protected when setpos=5.\n"
            L"         integrity 'err 5' means its token can't be read from here.\n");
    return 0;
}

static bool wait_for(HWND hwnd, bool (*cond)(HWND), int ms) {
    for (int t = 0; t < ms; t += 50) {
        if (cond(hwnd)) return true;
        Sleep(50);
    }
    return cond(hwnd);
}

static bool is_iconic(HWND h)     { return IsIconic(h) != 0; }
static bool is_not_iconic(HWND h) { return !IsIconic(h); }
static bool is_gone(HWND h)       { return !IsWindow(h); }

static HWND resolve_target(int argc, wchar_t **argv, int first) {
    for (int i = first; i < argc; i++) {
        if (argv[i][0] == L'-') continue;
        HWND h = (HWND)(ULONG_PTR)wcstoull(argv[i], NULL, 0);
        return IsWindow(h) ? h : NULL;
    }
    return FindWindowW(L"TaskManagerWindow", NULL);
}

static bool has_flag(int argc, wchar_t **argv, const wchar_t *flag) {
    for (int i = 1; i < argc; i++)
        if (_wcsicmp(argv[i], flag) == 0) return true;
    return false;
}

static int cmd_syscommand(int argc, wchar_t **argv) {
    HWND target = resolve_target(argc, argv, 2);
    if (!target) {
        wprintf(L"FAIL  no target: open Task Manager or pass an HWND\n");
        return 2;
    }

    wchar_t desc[128];
    describe_window(target, desc, 128);
    wprintf(L"target    %p  %ls\n\n", (void *)target, desc);

    wprintf(L"[1] no-op SetWindowPos\n");
    DWORD e = try_setpos_noop(target);
    wprintf(L"      error = %ld  %ls\n\n", (long)e,
            e == ERROR_ACCESS_DENIED ? L"(UIPI: target is protected)" : L"");
    if (e != ERROR_ACCESS_DENIED) {
        wprintf(L"FAIL  the target is not UIPI-protected from this process; run this "
                L"unelevated against an elevated window\n");
        return 2;
    }

    wprintf(L"[2] PostMessage(WM_CLOSE)\n");
    e = try_post(target, WM_CLOSE, 0, 0);
    wprintf(L"      error = %ld  %ls\n\n", (long)e,
            e == ERROR_ACCESS_DENIED ? L"(blocked, as mshell already sees)"
                                     : L"(NOT blocked -- the window may be closing)");
    if (!IsWindow(target)) {
        wprintf(L"VERDICT  WM_CLOSE went through and closed the target\n");
        return 1;
    }

    wprintf(L"[3] PostMessage(WM_SYSCOMMAND, SC_MINIMIZE)\n");
    DWORD e_min = try_post(target, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    bool  minimized = e_min == 0 && wait_for(target, is_iconic, 1500);
    wprintf(L"      error = %ld  minimized = %d\n", (long)e_min, minimized);

    DWORD_PTR res = 0;
    SetLastError(0);
    LRESULT sent = SendMessageTimeoutW(target, WM_SYSCOMMAND, SC_MINIMIZE, 0,
                                       SMTO_ABORTIFHUNG, 1000, &res);
    DWORD e_send = sent ? 0 : GetLastError();
    if (!minimized && sent) minimized = wait_for(target, is_iconic, 1500);
    wprintf(L"      SendMessageTimeout: ok = %d  error = %ld  minimized = %d\n\n",
            sent != 0, (long)e_send, minimized);

    wprintf(L"[4] PostMessage(WM_SYSCOMMAND, SC_RESTORE)\n");
    DWORD e_res = try_post(target, WM_SYSCOMMAND, SC_RESTORE, 0);
    bool  restored = e_res == 0 && wait_for(target, is_not_iconic, 1500);
    wprintf(L"      error = %ld  restored = %d\n\n", (long)e_res, restored || !IsIconic(target));

    bool closed = false;
    DWORD e_close = (DWORD)-3;
    if (has_flag(argc, argv, L"--close")) {
        wprintf(L"[5] PostMessage(WM_SYSCOMMAND, SC_CLOSE)\n");
        e_close = try_post(target, WM_SYSCOMMAND, SC_CLOSE, 0);
        closed = e_close == 0 && wait_for(target, is_gone, 3000);
        wprintf(L"      error = %ld  closed = %d\n\n", (long)e_close, closed);
    }

    bool pass = e_min == 0 && minimized;
    wprintf(L"VERDICT  %ls -- WM_SYSCOMMAND %ls UIPI for this process%ls\n",
            pass ? L"PASS" : L"FAIL",
            pass ? L"passes" : L"is blocked by",
            e_close == (DWORD)-3 ? L" (SC_CLOSE not tried; add --close)" : L"");
    return pass ? 0 : 1;
}

static const wchar_t *mods_name(UINT mods, wchar_t *buf, size_t cap) {
    _snwprintf(buf, cap, L"%ls%ls%ls%ls",
               (mods & MOD_WIN)     ? L"Win+"   : L"",
               (mods & MOD_CONTROL) ? L"Ctrl+"  : L"",
               (mods & MOD_ALT)     ? L"Alt+"   : L"",
               (mods & MOD_SHIFT)   ? L"Shift+" : L"");
    buf[cap - 1] = L'\0';
    return buf;
}

static const wchar_t *vk_name(UINT vk, wchar_t *buf, size_t cap) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        _snwprintf(buf, cap, L"%lc", (wint_t)vk);
    else if (vk >= VK_F1 && vk <= VK_F24)
        _snwprintf(buf, cap, L"F%u", vk - VK_F1 + 1);
    else {
        const wchar_t *n = NULL;
        switch (vk) {
        case VK_ESCAPE: n = L"Esc";   break;
        case VK_TAB:    n = L"Tab";   break;
        case VK_SPACE:  n = L"Space"; break;
        case VK_RETURN: n = L"Enter"; break;
        case VK_LEFT:   n = L"Left";  break;
        case VK_RIGHT:  n = L"Right"; break;
        case VK_UP:     n = L"Up";    break;
        case VK_DOWN:   n = L"Down";  break;
        case VK_LWIN:   n = L"LWin";  break;
        case VK_RWIN:   n = L"RWin";  break;
        case VK_SNAPSHOT: n = L"PrintScreen"; break;
        case VK_VOLUME_UP: n = L"VolumeUp"; break;
        case VK_MEDIA_PLAY_PAUSE: n = L"PlayPause"; break;
        default: break;
        }
        if (n) _snwprintf(buf, cap, L"%ls", n);
        else   _snwprintf(buf, cap, L"vk0x%02X", vk);
    }
    buf[cap - 1] = L'\0';
    return buf;
}

static int cmd_register_all(void) {
    static const UINT combos[] = {
        MOD_WIN, MOD_WIN | MOD_SHIFT, MOD_WIN | MOD_CONTROL, MOD_WIN | MOD_ALT,
        MOD_WIN | MOD_SHIFT | MOD_CONTROL, MOD_WIN | MOD_SHIFT | MOD_ALT,
        MOD_WIN | MOD_CONTROL | MOD_ALT, MOD_WIN | MOD_SHIFT | MOD_CONTROL | MOD_ALT,
    };
    UINT keys[128];
    int  nk = 0;
    for (UINT c = 'A'; c <= 'Z'; c++) keys[nk++] = c;
    for (UINT c = '0'; c <= '9'; c++) keys[nk++] = c;
    for (UINT c = VK_F1; c <= VK_F12; c++) keys[nk++] = c;
    static const UINT extra[] = {
        VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7,
        VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
        VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_SPACE, VK_RETURN, VK_TAB, VK_ESCAPE,
        VK_BACK, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_SNAPSHOT,
    };
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) keys[nk++] = extra[i];

    static const struct { UINT mods, vk; } specials[] = {
        { MOD_ALT, VK_TAB }, { MOD_ALT | MOD_SHIFT, VK_TAB }, { MOD_ALT, VK_ESCAPE },
        { MOD_ALT, VK_SPACE }, { MOD_CONTROL, VK_ESCAPE },
        { MOD_CONTROL | MOD_SHIFT, VK_ESCAPE }, { 0, VK_F12 }, { 0, VK_LWIN },
        { MOD_WIN, VK_LWIN }, { 0, VK_RWIN }, { 0, 'H' }, { 0, VK_ESCAPE },
        { 0, VK_VOLUME_UP }, { 0, VK_MEDIA_PLAY_PAUSE },
    };

    int ok = 0, fail = 0;
    wchar_t mb[32], kb[24];

    wprintf(L"[1] RegisterHotKey + UnregisterHotKey for every Win chord, then specials\n"
            L"    only failures are listed\n\n");

    for (size_t m = 0; m < sizeof combos / sizeof combos[0]; m++) {
        for (int k = 0; k < nk; k++) {
            if (RegisterHotKey(NULL, 1, combos[m], keys[k])) {
                UnregisterHotKey(NULL, 1);
                ok++;
            } else {
                fail++;
                wprintf(L"      FAIL  %ls%ls  error=%lu\n", mods_name(combos[m], mb, 32),
                        vk_name(keys[k], kb, 24), GetLastError());
            }
        }
    }

    wprintf(L"\n[2] specials\n");
    for (size_t i = 0; i < sizeof specials / sizeof specials[0]; i++) {
        bool r = RegisterHotKey(NULL, 1, specials[i].mods, specials[i].vk) != 0;
        DWORD e = r ? 0 : GetLastError();
        if (r) UnregisterHotKey(NULL, 1);
        wprintf(L"      %-4ls  %ls%ls", r ? L"ok" : L"FAIL",
                mods_name(specials[i].mods, mb, 32), vk_name(specials[i].vk, kb, 24));
        if (r) wprintf(L"\n");
        else   wprintf(L"  error=%lu\n", e);
    }

    wprintf(L"\nVERDICT  %d Win chords registered, %d refused\n", ok, fail);
    return 0;
}

static int hotkey_id(UINT mods, UINT vk) { return (int)((mods << 8) | vk); }

static bool hk_register(UINT mods, UINT vk, Hotkey *slot) {
    wchar_t mb[32], kb[24];
    int id = hotkey_id(mods, vk);
    if (!RegisterHotKey(NULL, id, mods, vk)) {
        emit(L"register FAIL %ls%ls error=%lu", mods_name(mods, mb, 32),
             vk_name(vk, kb, 24), GetLastError());
        return false;
    }
    slot->id = id;
    slot->mods = mods;
    slot->vk = vk;
    return true;
}

static void enter_submap(const wchar_t *via) {
    if (g_in_submap) return;
    double t0 = now_ms();
    int fail = 0;
    g_nsub = 0;
    for (UINT c = 'A'; c <= 'Z'; c++) {
        if (c == 'X') continue;
        if (hk_register(0, c, &g_sub[g_nsub])) g_nsub++; else fail++;
    }
    for (UINT c = '0'; c <= '9'; c++)
        if (hk_register(0, c, &g_sub[g_nsub])) g_nsub++; else fail++;
    if (hk_register(0, VK_ESCAPE, &g_sub[g_nsub])) g_nsub++; else fail++;
    if (hk_register(MOD_WIN, 'H', &g_sub[g_nsub])) g_nsub++; else fail++;
    g_in_submap = true;
    emit(L"submap ENTER via %ls: %d registered, %d failed, %.3f ms", via, g_nsub, fail,
         now_ms() - t0);
}

static void exit_submap(const wchar_t *via) {
    if (!g_in_submap) return;
    double t0 = now_ms();
    for (int i = 0; i < g_nsub; i++) UnregisterHotKey(NULL, g_sub[i].id);
    int n = g_nsub;
    g_nsub = 0;
    g_in_submap = false;
    emit(L"submap EXIT  via %ls: %d unregistered, %.3f ms", via, n, now_ms() - t0);
}

static void focus_test(void) {
    wchar_t before[96], after[96];
    describe_window(GetForegroundWindow(), before, 96);

    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.dwExtraInfo = PROBE_TAG;
    SetLastError(0);
    UINT sent = SendInput(1, &in, sizeof in);
    DWORD e_send = sent ? 0 : GetLastError();

    HWND np = FindWindowW(L"Notepad", NULL);
    SetLastError(0);
    BOOL fg = np ? SetForegroundWindow(np) : FALSE;
    DWORD e_fg = fg ? 0 : GetLastError();
    Sleep(150);
    describe_window(GetForegroundWindow(), after, 96);

    emit(L"F1 focus test: fg before=%ls  SendInput=%u (error %lu)  notepad=%p "
         L"SetForegroundWindow=%d (error %lu)  fg after=%ls",
         before, sent, e_send, (void *)np, fg, e_fg, after);
}

static LRESULT CALLBACK kb_proc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(NULL, code, wp, lp);

    KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
    bool  down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    DWORD vk = k->vkCode;
    bool  ours = k->dwExtraInfo == PROBE_TAG;
    bool  swallow = false;

    if (g_mode == MODE_MSHELL && !ours) {
        if (vk == VK_LWIN || vk == VK_RWIN) {
            if (down) {
                if (!g_win_down) g_win_downs_seen++;
                g_win_down = true;
                swallow = true;
            } else {
                g_win_ups_seen++;
                g_win_down = false;
            }
        } else if (g_in_submap) {
            if (down) {
                swallow = true;
                if (vk == VK_ESCAPE) exit_submap(L"hook");
            }
        } else if (down && g_win_down) {
            swallow = true;
            if (vk == 'X') enter_submap(L"hook");
            if (vk == 'Y' && g_stall) Sleep(1500);
        }
    }

    if (down) {
        g_hook_downs++;
        if (swallow) g_hook_swallowed++;
    }

    wchar_t fg[96], kb[24];
    describe_window(GetForegroundWindow(), fg, 96);
    emit(L"hook   %ls %-8ls %ls%ls%ls  fg=%ls",
         down ? L"down" : L"up  ", vk_name(vk, kb, 24),
         swallow ? L"SWALLOW" : L"pass",
         (k->flags & LLKHF_INJECTED) ? L" injected" : L"",
         ours ? L" (probe)" : L"", fg);

    return swallow ? 1 : CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        wchar_t under[96], fg[96];
        describe_window(GetAncestor(WindowFromPoint(m->pt), GA_ROOT), under, 96);
        describe_window(GetForegroundWindow(), fg, 96);
        emit(L"mouse  %ls down  under=%ls  fg=%ls",
             wp == WM_LBUTTONDOWN ? L"L" : L"R", under, fg);
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void CALLBACK win_event(HWINEVENTHOOK h, DWORD ev, HWND hwnd, LONG obj, LONG child,
                               DWORD tid, DWORD t) {
    (void)h; (void)obj; (void)child; (void)tid; (void)t;
    if (ev == EVENT_SYSTEM_DESKTOPSWITCH) {
        emit(L"event  DESKTOPSWITCH");
        return;
    }
    wchar_t d[96];
    describe_window(hwnd, d, 96);
    emit(L"event  FOREGROUND -> %ls", d);
}

static void on_hotkey(WPARAM id, LPARAM lp) {
    UINT mods = LOWORD(lp), vk = HIWORD(lp);
    HWND fgw = GetForegroundWindow();
    bool blind = fgw && window_blind(fgw, NULL);
    if (blind) g_hk_blind++; else g_hk_visible++;

    wchar_t fg[96], mb[32], kb[24];
    describe_window(fgw, fg, 96);
    emit(L"HOTKEY id=0x%03X %ls%ls  async LWin=%d Shift=%d  fg=%ls",
         (unsigned)id, mods_name(mods, mb, 32), vk_name(vk, kb, 24),
         (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0,
         (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, fg);

    if (g_mode != MODE_MSHELL) return;
    if (!g_in_submap && vk == 'X' && mods == MOD_WIN) enter_submap(L"hotkey");
    else if (g_in_submap && vk == VK_ESCAPE)          exit_submap(L"hotkey");
    else if (vk == 'J' && mods == (MOD_WIN | MOD_SHIFT)) focus_test();
}

static BOOL WINAPI on_console_ctrl(DWORD type) {
    (void)type;
    PostThreadMessageW(g_main_tid, WM_QUIT, 0, 0);
    return TRUE;
}

static void print_steps(void) {
    if (g_mode == MODE_PASS) {
        wprintf(L"mode pass: the hook passes everything; bare H is registered.\n"
                L"  D3  focus Notepad and type h   expect: HOTKEY H, and no 'h' typed\n\n");
        return;
    }
    wprintf(L"mode mshell: the hook swallows Win-down and Win+keys like mshell does.\n"
            L"Registered: Win+J, Win+Shift+J (focus test), Win+X (enter submap)%ls.\n"
            L"Win+X registers bare A-Z, 0-9, Esc and Win+H; Esc leaves.\n\n"
            L"  D1  Notepad: Win+J                      expect: hook SWALLOW, no HOTKEY\n"
            L"  H1  Task Manager: Win+J                 expect: HOTKEY, no hook line\n"
            L"  H1s Task Manager: Win+X, h, 2, Esc      expect: 4 HOTKEYs, submap enter/exit\n"
            L"  D2  Notepad: Win+X, h, Esc, then type 'abc' fast\n"
            L"                                          expect: no HOTKEY; Notepad gets 'abc'\n"
            L"  D4  hold Win+J 2 s in Notepad, then in Task Manager (compare repeat)\n"
            L"  S1  Notepad: hold Win, click Task Manager, release Win, click Notepad, type j\n"
            L"  S2  Task Manager: hold Win, press Shift+J (focus test moves to Notepad), J\n"
            L"  F1  Task Manager: Win+Shift+J           expect: F1 line shows whether focus moved\n"
            L"  M1  with Task Manager focused, click Task Manager, then Notepad\n"
            L"%ls\n",
            g_stall ? L", Win+Y (stalls the hook 1.5 s)" : L"",
            g_stall ? L"  T1  LAST: Notepad: Win+Y               does a HOTKEY also arrive?\n" : L"");
}

static int cmd_hotkey(int argc, wchar_t **argv) {
    int seconds = 120;
    for (int i = 2; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--mode") == 0 && i + 1 < argc) {
            i++;
            if (_wcsicmp(argv[i], L"pass") == 0) g_mode = MODE_PASS;
            else if (_wcsicmp(argv[i], L"mshell") == 0) g_mode = MODE_MSHELL;
            else { wprintf(L"FAIL  unknown mode %ls\n", argv[i]); return 2; }
        } else if (_wcsicmp(argv[i], L"--seconds") == 0 && i + 1 < argc) {
            seconds = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--stall") == 0) {
            g_stall = true;
        }
    }
    if (seconds <= 0) seconds = 120;

    g_main_tid = GetCurrentThreadId();
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    if (g_mode == MODE_MSHELL) {
        if (hk_register(MOD_WIN, 'J', &g_root[g_nroot])) g_nroot++;
        if (hk_register(MOD_WIN | MOD_SHIFT, 'J', &g_root[g_nroot])) g_nroot++;
        if (hk_register(MOD_WIN, 'X', &g_root[g_nroot])) g_nroot++;
        if (g_stall && hk_register(MOD_WIN, 'Y', &g_root[g_nroot])) g_nroot++;
    } else {
        if (hk_register(0, 'H', &g_root[g_nroot])) g_nroot++;
    }

    HHOOK kb = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, GetModuleHandleW(NULL), 0);
    HHOOK ms = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(NULL), 0);
    HWINEVENTHOOK ev1 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                        NULL, win_event, 0, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK ev2 = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
                                        NULL, win_event, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!kb) {
        wprintf(L"FAIL  SetWindowsHookEx(WH_KEYBOARD_LL) error=%lu\n", GetLastError());
        return 2;
    }

    print_steps();
    wprintf(L"Running %d s (Ctrl+C ends early). Run mshell --msg panic first so mshell's "
            L"hook stays out of the way; reload mshell afterwards.\n\n", seconds);
    fflush(stdout);

    HANDLE printer = CreateThread(NULL, 0, printer_thread, NULL, 0, NULL);
    SetTimer(NULL, 0, (UINT)seconds * 1000u, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY)      on_hotkey(msg.wParam, msg.lParam);
        else if (msg.message == WM_TIMER && !msg.hwnd) break;
        else DispatchMessageW(&msg);
    }

    exit_submap(L"shutdown");
    for (int i = 0; i < g_nroot; i++) UnregisterHotKey(NULL, g_root[i].id);
    UnhookWindowsHookEx(kb);
    if (ms) UnhookWindowsHookEx(ms);
    if (ev1) UnhookWinEvent(ev1);
    if (ev2) UnhookWinEvent(ev2);

    InterlockedExchange(&g_log_done, 1);
    SetEvent(g_log_evt);
    WaitForSingleObject(printer, 2000);

    wprintf(L"\nsummary   hook key-downs %d (swallowed %d); Win-downs seen %d, Win-ups seen %d\n"
            L"          HOTKEYs with a blind foreground %d, with a hook-visible foreground %d\n",
            g_hook_downs, g_hook_swallowed, g_win_downs_seen, g_win_ups_seen,
            g_hk_blind, g_hk_visible);
    if (g_mode == MODE_MSHELL) {
        wprintf(L"VERDICT  H1 %ls -- hotkeys %ls under a blind foreground\n",
                g_hk_blind ? L"PASS" : L"not exercised", g_hk_blind ? L"arrive" : L"were not seen");
        wprintf(L"VERDICT  D1/D2 %ls -- %d hotkey(s) fired while the hook could see the "
                L"foreground (expect 0 unless you ran S2)\n",
                g_hk_visible ? L"CHECK" : L"PASS", g_hk_visible);
    }
    return 0;
}

static void usage(void) {
    wprintf(L"usage: probe_uipi integrity\n"
            L"       probe_uipi syscommand [hwnd] [--close]\n"
            L"       probe_uipi hotkey [--mode mshell|pass] [--seconds N] [--stall]\n"
            L"       probe_uipi register-all\n");
}

int wmain(int argc, wchar_t **argv) {
    if (_isatty(_fileno(stdout))) _setmode(_fileno(stdout), _O_U16TEXT);

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    InitializeCriticalSection(&g_log_cs);
    g_log_evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    wprintf(L"=== mshell UIPI probe ===\n\n");
    self_report();

    if (argc < 2) { usage(); return 2; }
    if (_wcsicmp(argv[1], L"integrity") == 0)    return cmd_integrity();
    if (_wcsicmp(argv[1], L"syscommand") == 0)   return cmd_syscommand(argc, argv);
    if (_wcsicmp(argv[1], L"hotkey") == 0)       return cmd_hotkey(argc, argv);
    if (_wcsicmp(argv[1], L"register-all") == 0) return cmd_register_all();
    usage();
    return 2;
}
