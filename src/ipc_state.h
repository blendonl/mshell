#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   full;
} StrBuf;

void   sb_init(StrBuf *b, char *buf, size_t cap);
void   sb_addf(StrBuf *b, const char *fmt, ...);
void   sb_vaddf(StrBuf *b, const char *fmt, va_list ap);
void   sb_add_escaped(StrBuf *b, const char *utf8);
size_t sb_escape(char *out, size_t cap, const char *utf8);

enum { IPC_HDR_UNSUPPORTED = -1, IPC_HDR_OFF = 0, IPC_HDR_ON = 1 };

typedef struct {
    const char *name;
    int         id;
    int         count;
    const char *layout;
    int         monitor;
} IpcDesktop;

typedef struct {
    int         index;
    const char *device;
    const char *desktop;
    long        x, y, width, height;
    unsigned    dpi;
    int         refresh;
    int         rotation;
    int         hdr;
    bool        focused;
} IpcMonitor;

typedef struct {
    const char       *version;
    const IpcDesktop *desktops;
    int               desktop_count;
    int               current_desktop_id;
    const IpcMonitor *monitors;
    int               monitor_count;
    const char       *focused_title;
} IpcState;

bool ipc_state_build(char *out, size_t cap, const IpcState *st);
