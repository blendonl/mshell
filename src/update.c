#include "mshell.h"
#include "update_parse.h"

#include <winhttp.h>
#include <bcrypt.h>

#define UPDATE_HOST  L"api.github.com"
#define UPDATE_PATH  L"/repos/blendonl/mshell/releases/latest"
#define UPDATE_URL   L"https://api.github.com/repos/blendonl/mshell/releases/latest"
#define UPDATE_KEY   L"Software\\mshell"

#define WINLOGON_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"

#define UPDATE_MAX_DOWNLOAD  (64u * 1024u * 1024u)
#define UPDATE_MAX_JSON      (1u * 1024u * 1024u)

#define UPDATE_ASSET_SUFFIX  "-win64.zip"

static BYTE *http_get(const wchar_t *url, DWORD *out_len,
                      DWORD max_bytes, DWORD recv_timeout_ms) {
    HINTERNET ses = NULL, con = NULL, req = NULL;
    BYTE     *buf = NULL;
    bool      ok  = false;

    URL_COMPONENTS uc;
    wchar_t host[256], path[2048], extra[2048];
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize      = sizeof(uc);
    uc.lpszHostName      = host;  uc.dwHostNameLength   = ARRAYSIZE(host);
    uc.lpszUrlPath       = path;  uc.dwUrlPathLength    = ARRAYSIZE(path);
    uc.lpszExtraInfo     = extra; uc.dwExtraInfoLength  = ARRAYSIZE(extra);

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        log_err(L"update: cannot parse URL %ls", url);
        return NULL;
    }

    wchar_t target[4096];
    _snwprintf(target, ARRAYSIZE(target) - 1, L"%ls%ls", path, extra);
    target[ARRAYSIZE(target) - 1] = L'\0';

    ses = WinHttpOpen(L"mshell/" MSHELL_VERSION_W,
                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto out;

    WinHttpSetTimeouts(ses, 5000, 5000, 5000, (int)recv_timeout_ms);

    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) goto out;

    req = WinHttpOpenRequest(con, L"GET", target, NULL,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS
                                 ? WINHTTP_FLAG_SECURE : 0);
    if (!req) goto out;

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto out;
    if (!WinHttpReceiveResponse(req, NULL)) goto out;

    DWORD status = 0, status_sz = sizeof(status);
    if (!WinHttpQueryHeaders(req,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                             WINHTTP_NO_HEADER_INDEX)) goto out;
    if (status != 200) {
        log_err(L"update: HTTP %lu for %ls", status, url);
        goto out;
    }

    DWORD cap = 65536, used = 0;
    buf = (BYTE *)malloc(cap);
    if (!buf) goto out;

    for (;;) {
        if (used + 1 >= cap) {
            if (cap >= max_bytes) {
                log_err(L"update: response exceeds %lu bytes", max_bytes);
                goto out;
            }
            DWORD next = cap * 2;
            if (next > max_bytes) next = max_bytes;
            BYTE *bigger = (BYTE *)realloc(buf, next + 1);
            if (!bigger) goto out;
            buf = bigger;
            cap = next;
        }

        DWORD got = 0;
        if (!WinHttpReadData(req, buf + used, cap - used, &got)) goto out;
        if (got == 0) break;
        used += got;
    }

    buf[used] = '\0';
    if (out_len) *out_len = used;
    ok = true;

out:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (!ok) { free(buf); buf = NULL; }
    return buf;
}

static bool sha256_hex(const BYTE *data, DWORD len, char out[65]) {
    BCRYPT_ALG_HANDLE  alg = NULL;
    BCRYPT_HASH_HANDLE h   = NULL;
    BYTE digest[32];
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0)
        return false;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) != 0) goto out;
    if (BCryptHashData(h, (PUCHAR)data, len, 0) != 0)        goto out;
    if (BCryptFinishHash(h, digest, sizeof(digest), 0) != 0)  goto out;

    for (int i = 0; i < 32; i++)
        _snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = '\0';
    ok = true;

out:
    if (h)   BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static void update_notify(NotifyKind kind, int ms, const wchar_t *fmt, ...) {
    wchar_t msg[NOTIFY_TEXT_CAP];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(msg, NOTIFY_TEXT_CAP - 1, fmt, ap);
    va_end(ap);
    msg[NOTIFY_TEXT_CAP - 1] = L'\0';

    log_msg(kind == NOTIFY_ERROR ? LOG_ERROR : LOG_INFO, L"update: %ls", msg);

    if (g.message_window)
        PostMessageW(g.message_window, WM_MSHELL_UPDATE,
                     MAKEWPARAM((WORD)kind, (WORD)ms), (LPARAM)_wcsdup(msg));
}

