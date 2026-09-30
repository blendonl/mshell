#pragma once

#include <stdint.h>

#define MSHELLD_PROTO_VERSION  4u
#define MSHELLD_PIPE_PREFIX    L"\\\\.\\pipe\\mshelld-"

typedef enum {
    PROTO_HELLO  = 1,
    PROTO_SETPOS = 2,
    PROTO_ZORDER = 3,
    PROTO_CLOSE  = 5,
    PROTO_OK     = 6,
    PROTO_FAIL   = 7,
} ProtoType;

typedef struct {
    uint32_t type;
    uint32_t version;

    uint64_t hwnd;
    int32_t  x, y, w, h;
    uint32_t flags;
} ProtoMsg;
