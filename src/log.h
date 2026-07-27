/* ===========================================================================
 * log.h — leveled, timestamped, rotating log shared by mshell.exe and
 *         mshelld.exe.
 *
 * Deliberately standalone: it pulls in nothing from mshell.h and keeps its
 * state in file statics rather than on the MShell global, because mshelld.exe
 * links exactly one translation unit of its own (see HELPER_SRCS in the
 * Makefile) and has no MShell at all. Anything that reached for `g` here would
 * make the helper unlinkable.
 *
 * Levels are ordered most-severe-first and a message is written when its level
 * is <= the active one, so raising the level widens the log.
 *
 *   ERROR  failures the user has to be able to diagnose
 *   WARN   something was wrong but was worked around
 *   INFO   lifecycle: startup, config loaded, shutdown
 *   DEBUG  per-keystroke matches, tiling passes, focus changes
 *   TRACE  reserved; nothing emits it yet
 *
 * log_err()/log_w() are macros over log_msg() so the ~85 existing call sites
 * keep working unchanged: log_err is ERROR (always written) and log_w is DEBUG
 * (written only once the level is raised, as --verbose used to do).
 * =========================================================================== */
#ifndef MSHELL_LOG_H
#define MSHELL_LOG_H

#include <windows.h>
#include <stdarg.h>
#include <stdbool.h>

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
    LOG_TRACE = 4,
} LogLevel;

/* Open %LOCALAPPDATA%\mshell\<basename>.log for append, rotating it first if
 * it has grown past the size cap. Falls back to %TEMP% when LOCALAPPDATA is
 * unset (a service account, say). Safe to call once; later calls are ignored.
 *
 * Logging works before this is called — it just goes to OutputDebugStringW
 * only, which is what a debugger sees. */
void     log_init(const wchar_t *basename, LogLevel level);
void     log_shutdown(void);

void     log_set_level(LogLevel level);
LogLevel log_get_level(void);

/* "error"/"warn"/"info"/"debug"/"trace", case-insensitive. False if unknown,
 * leaving *out untouched. */
bool     log_level_from_name(const char *name, LogLevel *out);

void     log_msg(LogLevel level, const wchar_t *fmt, ...);
void     log_vmsg(LogLevel level, const wchar_t *fmt, va_list ap);

#define log_err(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define log_w(...)   log_msg(LOG_DEBUG, __VA_ARGS__)

#endif /* MSHELL_LOG_H */