static bool checked_today(void) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    DWORD today = (DWORD)st.wYear * 400 + (DWORD)st.wMonth * 31 + st.wDay;

    HKEY  k;
    DWORD last = 0, sz = sizeof(last);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UPDATE_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        NULL, &k, NULL) != ERROR_SUCCESS)
        return true;

    RegQueryValueExW(k, L"LastUpdateCheck", NULL, NULL, (LPBYTE)&last, &sz);
    bool done = (last == today);
    if (!done)
        RegSetValueExW(k, L"LastUpdateCheck", 0, REG_DWORD,
                       (const BYTE *)&today, sizeof(today));
    RegCloseKey(k);
    return done;
}

static BYTE *fetch_latest_release(char *tag, size_t tag_cap) {
    DWORD len = 0;
    BYTE *body = http_get(UPDATE_URL, &len, UPDATE_MAX_JSON, 10000);
    if (!body) return NULL;

    if (!update_json_str((const char *)body, (const char *)body + len,
                         "tag_name", tag, tag_cap)) {
        log_err(L"update: no tag_name in the release response");
        free(body);
        return NULL;
    }

    if (tag[0] == 'v' || tag[0] == 'V')
        memmove(tag, tag + 1, strlen(tag));

    return body;
}

static DWORD WINAPI update_thread(LPVOID param) {
    (void)param;

    char  tag[64];
    BYTE *body = fetch_latest_release(tag, sizeof(tag));
    if (!body) return 0;

    if (update_version_cmp(tag, MSHELL_VERSION) > 0) {
        wchar_t latest[64];
        MultiByteToWideChar(CP_UTF8, 0, tag, -1, latest, ARRAYSIZE(latest));
        update_notify(NOTIFY_INFO, 15000,
                      L"mshell %ls is available (you have %ls)",
                      latest, MSHELL_VERSION_W);
    } else {
        log_msg(LOG_INFO, L"update: up to date");
    }

    free(body);
    return 0;
}

