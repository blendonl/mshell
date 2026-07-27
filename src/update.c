/* ===========================================================================
 * update.c — tell the user a newer release exists. Nothing more.
 *
 * NOTIFY-ONLY, deliberately, and this is the whole design decision.
 *
 * mshell is the Windows shell. An auto-updater for a shell would download code
 * and swap out the process the session depends on, unattended — and a bad
 * update to the shell is not a crashed application, it is a black screen at
 * sign-in with no desktop to fix it from. install.bat already handles upgrading
 * carefully (it renames the running image rather than overwriting it, and only
 * restarts once the replacement is in place); nothing here should second-guess
 * that from a background thread.
 *
 * There is also no code-signing certificate, so a downloaded binary could not
 * be verified as ours even if we wanted to apply it. Fetching over TLS proves
 * where the bytes came from, not what they are.
 *
 * So: an opt-in check, at most once a day, that compares version strings and
 * raises one notification. The user upgrades when they choose to.
 *
 * The check runs on its own thread — WinHTTP blocks, and this must never be on
 * the thread that services keybinds — and reports back by posting to the
 * message window, the same pattern the config watcher and IPC server use.
 * =========================================================================== */
#include "mshell.h"

#include <winhttp.h>

#define UPDATE_HOST  L"api.github.com"
#define UPDATE_PATH  L"/repos/mshell/mshell/releases/latest"
#define UPDATE_KEY   L"Software\\mshell"

/* Compare dotted versions numerically: "0.9.0" < "0.10.0", which a string
 * comparison gets backwards. Returns >0 when `a` is newer. */
static int version_cmp(const wchar_t *a, const wchar_t *b) {
    while (*a || *b) {
        int va = 0, vb = 0;
        while (*a >= L'0' && *a <= L'9') va = va * 10 + (*a++ - L'0');
        while (*b >= L'0' && *b <= L'9') vb = vb * 10 + (*b++ - L'0');
        if (va != vb) return va - vb;
        if (*a == L'.') a++;
        if (*b == L'.') b++;
        if (!*a && !*b) break;
        /* A non-numeric suffix ("-rc1") stops the comparison rather than being
         * guessed at: a release-candidate scheme we do not use should not
         * silently read as newer. */
        if ((*a && (*a < L'0' || *a > L'9')) ||
            (*b && (*b < L'0' || *b > L'9'))) break;
    }
    return 0;
}

/* Pull "tag_name":"v0.12.0" out of the JSON without a parser. The response is
 * one known field from one known endpoint; linking a JSON library to read a
 * version string would be the larger mistake. */
static bool extract_tag(const char *json, wchar_t *out, size_t cap) {
    const char *k = strstr(json, "\"tag_name\"");
    if (!k) return false;
    k = strchr(k + 10, '"');
    if (!k) return false;
    k++;
    if (*k == 'v' || *k == 'V') k++;   /* tags are conventionally v-prefixed */

    char buf[64];
    size_t n = 0;
    while (*k && *k != '"' && n < sizeof(buf) - 1) buf[n++] = *k++;
    buf[n] = '\0';
    if (!n) return false;

    MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, (int)cap);
    return true;
}

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

static DWORD WINAPI update_thread(LPVOID param) {
    (void)param;

    HINTERNET ses = NULL, con = NULL, req = NULL;
    char     *body = NULL;

    ses = WinHttpOpen(L"mshell/" MSHELL_VERSION_W,
                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto out;

    /* Short timeouts: this is a background nicety, and a hung connection must
     * not keep a thread alive across a shutdown. */
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);

    con = WinHttpConnect(ses, UPDATE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) goto out;

    req = WinHttpOpenRequest(con, L"GET", UPDATE_PATH, NULL,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE);
    if (!req) goto out;

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto out;
    if (!WinHttpReceiveResponse(req, NULL)) goto out;

    DWORD cap = 16384, used = 0, got = 0;
    body = (char *)malloc(cap);
    if (!body) goto out;

    do {
        if (used + 4096 >= cap) break;      /* the tag is near the front */
        if (!WinHttpReadData(req, body + used, 4096, &got)) break;
        used += got;
    } while (got > 0);
    body[used] = '\0';

    wchar_t latest[64];
    if (extract_tag(body, latest, 64) &&
        version_cmp(latest, MSHELL_VERSION_W) > 0) {
        wchar_t msg[NOTIFY_TEXT_CAP];
        _snwprintf(msg, NOTIFY_TEXT_CAP - 1,
                   L"mshell %ls is available (you have %ls)",
                   latest, MSHELL_VERSION_W);
        msg[NOTIFY_TEXT_CAP - 1] = L'\0';

        /* The notification is raised on the main thread — every overlay is. */
        log_msg(LOG_INFO, L"update: %ls", msg);
        if (g.message_window)
            PostMessageW(g.message_window, WM_MSHELL_UPDATE, 0,
                         (LPARAM)_wcsdup(msg));
    } else {
        log_msg(LOG_INFO, L"update: up to date");
    }

out:
    free(body);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return 0;
}

void update_check_async(void) {
    if (!g.update_check) return;
    if (checked_today()) return;

    HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);   /* fire and forget: it reports by PostMessage */
}
