#pragma once

#include <stdint.h>

#define MSHELLD_PROTO_VERSION  3u
#define MSHELLD_PIPE_PREFIX    L"\\\\.\\pipe\\mshelld-"

typedef enum {
    PROTO_HELLO = 1,
    PROTO_SETPOS,
    PROTO_ZORDER,
    PROTO_CLOAK,
    PROTO_CLOSE,
    PROTO_OK,
    PROTO_FAIL,
} ProtoType;

typedef struct {
    uint32_t type;
    uint32_t version;

    uint64_t hwnd;
    int32_t  x, y, w, h;
    uint32_t flags;
} ProtoMsg;