void update_check_async(void) {
    if (!g.cfg.update_check) return;
    if (checked_today()) return;

    HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static volatile LONG s_update_running = 0;

static bool run_wait(const wchar_t *cmdline, const wchar_t *cwd,
                     DWORD timeout_ms, DWORD *exit_code) {
    wchar_t buf[2048];
    _snwprintf(buf, ARRAYSIZE(buf) - 1, L"%ls", cmdline);
    buf[ARRAYSIZE(buf) - 1] = L'\0';

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, cwd, &si, &pi))
        return false;

    bool ok = (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0);
    if (ok && exit_code) GetExitCodeProcess(pi.hProcess, exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

static bool prepare_workdir(wchar_t *out, size_t cap) {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return false;

    _snwprintf(out, cap - 1, L"%lsmshell-update", tmp);
    out[cap - 1] = L'\0';

    wchar_t cmd[MAX_PATH + 64];
    _snwprintf(cmd, ARRAYSIZE(cmd) - 1, L"cmd.exe /c rd /s /q \"%ls\"", out);
    cmd[ARRAYSIZE(cmd) - 1] = L'\0';
    run_wait(cmd, NULL, 15000, NULL);

    return CreateDirectoryW(out, NULL) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool running_as_installed_shell(void) {
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;

    const HKEY hives[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (int i = 0; i < 2; i++) {
        HKEY k;
        if (RegOpenKeyExW(hives[i], WINLOGON_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
            continue;

        wchar_t shell[MAX_PATH * 2];
        DWORD   sz = sizeof(shell), type = 0;
        LSTATUS r = RegQueryValueExW(k, L"Shell", NULL, &type,
                                     (LPBYTE)shell, &sz);
        RegCloseKey(k);
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            continue;

        DWORD chars = sz / sizeof(wchar_t);
        if (chars >= ARRAYSIZE(shell)) chars = ARRAYSIZE(shell) - 1;
        shell[chars] = L'\0';

        wchar_t path[MAX_PATH * 2];
        if (type == REG_EXPAND_SZ) {
            if (!ExpandEnvironmentStringsW(shell, path, ARRAYSIZE(path)))
                continue;
        } else {
            _snwprintf(path, ARRAYSIZE(path) - 1, L"%ls", shell);
        }
        path[ARRAYSIZE(path) - 1] = L'\0';

        wchar_t *p = path, *endq;
        if (*p == L'"' && (endq = wcschr(p + 1, L'"')) != NULL) {
            *endq = L'\0';
            p++;
        } else {
            wchar_t *arg = wcsstr(p, L" --");
            if (arg) *arg = L'\0';
        }

        if (_wcsicmp(p, self) == 0) return true;
    }
    return false;
}

static void install_log_path(wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        if (!GetTempPathW(MAX_PATH, dir)) { out[0] = L'\0'; return; }
        _snwprintf(out, cap - 1, L"%lsmshell-install.log", dir);
    } else {
        _snwprintf(out, cap - 1, L"%ls\\mshell\\install.log", dir);
    }
    out[cap - 1] = L'\0';
}

static bool winlogon_restarts_the_shell(void) {
    HKEY  k;
    DWORD v = 1, sz = sizeof v, type = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WINLOGON_KEY, 0, KEY_READ, &k)
        != ERROR_SUCCESS)
        return true;

    if (RegQueryValueExW(k, L"AutoRestartShell", NULL, &type, (LPBYTE)&v, &sz)
            != ERROR_SUCCESS || type != REG_DWORD)
        v = 1;
    RegCloseKey(k);

    return v != 0;
}

void update_clear_staged_image(void) {
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH - 5) return;

    wchar_t old[MAX_PATH + 8];
    _snwprintf(old, ARRAYSIZE(old) - 1, L"%ls.old", self);
    old[ARRAYSIZE(old) - 1] = L'\0';

    if (GetFileAttributesW(old) == INVALID_FILE_ATTRIBUTES) return;
    if (DeleteFileW(old))
        log_msg(LOG_INFO, L"update: removed the previous build (%ls)", old);
}

static void update_restart_self(const wchar_t *version) {
    if (!winlogon_restarts_the_shell()) {
        update_notify(NOTIFY_WARN, 30000,
                      L"mshell %ls is installed, but AutoRestartShell is off on "
                      L"this machine — stopping the shell would log you out. "
                      L"Sign out and back in to start the new build.", version);
        return;
    }

    update_notify(NOTIFY_INFO, 5000,
                  L"mshell %ls installed — restarting.", version);
    Sleep(1200);

    if (g.message_window)
        PostMessageW(g.message_window, WM_MSHELL_RESTART, 0, 0);
}

static DWORD WINAPI install_thread(LPVOID param) {
    (void)param;

    BYTE *body = NULL, *zip = NULL;

    update_notify(NOTIFY_INFO, 4000, L"Checking for updates …");

    char tag[64];
    body = fetch_latest_release(tag, sizeof(tag));
    if (!body) {
        update_notify(NOTIFY_ERROR, 12000,
                      L"Could not reach GitHub to check for updates.");
        goto out;
    }

    wchar_t latest[64];
    MultiByteToWideChar(CP_UTF8, 0, tag, -1, latest, ARRAYSIZE(latest));

    if (update_version_cmp(tag, MSHELL_VERSION) <= 0) {
        update_notify(NOTIFY_INFO, 6000,
                      L"mshell %ls is the latest release.", MSHELL_VERSION_W);
        goto out;
    }

    char name_u8[256], url_u8[1024], digest_u8[128];
    if (!update_find_asset((const char *)body, UPDATE_ASSET_SUFFIX,
                           name_u8, sizeof(name_u8),
                           url_u8,  sizeof(url_u8),
                           digest_u8, sizeof(digest_u8))) {
        update_notify(NOTIFY_ERROR, 12000,
                      L"Release %ls has no %hs asset to install.",
                      latest, UPDATE_ASSET_SUFFIX);
        goto out;
    }

    wchar_t url[1024], asset[256];
    MultiByteToWideChar(CP_UTF8, 0, url_u8,  -1, url,   ARRAYSIZE(url));
    MultiByteToWideChar(CP_UTF8, 0, name_u8, -1, asset, ARRAYSIZE(asset));

    update_notify(NOTIFY_INFO, 8000,
                  L"Downloading mshell %ls …", latest);

    DWORD zip_len = 0;
    zip = http_get(url, &zip_len, UPDATE_MAX_DOWNLOAD, 120000);
    if (!zip || zip_len == 0) {
        update_notify(NOTIFY_ERROR, 12000, L"Download of mshell %ls failed.",
                      latest);
        goto out;
    }

    if (digest_u8[0]) {
        const char *want = digest_u8;
        if (_strnicmp(want, "sha256:", 7) == 0) {
            want += 7;

            char got[65];
            if (!sha256_hex(zip, zip_len, got)) {
                update_notify(NOTIFY_ERROR, 12000,
                              L"Could not hash the download to verify it.");
                goto out;
            }
            if (_stricmp(got, want) != 0) {
                update_notify(NOTIFY_ERROR, 20000,
                              L"The download does not match the hash GitHub "
                              L"published. Nothing was installed.");
                goto out;
            }
            log_msg(LOG_INFO, L"update: sha256 verified (%hs)", got);
        } else {
            log_msg(LOG_INFO, L"update: unknown digest algorithm '%hs', "
                              L"skipping verification", digest_u8);
        }
    } else {
        log_msg(LOG_INFO, L"update: release %ls published no digest", latest);
    }

    wchar_t dir[MAX_PATH];
    if (!prepare_workdir(dir, ARRAYSIZE(dir))) {
        update_notify(NOTIFY_ERROR, 12000,
                      L"Could not create a working folder for the update.");
        goto out;
    }

    wchar_t zip_path[MAX_PATH];
    _snwprintf(zip_path, ARRAYSIZE(zip_path) - 1, L"%ls\\%ls", dir, asset);
    zip_path[ARRAYSIZE(zip_path) - 1] = L'\0';

    HANDLE f = CreateFileW(zip_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        update_notify(NOTIFY_ERROR, 12000, L"Could not write %ls.", zip_path);
        goto out;
    }
    DWORD written = 0;
    BOOL  wrote   = WriteFile(f, zip, zip_len, &written, NULL);
    CloseHandle(f);
    if (!wrote || written != zip_len) {
        update_notify(NOTIFY_ERROR, 12000,
                      L"Could not write the whole download to disk.");
        goto out;
    }

    update_notify(NOTIFY_INFO, 6000, L"Unpacking mshell %ls …", latest);

    wchar_t cmd[MAX_PATH * 3];
    DWORD   rc = 1;
    _snwprintf(cmd, ARRAYSIZE(cmd) - 1,
               L"tar.exe -xf \"%ls\" -C \"%ls\"", zip_path, dir);
    cmd[ARRAYSIZE(cmd) - 1] = L'\0';
    if (!run_wait(cmd, dir, 120000, &rc) || rc != 0) {
        _snwprintf(cmd, ARRAYSIZE(cmd) - 1,
                   L"powershell.exe -NoProfile -NonInteractive -Command "
                   L"\"Expand-Archive -LiteralPath '%ls' -DestinationPath "
                   L"'%ls' -Force\"", zip_path, dir);
        cmd[ARRAYSIZE(cmd) - 1] = L'\0';
        rc = 1;
        if (!run_wait(cmd, dir, 180000, &rc) || rc != 0) {
            update_notify(NOTIFY_ERROR, 12000,
                          L"Could not unpack %ls.", asset);
            goto out;
        }
    }

    wchar_t root[MAX_PATH], bat[MAX_PATH];
    _snwprintf(root, ARRAYSIZE(root) - 1, L"%ls\\%ls", dir, asset);
    root[ARRAYSIZE(root) - 1] = L'\0';
    size_t rl = wcslen(root);
    if (rl > 4 && _wcsicmp(root + rl - 4, L".zip") == 0) root[rl - 4] = L'\0';

    _snwprintf(bat, ARRAYSIZE(bat) - 1, L"%ls\\install.bat", root);
    bat[ARRAYSIZE(bat) - 1] = L'\0';

    if (GetFileAttributesW(bat) == INVALID_FILE_ATTRIBUTES) {
        update_notify(NOTIFY_ERROR, 15000,
                      L"Unpacked %ls but found no install.bat in it.", asset);
        goto out;
    }

    if (!running_as_installed_shell()) {
        update_notify(NOTIFY_WARN, 20000,
                      L"mshell %ls is unpacked, but this session is not the "
                      L"installed shell — not installing. Run "
                      L"install.bat in %ls yourself.", latest, root);
        goto out;
    }

    update_notify(NOTIFY_INFO, 15000, L"Installing mshell %ls …", latest);

    wchar_t ilog[MAX_PATH];
    install_log_path(ilog, ARRAYSIZE(ilog));

    wchar_t run[MAX_PATH * 3];
    _snwprintf(run, ARRAYSIZE(run) - 1,
               L"cmd.exe /c \"\"%ls\" /norestart >\"%ls\" 2>&1\"", bat, ilog);
    run[ARRAYSIZE(run) - 1] = L'\0';

    DWORD irc = 1;
    if (!run_wait(run, root, 300000, &irc)) {
        update_notify(NOTIFY_ERROR, 25000,
                      L"install.bat did not finish. Nothing was restarted; "
                      L"%ls says how far it got.", ilog);
        goto out;
    }
    if (irc != 0) {
        update_notify(NOTIFY_ERROR, 25000,
                      L"install.bat failed (exit %lu) — see %ls. The running "
                      L"mshell is untouched.", irc, ilog);
        goto out;
    }

    log_msg(LOG_INFO, L"update: install.bat finished cleanly (log: %ls)", ilog);
    update_restart_self(latest);

out:
    free(zip);
    free(body);
    InterlockedExchange(&s_update_running, 0);
    return 0;
}

void update_install_async(void) {
    if (InterlockedCompareExchange(&s_update_running, 1, 0) != 0) {
        log_msg(LOG_INFO, L"update: already in progress");
        return;
    }

    HANDLE t = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    } else {
        InterlockedExchange(&s_update_running, 0);
        log_err(L"update: CreateThread failed: %lu", GetLastError());
    }
}
