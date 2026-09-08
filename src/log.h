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

void     log_init(const wchar_t *basename, LogLevel level);
void     log_shutdown(void);

void     log_set_level(LogLevel level);
LogLevel log_get_level(void);

bool     log_level_from_name(const char *name, LogLevel *out);

void     log_msg(LogLevel level, const wchar_t *fmt, ...);
void     log_vmsg(LogLevel level, const wchar_t *fmt, va_list ap);

#define log_err(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define log_w(...)   log_msg(LOG_DEBUG, __VA_ARGS__)

#endif
