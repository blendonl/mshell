#include <windows.h>
#include <objbase.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <wchar.h>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

#ifndef DWM_CLOAKED_APP
#define DWM_CLOAKED_APP       0x00000001
#endif
#ifndef DWM_CLOAKED_SHELL
#define DWM_CLOAKED_SHELL     0x00000002
#endif
#ifndef DWM_CLOAKED_INHERITED
#define DWM_CLOAKED_INHERITED 0x00000004
#endif

#define CLOAK_TYPE_SHELL 1
#define CLOAK_FLAG_ON    2
#define CLOAK_FLAG_OFF   0

static const CLSID CLSID_ImmersiveShell_ =
    { 0xC2F03A33, 0x21F5, 0x47FA, { 0xB4,0xBB,0x15,0x63,0x62,0xA2,0xF2,0x39 } };
static const IID IID_ShellServiceProvider =
    { 0x6D5140C1, 0x7436, 0x11CE, { 0x80,0x34,0x00,0xAA,0x00,0x60,0x09,0xFA } };
static const IID IID_AppViewCollection_1841 =
    { 0x1841C6D7, 0x4F9D, 0x42C0, { 0xAF,0x41,0x87,0x47,0x53,0x8F,0x10,0xE5 } };
static const IID IID_AppViewCollection_2C08 =
    { 0x2C08ADF0, 0xA386, 0x4B35, { 0x92,0x50,0x0F,0xE1,0x83,0x47,0x6F,0xCC } };
static const IID IID_AppView_372E =
    { 0x372E1D3B, 0x38D3, 0x42E4, { 0xA1,0x5B,0x8A,0xB2,0xB1,0x78,0xF5,0x13 } };

typedef struct ShellServiceProvider ShellServiceProvider;
typedef struct AppViewCollection    AppViewCollection;
typedef struct AppView              AppView;

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ShellServiceProvider *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ShellServiceProvider *);
    ULONG   (STDMETHODCALLTYPE *Release)(ShellServiceProvider *);
    HRESULT (STDMETHODCALLTYPE *QueryService)(ShellServiceProvider *, REFGUID, REFIID, void **);
} ShellServiceProviderVtbl;
struct ShellServiceProvider { const ShellServiceProviderVtbl *lpVtbl; };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AppViewCollection *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(AppViewCollection *);
    ULONG   (STDMETHODCALLTYPE *Release)(AppViewCollection *);
    HRESULT (STDMETHODCALLTYPE *GetViews)(AppViewCollection *, void **);
    HRESULT (STDMETHODCALLTYPE *GetViewsByZOrder)(AppViewCollection *, void **);
    HRESULT (STDMETHODCALLTYPE *GetViewsByAppUserModelId)(AppViewCollection *, PCWSTR, void **);
    HRESULT (STDMETHODCALLTYPE *GetViewForHwnd)(AppViewCollection *, HWND, AppView **);
} AppViewCollectionVtbl;
struct AppViewCollection { const AppViewCollectionVtbl *lpVtbl; };

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AppView *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(AppView *);
    ULONG   (STDMETHODCALLTYPE *Release)(AppView *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(AppView *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(AppView *, void **);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(AppView *, int *);
    HRESULT (STDMETHODCALLTYPE *SetFocus)(AppView *);
    HRESULT (STDMETHODCALLTYPE *SwitchTo)(AppView *);
    HRESULT (STDMETHODCALLTYPE *TryInvokeBack)(AppView *, void *);
    HRESULT (STDMETHODCALLTYPE *GetThumbnailWindow)(AppView *, HWND *);
    HRESULT (STDMETHODCALLTYPE *GetMonitor)(AppView *, void **);
    HRESULT (STDMETHODCALLTYPE *GetVisibility)(AppView *, int *);
    HRESULT (STDMETHODCALLTYPE *SetCloak)(AppView *, UINT, int);
} AppViewVtbl;
struct AppView { const AppViewVtbl *lpVtbl; };

static AppView *g_cloaked_view;
static HWND     g_cloaked_hwnd;

static void uncloak_if_cloaked(void) {
    if (!g_cloaked_view) return;
    g_cloaked_view->lpVtbl->SetCloak(g_cloaked_view, CLOAK_TYPE_SHELL, CLOAK_FLAG_OFF);
    g_cloaked_view = NULL;
    wprintf(L"      emergency uncloak issued for %p\n", (void *)g_cloaked_hwnd);
}

static BOOL WINAPI on_console_ctrl(DWORD type) {
    (void)type;
    uncloak_if_cloaked();
    return FALSE;
}

static void report_build(void) {
    typedef LONG (WINAPI *RtlGetVersionFn)(RTL_OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    RTL_OSVERSIONINFOW vi = { .dwOSVersionInfoSize = sizeof vi };

    if (fn && fn(&vi) == 0)
        wprintf(L"windows       %lu.%lu build %lu\n",
                vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    else
        wprintf(L"windows       version unavailable\n");
}

static void report_shell(void) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    HWND prog = FindWindowW(L"Progman", NULL);
    DWORD pid = 0;
    wchar_t exe[MAX_PATH] = L"";

    HWND probe = tray ? tray : prog;
    if (probe) GetWindowThreadProcessId(probe, &pid);
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            DWORD n = MAX_PATH;
            QueryFullProcessImageNameW(h, 0, exe, &n);
            CloseHandle(h);
        }
    }
    wprintf(L"shell windows Shell_TrayWnd=%p Progman=%p\n", (void *)tray, (void *)prog);
    wprintf(L"shell process %ls\n", exe[0] ? exe : L"(none found -- explorer is probably not running)");
}

