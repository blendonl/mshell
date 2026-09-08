#include "ipc_state.h"

#include <stdio.h>
#include <string.h>

void sb_init(StrBuf *b, char *buf, size_t cap) {
    b->buf  = buf;
    b->cap  = cap;
    b->len  = 0;
    b->full = (cap == 0);
    if (cap) buf[0] = '\0';
}

void sb_vaddf(StrBuf *b, const char *fmt, va_list ap) {
    if (!b || b->full || !b->buf || b->cap == 0) return;

    size_t room = b->cap - b->len;
    if (room <= 1) {
        b->full = true;
        return;
    }

    int n = vsnprintf(b->buf + b->len, room, fmt, ap);
    if (n < 0 || (size_t)n >= room) {
        b->len  = b->cap - 1;
        b->full = true;
        b->buf[b->len] = '\0';
        return;
    }

    b->len += (size_t)n;
}

void sb_addf(StrBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sb_vaddf(b, fmt, ap);
    va_end(ap);
}

static size_t escape_char(unsigned char c, char enc[6]) {
    if (c == '"' || c == '\\') {
        enc[0] = '\\';
        enc[1] = (char)c;
        return 2;
    }
    if (c < 0x20) {
        static const char hex[] = "0123456789abcdef";
        enc[0] = '\\';
        enc[1] = 'u';
        enc[2] = '0';
        enc[3] = '0';
        enc[4] = hex[(c >> 4) & 0xf];
        enc[5] = hex[c & 0xf];
        return 6;
    }
    enc[0] = (char)c;
    return 1;
}

size_t sb_escape(char *out, size_t cap, const char *utf8) {
    if (!out || cap == 0) return 0;

    size_t o = 0;
    for (size_t i = 0; utf8 && utf8[i]; i++) {
        char   enc[6];
        size_t n = escape_char((unsigned char)utf8[i], enc);
        if (o + n >= cap) break;
        memcpy(out + o, enc, n);
        o += n;
    }

    out[o] = '\0';
    return o;
}

void sb_add_escaped(StrBuf *b, const char *utf8) {
    if (!b || b->full || !b->buf || b->cap == 0) return;

    for (size_t i = 0; utf8 && utf8[i]; i++) {
        char   enc[6];
        size_t n = escape_char((unsigned char)utf8[i], enc);

        if (b->len + n >= b->cap) {
            b->full = true;
            b->buf[b->len] = '\0';
            return;
        }

        memcpy(b->buf + b->len, enc, n);
        b->len += n;
        b->buf[b->len] = '\0';
    }
}

bool ipc_state_build(char *out, size_t cap, const IpcState *st) {
    StrBuf b;
    sb_init(&b, out, cap);

    if (!st) {
        sb_addf(&b, "{}");
        return !b.full;
    }

    sb_addf(&b, "{\"version\":\"");
    sb_add_escaped(&b, st->version ? st->version : "");
    sb_addf(&b, "\",\"desktops\":[");

    for (int i = 0; i < st->desktop_count && !b.full; i++) {
        const IpcDesktop *d = &st->desktops[i];
        sb_addf(&b, "%s{\"name\":\"", i ? "," : "");
        sb_add_escaped(&b, d->name ? d->name : "");
        sb_addf(&b, "\",\"current\":%s,\"windows\":%d,\"layout\":\"",
                d->id == st->current_desktop_id ? "true" : "false", d->count);
        sb_add_escaped(&b, d->layout ? d->layout : "");
        sb_addf(&b, "\",\"monitor\":%d}", d->monitor);
    }
    sb_addf(&b, "],\"monitors\":[");

    for (int i = 0; i < st->monitor_count && !b.full; i++) {
        const IpcMonitor *m = &st->monitors[i];
        sb_addf(&b, "%s{\"index\":%d,\"device\":\"", i ? "," : "", m->index);
        sb_add_escaped(&b, m->device ? m->device : "");
        sb_addf(&b, "\",\"desktop\":\"");
        sb_add_escaped(&b, m->desktop ? m->desktop : "");
        sb_addf(&b, "\",\"x\":%ld,\"y\":%ld,\"width\":%ld,\"height\":%ld,"
                    "\"dpi\":%u,\"refresh\":%d,\"rotation\":%d,\"hdr\":%s,"
                    "\"focused\":%s}",
                m->x, m->y, m->width, m->height, m->dpi, m->refresh,
                m->rotation,
                m->hdr == IPC_HDR_UNSUPPORTED ? "null"
                    : m->hdr == IPC_HDR_ON    ? "true" : "false",
                m->focused ? "true" : "false");
    }
    sb_addf(&b, "],");

    if (st->focused_title) {
        sb_addf(&b, "\"focused\":\"");
        sb_add_escaped(&b, st->focused_title);
        sb_addf(&b, "\"}");
    } else {
        sb_addf(&b, "\"focused\":null}");
    }

    return !b.full;
}
