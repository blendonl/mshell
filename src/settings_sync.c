#include "mshell.h"
#include "settings_parse.h"

#define SETTINGS_PER_FIELD_MS  16000
#define SETTINGS_SLACK_MS      10000
#define SETTINGS_LINE_MAX      1024

static SRWLOCK  s_lock = SRWLOCK_INIT;
static wchar_t *s_pending;
static int      s_pending_fields;
static bool     s_running;

static bool append_utf8_arg(wchar_t *buf, size_t cap, size_t *used, const char *u8) {
    wchar_t wide[SETTING_TEXT_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, u8, -1, wide, SETTING_TEXT_MAX) <= 0) return false;
    size_t next = settings_append_arg(buf, cap, *used, wide);
    if (!next) return false;
    *used = next;
    return true;
}

static wchar_t *build_command(void) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return NULL;

    size_t   cap = MAX_PATH + 64 + (size_t)g.cfg.setting_assign_count * 128;
    wchar_t *cmd = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (!cmd) return NULL;

    size_t used = settings_append_arg(cmd, cap, 0, exe);
    bool   ok   = used != 0;
    if (ok) ok = (used = settings_append_arg(cmd, cap, used, L"--settings")) != 0;
    if (ok) ok = (used = settings_append_arg(cmd, cap, used, L"set")) != 0;

    for (int k = 0; ok && k < g.cfg.setting_assign_count; k++) {
        const SettingAssign *a = &g.cfg.setting_assigns[k];
        ok = append_utf8_arg(cmd, cap, &used, a->field->path) &&
             append_utf8_arg(cmd, cap, &used, a->text);
    }

    if (!ok) {
        free(cmd);
        return NULL;
    }
    return cmd;
}

static void log_child_line(const char *line, int *failures) {
    wchar_t wide[SETTINGS_LINE_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, SETTINGS_LINE_MAX) <= 0) return;

    if (strstr(line, ": FAILED")) {
        (*failures)++;
        log_err(L"settings: %ls", wide);
    } else if (strstr(line, " -> ")) {
        log_w(L"settings: %ls", wide);
    } else {
        log_msg(LOG_DEBUG, L"settings: %ls", wide);
    }
}

static void read_child_output(HANDLE pipe, int *failures) {
    char  line[SETTINGS_LINE_MAX];
    size_t len = 0;
    char  chunk[512];
    DWORD got = 0;

    while (ReadFile(pipe, chunk, sizeof chunk, &got, NULL) && got > 0) {
        for (DWORD k = 0; k < got; k++) {
            char c = chunk[k];
            if (c == '\r') continue;
            if (c == '\n' || len + 1 >= sizeof line) {
                line[len] = '\0';
                if (len) log_child_line(line, failures);
                len = 0;
                if (c == '\n') continue;
            }
            line[len++] = c;
        }
    }
    line[len] = '\0';
    if (len) log_child_line(line, failures);
}

static VOID CALLBACK kill_overdue(PVOID process, BOOLEAN fired) {
    (void)fired;
    TerminateProcess((HANDLE)process, 1);
}

static void notify_failures(int failures) {
    wchar_t msg[NOTIFY_TEXT_CAP];
    _snwprintf(msg, NOTIFY_TEXT_CAP - 1,
               L"%d Windows setting%ls from the config could not be applied — see mshell.log",
               failures, failures == 1 ? L"" : L"s");
    msg[NOTIFY_TEXT_CAP - 1] = L'\0';
    if (g.message_window)
        PostMessageW(g.message_window, WM_MSHELL_UPDATE,
                     MAKEWPARAM((WORD)NOTIFY_WARN, (WORD)12000), (LPARAM)_wcsdup(msg));
}

static void run_apply(wchar_t *cmd, int fields) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE read_end = NULL, write_end = NULL;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        log_err(L"settings: could not create a pipe: %lu", GetLastError());
        return;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    bool attrs_ok = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                              &write_end, sizeof write_end, NULL, NULL);

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb          = sizeof si;
    si.StartupInfo.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdOutput  = write_end;
    si.StartupInfo.hStdError   = write_end;
    si.lpAttributeList         = attrs_ok ? attrs : NULL;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL started = attrs_ok &&
        CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                       NULL, NULL, &si.StartupInfo, &pi);
    DWORD start_error = GetLastError();

    CloseHandle(write_end);
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    free(attrs);

    if (!started) {
        log_err(L"settings: could not start the settings worker: %lu", start_error);
        CloseHandle(read_end);
        return;
    }
    CloseHandle(pi.hThread);

    HANDLE timer = NULL;
    DWORD  limit = (DWORD)fields * SETTINGS_PER_FIELD_MS * 2 + SETTINGS_SLACK_MS;
    CreateTimerQueueTimer(&timer, NULL, kill_overdue, pi.hProcess, limit, 0, WT_EXECUTEONLYONCE);

    int failures = 0;
    read_child_output(read_end, &failures);
    CloseHandle(read_end);

    WaitForSingleObject(pi.hProcess, limit);
    if (timer) DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    if (code != 0 && failures == 0) failures = 1;
    if (failures > 0) notify_failures(failures);
    log_msg(LOG_INFO, L"settings: worker finished (exit %lu, %d failed)", code, failures);
}

static DWORD WINAPI apply_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        AcquireSRWLockExclusive(&s_lock);
        wchar_t *cmd    = s_pending;
        int      fields = s_pending_fields;
        s_pending = NULL;
        if (!cmd) s_running = false;
        ReleaseSRWLockExclusive(&s_lock);

        if (!cmd) return 0;
        run_apply(cmd, fields);
        free(cmd);
    }
}

void settings_sync(void) {
    if (g.cfg.setting_assign_count == 0) return;

    wchar_t *cmd = build_command();
    if (!cmd) {
        log_err(L"settings: the config's Windows settings do not fit on a command line");
        return;
    }

    AcquireSRWLockExclusive(&s_lock);
    free(s_pending);
    s_pending        = cmd;
    s_pending_fields = g.cfg.setting_assign_count;
    bool start       = !s_running;
    s_running        = true;
    ReleaseSRWLockExclusive(&s_lock);

    log_msg(LOG_INFO, L"settings: applying %d Windows settings from the config",
            g.cfg.setting_assign_count);
    if (!start) return;

    HANDLE thread = CreateThread(NULL, 0, apply_thread, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
        return;
    }

    log_err(L"settings: could not start the apply thread: %lu", GetLastError());
    AcquireSRWLockExclusive(&s_lock);
    s_running = false;
    ReleaseSRWLockExclusive(&s_lock);
}
