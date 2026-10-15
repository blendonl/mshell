#pragma once

#include <stdint.h>

#define MSHELLD_PROTO_VERSION  5u
#define MSHELLD_PIPE_PREFIX    L"\\\\.\\pipe\\mshelld-"

typedef enum {
    PROTO_HELLO    = 1,
    PROTO_SETPOS   = 2,
    PROTO_ZORDER   = 3,
    PROTO_CLOSE    = 5,
    PROTO_OK       = 6,
    PROTO_FAIL     = 7,
    PROTO_SHOW     = 8,   /* ShowWindow(hwnd, (int)flags) on a higher-integrity window */
    PROTO_SETSTYLE = 9,   /* set GWL_STYLE/GWL_EXSTYLE, then frame-change, cross-integrity */
} ProtoType;

typedef struct {
    uint32_t type;
    uint32_t version;

    uint64_t hwnd;
    int32_t  x, y, w, h;
    uint32_t flags;

    /* PROTO_SETSTYLE: the whole GWL_STYLE / GWL_EXSTYLE words to assert.
       64-bit because LONG_PTR is 64-bit on the only architecture we build. */
    int64_t  style;
    int64_t  exstyle;
} ProtoMsg;