static BOOL CALLBACK pick_target(HWND hwnd, LPARAM lp) {
    HWND *out = (HWND *)lp;
    DWORD pid = 0;
    wchar_t title[64];

    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (!GetWindowTextW(hwnd, title, 64) || !title[0]) return TRUE;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return TRUE;

    *out = hwnd;
    return FALSE;
}

static void describe(HWND hwnd) {
    wchar_t title[128] = L"", cls[128] = L"";
    DWORD pid = 0;
    GetWindowTextW(hwnd, title, 128);
    GetClassNameW(hwnd, cls, 128);
    GetWindowThreadProcessId(hwnd, &pid);
    wprintf(L"target        %p  pid=%lu  class=%ls  title=%ls\n",
            (void *)hwnd, pid, cls, title);
}

static int cloaked_state(HWND hwnd) {
    int cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)))
        return -1;
    return cloaked;
}

static void report_cloaked_state(HWND hwnd, const wchar_t *when) {
    int c = cloaked_state(hwnd);
    if (c < 0) { wprintf(L"      DWMWA_CLOAKED %ls: query failed\n", when); return; }
    wprintf(L"      DWMWA_CLOAKED %ls: 0x%X%ls%ls%ls%ls\n", when, (unsigned)c,
            c == 0 ? L" (not cloaked)" : L"",
            (c & DWM_CLOAKED_APP) ? L" APP" : L"",
            (c & DWM_CLOAKED_SHELL) ? L" SHELL" : L"",
            (c & DWM_CLOAKED_INHERITED) ? L" INHERITED" : L"");
}

