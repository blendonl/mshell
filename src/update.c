/* ===========================================================================
 * update.c — noticing that a new release exists, and applying one when asked.
 *
 * TWO HALVES, and the line between them is the whole design decision.
 *
 * The BACKGROUND CHECK (update_check_async) is notify-only, deliberately, and
 * stays that way. mshell is the Windows shell: an updater that downloaded code
 * and swapped out the process the session depends on *unattended* would turn a
 * bad release into a black screen at sign-in with no desktop left to fix it
 * from. A once-a-day check that raises one notification cannot do that.
 *
 * The `update` ACTION (update_install_async) is the half that applies, and
 * what makes it defensible is precisely what the background check lacks:
 * somebody pressed a key. It is attended, it is one keystroke away from not
 * having happened, and the user is sitting in front of the machine when the
 * shell restarts. That is a different risk from an unattended swap rather than
 * a smaller helping of the same one.
 *
 * It does NOT reimplement the install. install.bat already gets the hard part
 * right — it renames the running image instead of overwriting it, restarts
 * only when Winlogon is willing to put a shell back, and leaves the old build
 * running if the kill is refused. Duplicating that sequence here would mean
 * two copies of it, and the one in C would be the less tested. So this fetches
 * the release, checks it, unpacks it, and runs the install.bat that came in
 * the zip.
 *
 * There is still no code-signing certificate, so nothing here proves the
 * binary is ours. What it does prove is that the bytes on disk are the bytes
 * GitHub's API described: the release metadata carries a SHA-256 per asset and
 * arrives over TLS, and the download is hashed against it before anything is
 * unpacked. That catches the truncated or corrupted download, which is the
 * failure this is actually able to detect.
 *
 * Both halves run on their own thread — WinHTTP blocks, and neither may sit on
 * the thread that services keybinds — and report back by posting to the
 * message window, the same pattern the config watcher and IPC server use.
 * =========================================================================== */
#include "mshell.h"
#include "update_parse.h"

#include <winhttp.h>
#include <bcrypt.h>

#define UPDATE_HOST  L"api.github.com"
#define UPDATE_PATH  L"/repos/blendonl/mshell/releases/latest"
#define UPDATE_URL   L"https://api.github.com/repos/blendonl/mshell/releases/latest"
#define UPDATE_KEY   L"Software\\mshell"

/* The release zip is ~500 KB today. The cap is not a prediction, it is a
 * ceiling on what a hostile or broken response can make us allocate. */
#define UPDATE_MAX_DOWNLOAD  (64u * 1024u * 1024u)
#define UPDATE_MAX_JSON      (1u * 1024u * 1024u)

/* Which asset in the release is the one we install. `make dist` names it after
 * DISTNAME, and the folder inside the zip has the same name minus the suffix,
 * which is how the install.bat inside it is found without guessing. */
#define UPDATE_ASSET_SUFFIX  "-win64.zip"

/* ===========================================================================
 * HTTP
 * =========================================================================== */

/* GET `url` into a malloc'd, NUL-terminated buffer. Follows redirects, which
 * the asset download needs — browser_download_url is a github.com link that
 * lands on a CDN host. Returns NULL and logs on any failure, including a
 * non-200 status: the previous code parsed whatever body came back, so a 404
 * read as "no tag_name" rather than as the error it was. */
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

    /* Path and query go to WinHttpOpenRequest as one string. */
    wchar_t target[4096];
    _snwprintf(target, ARRAYSIZE(target) - 1, L"%ls%ls", path, extra);
    target[ARRAYSIZE(target) - 1] = L'\0';

    /* GitHub's API requires a User-Agent; WinHttpOpen supplies ours. */
    ses = WinHttpOpen(L"mshell/" MSHELL_VERSION_W,
                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto out;

    /* Connect quickly or not at all, but allow a download the time to arrive:
     * a hung connection must not keep this thread alive across a shutdown. */
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
        if (got == 0) break;                     /* the whole body is in */
        used += got;
    }

    buf[used] = '\0';                            /* callers may treat it as text */
    if (out_len) *out_len = used;
    ok = true;

out:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (!ok) { free(buf); buf = NULL; }
    return buf;
}

/* ===========================================================================
 * Integrity
 * =========================================================================== */

/* Lowercase hex SHA-256 of `data`. Windows supplies the primitive; the point
 * is only to compare against the digest the API stated. */
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

/* ===========================================================================
 * Reporting
 * =========================================================================== */

/* Raise a toast from the update thread. Overlays are painted on the main
 * thread, so the text is handed over by PostMessage and freed there. The
 * duration rides along in wParam because a failure worth reading and a
 * progress line worth glancing at should not sit on screen for equal time. */
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

/* ===========================================================================
 * The daily check — notify only
 * =========================================================================== */

/* Last check, as a day number, so "at most once a day" survives a restart. */
static bool checked_today(void) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    DWORD today = (DWORD)st.wYear * 400 + (DWORD)st.wMonth * 31 + st.wDay;

    HKEY  k;
    DWORD last = 0, sz = sizeof(last);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UPDATE_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                        NULL, &k, NULL) != ERROR_SUCCESS)
        return true;                        /* cannot record it: do not ask */

    RegQueryValueExW(k, L"LastUpdateCheck", NULL, NULL, (LPBYTE)&last, &sz);
    bool done = (last == today);
    if (!done)
        RegSetValueExW(k, L"LastUpdateCheck", 0, REG_DWORD,
                       (const BYTE *)&today, sizeof(today));
    RegCloseKey(k);
    return done;
}

