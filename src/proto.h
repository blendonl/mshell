#pragma once

/*
 * proto.h — the wire format between mshell.exe and mshelld.exe.
 *
 * WHY THERE IS A HELPER AT ALL
 *
 * Windows blocks a lower-integrity process from moving a higher-integrity
 * window (UIPI). So an unelevated mshell cannot tile Task Manager, regedit, or
 * an elevated terminal — those windows just float. The obvious fix, running
 * mshell itself elevated, is the one thing 0.8.0 argued against: mshell reads
 * and executes init.lua, so an elevated mshell makes your config
 * administrator-level code living in a user-writable directory.
 *
 * mshelld.exe is the narrow way out. It is elevated, and it is deliberately
 * stupid: no Lua, no config file, no scripting, no window policy of its own. It
 * accepts one request — "put this window at this rectangle" — and performs it.
 * All the decisions stay in the unelevated shell.
 *
 * WHAT IS DELIBERATELY *NOT* HERE
 *
 * The keyboard hook stays in mshell.exe, even though moving it would also fix
 * keybinds while an elevated window has focus. Two reasons, and they are the
 * whole design:
 *
 *   - A low-level hook must return its verdict synchronously, inside
 *     LowLevelHooksTimeout. Asking the shell "should I swallow this?" over a
 *     pipe, per keystroke, cannot meet that reliably — and a hook that misses
 *     the deadline leaks the swallowed Win key, which is the exact bug the
 *     dedicated hook thread exists to prevent.
 *   - The alternative is to move the whole keymap state machine — submaps,
 *     leader, persist/one-shot, exit keys — into the helper as compiled data.
 *     That would put a config-derived automaton inside the elevated process,
 *     which is a far larger and more interesting surface than a list of
 *     rectangles, and it substantially defeats the point of keeping the
 *     elevated half dumb.
 *
 * So the split buys: mshell runs unelevated, your config is never elevated
 * code, and elevated windows are tiled like any other. What it does not buy is
 * keybinds while an elevated window has focus — which becomes the only reason
 * left to elevate mshell, and is documented as such in INSTALL.md.
 *
 * The messages are fixed-size structs over a message-mode pipe. Both binaries
 * are built from this header by the same compiler in the same `make`, so there
 * is no packing or endianness question to get wrong — and a version field makes
 * a mismatched pair refuse each other rather than misread.
 */

#include <stdint.h>

#define MSHELLD_PROTO_VERSION  1u
#define MSHELLD_PIPE_PREFIX    L"\\\\.\\pipe\\mshelld-"

typedef enum {
    PROTO_HELLO = 1,   /* shell -> helper: version handshake            */
    PROTO_SETPOS,      /* shell -> helper: privileged SetWindowPos      */
    PROTO_OK,          /* helper -> shell: performed                    */
    PROTO_FAIL,        /* helper -> shell: refused or failed            */
} ProtoType;

typedef struct {
    uint32_t type;
    uint32_t version;

    /* PROTO_SETPOS. The HWND travels as a 64-bit integer because it is only
     * ever a handle value here — the helper does not interpret it, it passes it
     * to SetWindowPos and reports what happened. */
    uint64_t hwnd;
    int32_t  x, y, w, h;
    uint32_t flags;
} ProtoMsg;