int wmain(int argc, wchar_t **argv) {
    HWND target = NULL;

    if (_isatty(_fileno(stdout))) _setmode(_fileno(stdout), _O_U16TEXT);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    wprintf(L"=== mshell shell-cloak probe ===\n\n");
    report_build();
    report_shell();
    wprintf(L"\n");

    if (argc > 1) {
        target = (HWND)(ULONG_PTR)wcstoull(argv[1], NULL, 0);
        if (!IsWindow(target)) {
            wprintf(L"FAIL  %ls is not a window handle\n", argv[1]);
            return 2;
        }
    } else {
        EnumWindows(pick_target, (LPARAM)&target);
        if (!target) {
            wprintf(L"FAIL  no suitable foreign window found -- pass one as "
                    L"`probe_shellcloak.exe 0x1234`\n");
            return 2;
        }
    }
    describe(target);
    wprintf(L"\n");

    wprintf(L"[1] DwmSetWindowAttribute(DWMWA_CLOAK) on that foreign window\n");
    {
        BOOL on = TRUE;
        HRESULT hr = DwmSetWindowAttribute(target, DWMWA_CLOAK, &on, sizeof on);
        wprintf(L"      hr = 0x%08lX  %ls\n", (unsigned long)hr,
                SUCCEEDED(hr) ? L"SUCCEEDED (unexpected -- undo it)"
                              : L"failed (expected: only the owner app may use this)");
        if (SUCCEEDED(hr)) {
            BOOL off = FALSE;
            DwmSetWindowAttribute(target, DWMWA_CLOAK, &off, sizeof off);
        }
    }
    wprintf(L"\n");

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        wprintf(L"FAIL  CoInitializeEx = 0x%08lX\n", (unsigned long)hr);
        return 2;
    }

    wprintf(L"[2] CoCreateInstance(CLSID_ImmersiveShell, CLSCTX_LOCAL_SERVER)\n");
    ShellServiceProvider *sp = NULL;
    hr = CoCreateInstance(&CLSID_ImmersiveShell_, NULL, CLSCTX_LOCAL_SERVER,
                          &IID_ShellServiceProvider, (void **)&sp);
    wprintf(L"      hr = 0x%08lX  sp = %p\n", (unsigned long)hr, (void *)sp);
    if (FAILED(hr) || !sp) {
        wprintf(L"\nVERDICT  the immersive shell is NOT reachable from this process.\n"
                L"         The shell cloak and the virtual-desktop API are both out.\n");
        CoUninitialize();
        return 1;
    }
    wprintf(L"\n");

    wprintf(L"[3] QueryService(IApplicationViewCollection)\n");
    AppViewCollection *avc = NULL;
    const IID *used = NULL;
    hr = sp->lpVtbl->QueryService(sp, &IID_AppViewCollection_1841,
                                 &IID_AppViewCollection_1841, (void **)&avc);
    wprintf(L"      1841C6D7 hr = 0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && avc) {
        used = &IID_AppViewCollection_1841;
    } else {
        hr = sp->lpVtbl->QueryService(sp, &IID_AppViewCollection_2C08,
                                     &IID_AppViewCollection_2C08, (void **)&avc);
        wprintf(L"      2C08ADF0 hr = 0x%08lX\n", (unsigned long)hr);
        if (SUCCEEDED(hr) && avc) used = &IID_AppViewCollection_2C08;
    }
    if (!used) {
        wprintf(L"\nVERDICT  the immersive shell answered, but no known "
                L"IApplicationViewCollection IID matched this build.\n"
                L"         The interface has moved again; the shell cloak needs a new IID.\n");
        sp->lpVtbl->Release(sp);
        CoUninitialize();
        return 1;
    }
    wprintf(L"      matched %ls\n\n",
            used == &IID_AppViewCollection_1841 ? L"1841C6D7 (Win10 1809+/Win11)"
                                                : L"2C08ADF0 (early Win10)");

    wprintf(L"[4] GetViewForHwnd\n");
    AppView *view = NULL;
    hr = avc->lpVtbl->GetViewForHwnd(avc, target, &view);
    wprintf(L"      hr = 0x%08lX  view = %p\n", (unsigned long)hr, (void *)view);
    if (FAILED(hr) || !view) {
        wprintf(L"\nVERDICT  no application view for that window. Try another "
                L"target before concluding anything.\n");
        avc->lpVtbl->Release(avc);
        sp->lpVtbl->Release(sp);
        CoUninitialize();
        return 1;
    }
    wprintf(L"\n");

    wprintf(L"[5] QueryInterface(IID_IApplicationView) -- vtable layout check\n");
    {
        AppView *confirm = NULL;
        hr = view->lpVtbl->QueryInterface(view, &IID_AppView_372E, (void **)&confirm);
        wprintf(L"      372E1D3B hr = 0x%08lX\n", (unsigned long)hr);
        if (FAILED(hr) || !confirm) {
            wprintf(L"\nVERDICT  the view does NOT answer to the IApplicationView IID we "
                    L"know.\n         Its vtable layout is unverified, so SetCloak is NOT "
                    L"being called --\n         slot 12 could be any method on this build.\n");
            view->lpVtbl->Release(view);
            avc->lpVtbl->Release(avc);
            sp->lpVtbl->Release(sp);
            CoUninitialize();
            return 1;
        }
        confirm->lpVtbl->Release(confirm);
    }
    wprintf(L"\n");

    wprintf(L"[6] SetCloak(SHELL, ON) -- the window should vanish for 3 seconds\n");
    report_cloaked_state(target, L"before");
    g_cloaked_view = view;
    g_cloaked_hwnd = target;
    hr = view->lpVtbl->SetCloak(view, CLOAK_TYPE_SHELL, CLOAK_FLAG_ON);
    wprintf(L"      hr = 0x%08lX\n", (unsigned long)hr);
    Sleep(300);
    report_cloaked_state(target, L"after ");

    int mid = cloaked_state(target);
    Sleep(3000);

    wprintf(L"\n[7] SetCloak(SHELL, OFF)\n");
    HRESULT hr_off = view->lpVtbl->SetCloak(view, CLOAK_TYPE_SHELL, CLOAK_FLAG_OFF);
    g_cloaked_view = NULL;
    wprintf(L"      hr = 0x%08lX\n", (unsigned long)hr_off);
    Sleep(300);
    report_cloaked_state(target, L"after ");

    wprintf(L"\n");
    if (SUCCEEDED(hr) && mid > 0 && (mid & DWM_CLOAKED_SHELL) && SUCCEEDED(hr_off) &&
        !(cloaked_state(target) & DWM_CLOAKED_SHELL)) {
        wprintf(L"VERDICT  PASS -- the shell cloak works from this process.\n"
                L"         A foreign window was cloaked by the Shell and uncloaked again.\n");
    } else {
        wprintf(L"VERDICT  FAIL -- the calls were reachable but the window did not end up\n"
                L"         cloaked-by-shell and back. Report the HRESULTs above.\n");
    }

    view->lpVtbl->Release(view);
    avc->lpVtbl->Release(avc);
    sp->lpVtbl->Release(sp);
    CoUninitialize();
    return 0;
}