/* Fetch the latest release and copy out its tag with any leading "v" dropped,
 * so it reads as a version everywhere it is used. The body is handed back
 * because the caller may want the assets out of it too. */
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

    if (tag[0] == 'v' || tag[0] == 'V')     /* tags are conventionally v-prefixed */
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
    if (!g.update_check) return;
    if (checked_today()) return;

    HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);   /* fire and forget: it reports by PostMessage */
}

/* ===========================================================================
 * The `update` action — fetch, verify, unpack, hand over to install.bat
 * =========================================================================== */

/* One at a time. Holding the key down, or pressing it again while a download
 * is in flight, must not start a second one racing the first into the same
 * working directory. */
static volatile LONG s_update_running = 0;

/* Run a command line to completion. Used for the unpack step, where the exit
 * code is the answer. The buffer is copied because CreateProcessW writes to
 * the command line it is given. */
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
    si.wShowWindow = SW_HIDE;              /* unpacking is not a spectacle */

    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, cwd, &si, &pi))
        return false;

    bool ok = (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0);
    if (ok && exit_code) GetExitCodeProcess(pi.hProcess, exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

/* %TEMP%\mshell-update — emptied first, so a failed attempt cannot leave a
 * half-unpacked tree for the next one to run install.bat out of. */
static bool prepare_workdir(wchar_t *out, size_t cap) {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return false;

    _snwprintf(out, cap - 1, L"%lsmshell-update", tmp);  /* GetTempPath ends in \ */
    out[cap - 1] = L'\0';

    wchar_t cmd[MAX_PATH + 64];
    _snwprintf(cmd, ARRAYSIZE(cmd) - 1, L"cmd.exe /c rd /s /q \"%ls\"", out);
    cmd[ARRAYSIZE(cmd) - 1] = L'\0';
    run_wait(cmd, NULL, 15000, NULL);        /* absent is a fine outcome */

    return CreateDirectoryW(out, NULL) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Is the running mshell.exe the copy that install.bat manages?
 *
 * install.bat installs to a fixed location and makes that path the user's
 * shell. Running it from a session started somewhere else — a portable tree,
 * `--test` alongside Explorer, a dev build out of the source directory —
 * would not update anything; it would install mshell as the shell for the
 * first time, which is a much bigger thing than what the key was pressed for.
 * So the registered shell is the reference: if that path is us, this is an
 * upgrade. Anything else stops short of install.bat, with the unpacked tree
 * left behind and named so it can be run by hand. */
static bool running_as_installed_shell(void) {
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;

    static const wchar_t *WINLOGON =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

    const HKEY hives[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (int i = 0; i < 2; i++) {
        HKEY k;
        if (RegOpenKeyExW(hives[i], WINLOGON, 0, KEY_READ, &k) != ERROR_SUCCESS)
            continue;

        wchar_t shell[MAX_PATH * 2];
        DWORD   sz = sizeof(shell), type = 0;
        LSTATUS r = RegQueryValueExW(k, L"Shell", NULL, &type,
                                     (LPBYTE)shell, &sz);
        RegCloseKey(k);
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            continue;
        shell[ARRAYSIZE(shell) - 1] = L'\0';

        /* The value is "<path>\mshell.exe --shell": take the exe, and allow it
         * to have been written quoted. */
        wchar_t path[MAX_PATH * 2];
        _snwprintf(path, ARRAYSIZE(path) - 1, L"%ls", shell);
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

    /* Verify before anything is written where a script might run it. An
     * absent digest costs the check, not the update — releases made before
     * GitHub exposed the field have none. */
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

    /* tar.exe has shipped in Windows since 1803 and reads zips; PowerShell's
     * Expand-Archive is the fallback for a machine where it is missing. */
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

    /* The zip keeps a versioned top-level folder named after the asset without
     * its ".zip", which is where install.bat lands. */
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

    /* The one irreversible step, and the one place this declines to act on its
     * own. See running_as_installed_shell(). */
    if (!running_as_installed_shell()) {
        update_notify(NOTIFY_WARN, 20000,
                      L"mshell %ls is unpacked, but this session is not the "
                      L"installed shell — not installing. Run "
                      L"install.bat in %ls yourself.", latest, root);
        goto out;
    }

    update_notify(NOTIFY_INFO, 15000,
                  L"Installing mshell %ls — mshell will restart.", latest);

    /* Give the toast a moment to be painted: install.bat's first act after the
     * copy is to kill this process, and a message that never made it to the
     * screen is the same as no message. */
    Sleep(1200);

    /* Handed over rather than waited on — install.bat terminates us on
     * purpose and starts the build it just wrote. Its console is left visible:
     * with no taskbar and the shell about to restart, that window is the only
     * account of what happened, and it says which of its outcomes was reached
     * (restarted, installed-but-not-restarted, or refused). */
    wchar_t run[MAX_PATH * 2];
    _snwprintf(run, ARRAYSIZE(run) - 1, L"cmd.exe /c \"install.bat\"");
    run[ARRAYSIZE(run) - 1] = L'\0';

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, run, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE, NULL, root, &si, &pi)) {
        update_notify(NOTIFY_ERROR, 20000,
                      L"Could not start install.bat in %ls.", root);
        goto out;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

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
