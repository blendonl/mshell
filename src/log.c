/* ===========================================================================
 * log.c — leveled, timestamped, rotating log.
 *
 * Three things this fixes over the previous inline logger:
 *
 *  - The file was opened "w", so every start truncated it. A crash left no
 *    evidence at all, which matters now that there is a crash handler whose
 *    job is to be diagnosable. It is opened "a" here and rotated by size.
 *  - It lived in %TEMP%, which cleaners empty. It moves to %LOCALAPPDATA%.
 *  - Writes were unsynchronised. The IPC server already logs from its own
 *    thread, so two threads could interleave mid-line; a CRITICAL_SECTION now
 *    covers the format-and-write.
 *
 * Kept from the old logger, deliberately: every line is flushed. As the shell
 * there is no console and no tray, so an unflushed tail lost to a hard kill is
 * exactly the evidence that was worth having.
 * =========================================================================== */
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* Rotate at 5 MB, keeping two older generations. A debug-level session is the
 * only thing that comes close to this; at the default level the file stays a
 * few lines long. */
#define LOG_MAX_BYTES  (5 * 1024 * 1024)
#define LOG_KEEP        2

static CRITICAL_SECTION s_cs;
static INIT_ONCE        s_once = INIT_ONCE_STATIC_INIT;

static FILE    *s_fp;
static LogLevel s_level = LOG_INFO;
static wchar_t  s_path[MAX_PATH];
static bool     s_inited;

static BOOL CALLBACK log_once_init(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    InitializeCriticalSection(&s_cs);
    return TRUE;
}

static void log_lock(void) {
    InitOnceExecuteOnce(&s_once, log_once_init, NULL, NULL);
    EnterCriticalSection(&s_cs);
}

static void log_unlock(void) {
    LeaveCriticalSection(&s_cs);
}

static const wchar_t *level_tag(LogLevel l) {
    switch (l) {
        case LOG_ERROR: return L"ERROR";
        case LOG_WARN:  return L"WARN ";
        case LOG_INFO:  return L"INFO ";
        case LOG_DEBUG: return L"DEBUG";
        default:        return L"TRACE";
    }
}

bool log_level_from_name(const char *name, LogLevel *out) {
    if (!name || !out) return false;
    if (!_stricmp(name, "error")) { *out = LOG_ERROR; return true; }
    if (!_stricmp(name, "warn"))  { *out = LOG_WARN;  return true; }
    if (!_stricmp(name, "info"))  { *out = LOG_INFO;  return true; }
    if (!_stricmp(name, "debug")) { *out = LOG_DEBUG; return true; }
    if (!_stricmp(name, "trace")) { *out = LOG_TRACE; return true; }
    return false;
}

void log_set_level(LogLevel level) {
    if (level < LOG_ERROR) level = LOG_ERROR;
    if (level > LOG_TRACE) level = LOG_TRACE;
    s_level = level;
}

LogLevel log_get_level(void) { return s_level; }

/* Build "<dir>\mshell\<basename>.log", creating the directory. Prefers
 * %LOCALAPPDATA%; falls back to %TEMP% so a session with neither still logs
 * somewhere rather than silently not at all. */
static bool log_resolve_path(const wchar_t *basename, wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH];

    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) &&
        !GetTempPathW(MAX_PATH, dir))
        return false;

    /* GetTempPathW leaves a trailing separator, the env var does not. */
    size_t n = wcslen(dir);
    if (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') {
        if (n + 1 >= MAX_PATH) return false;
        dir[n++] = L'\\';
        dir[n]   = L'\0';
    }

    if (_snwprintf(out, cap, L"%lsmshell", dir) < 0) return false;
    out[cap - 1] = L'\0';
    CreateDirectoryW(out, NULL);   /* ERROR_ALREADY_EXISTS is the normal case */

    if (_snwprintf(out, cap, L"%lsmshell\\%ls.log", dir, basename) < 0) return false;
    out[cap - 1] = L'\0';
    return true;
}

/* Shift .log -> .log.1 -> .log.2 and drop what falls off the end. Called with
 * the lock held and the stream already closed. */
static void log_rotate_locked(void) {
    wchar_t from[MAX_PATH + 8], to[MAX_PATH + 8];

    _snwprintf(to, MAX_PATH + 7, L"%ls.%d", s_path, LOG_KEEP);
    to[MAX_PATH + 7] = L'\0';
    DeleteFileW(to);

    for (int i = LOG_KEEP - 1; i >= 1; i--) {
        _snwprintf(from, MAX_PATH + 7, L"%ls.%d", s_path, i);
        _snwprintf(to,   MAX_PATH + 7, L"%ls.%d", s_path, i + 1);
        from[MAX_PATH + 7] = to[MAX_PATH + 7] = L'\0';
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    }

    _snwprintf(to, MAX_PATH + 7, L"%ls.1", s_path);
    to[MAX_PATH + 7] = L'\0';
    MoveFileExW(s_path, to, MOVEFILE_REPLACE_EXISTING);
}

void log_init(const wchar_t *basename, LogLevel level) {
    log_lock();
    if (s_inited) { log_unlock(); return; }

    log_set_level(level);

    if (log_resolve_path(basename, s_path, MAX_PATH)) {
        /* Rotate up front rather than mid-run: the size only matters at the
         * boundary, and doing it here means a fresh run always starts against
         * a file it is allowed to fill. */
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(s_path, GetFileExInfoStandard, &fad)) {
            ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (sz >= LOG_MAX_BYTES) log_rotate_locked();
        }
        s_fp = _wfopen(s_path, L"a, ccs=UTF-8");
    }

    s_inited = true;
    log_unlock();
}

void log_shutdown(void) {
    log_lock();
    if (s_fp) { fflush(s_fp); fclose(s_fp); s_fp = NULL; }
    s_inited = false;
    log_unlock();
}

void log_vmsg(LogLevel level, const wchar_t *fmt, va_list ap) {
    if (level > s_level) return;

    wchar_t body[1024];
    _vsnwprintf(body, 1023, fmt, ap);
    body[1023] = L'\0';

    SYSTEMTIME t;
    GetLocalTime(&t);

    wchar_t line[1152];
    _snwprintf(line, 1151,
               L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%ls] %ls",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, level_tag(level), body);
    line[1151] = L'\0';

    /* The debugger channel carries the bare message: DebugView stamps its own
     * time, so a second timestamp is just noise there. */
    OutputDebugStringW(body);
    OutputDebugStringW(L"\n");

    log_lock();
    if (s_fp) {
        fputws(line, s_fp);
        fputwc(L'\n', s_fp);
        fflush(s_fp);

        long pos = ftell(s_fp);
        if (pos >= LOG_MAX_BYTES) {
            fclose(s_fp);
            s_fp = NULL;
            log_rotate_locked();
            s_fp = _wfopen(s_path, L"a, ccs=UTF-8");
        }
    }
    log_unlock();
}

void log_msg(LogLevel level, const wchar_t *fmt, ...) {
    if (level > s_level) return;   /* cheap gate before touching varargs */
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(level, fmt, ap);
    va_end(ap);
}
