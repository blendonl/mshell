#include "../src/ipc_state.h"
#include "tests.h"

#include <stdlib.h>

#define IPC_REPLY_MAX     16384
#define MAX_DESKTOPS      32
#define DESKTOP_NAME_MAX  64

static bool nul_terminated_within(const char *buf, size_t cap) {
    for (size_t i = 0; i < cap; i++)
        if (buf[i] == '\0') return true;
    return false;
}

static bool balanced_json(const char *s) {
    int  braces = 0, brackets = 0;
    bool in_str = false, esc = false;

    for (const char *p = s; *p; p++) {
        if (esc)                { esc = false; continue; }
        if (in_str) {
            if (*p == '\\')      esc = true;
            else if (*p == '"')  in_str = false;
            continue;
        }
        switch (*p) {
        case '"': in_str = true;  break;
        case '{': braces++;       break;
        case '}': braces--;       break;
        case '[': brackets++;     break;
        case ']': brackets--;     break;
        default: break;
        }
        if (braces < 0 || brackets < 0) return false;
    }
    return braces == 0 && brackets == 0 && !in_str && !esc;
}

static int count_occurrences(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

static void test_sb_respects_capacity(void) {
    char   buf[16];
    StrBuf b;

    sb_init(&b, buf, sizeof buf);
    CHECK(b.len == 0 && !b.full, "a fresh buffer is empty and not full");
    CHECK(buf[0] == '\0', "a fresh buffer is terminated");

    sb_addf(&b, "hello");
    CHECK(b.len == 5 && !b.full, "a short append fits");
    CHECK(strcmp(buf, "hello") == 0, "a short append is verbatim");

    sb_addf(&b, "%s", "0123456789012345678901234567890");
    CHECK(b.full, "an oversized append marks the buffer full");
    CHECK(b.len == sizeof(buf) - 1, "the length clamps at cap - 1");
    CHECK(buf[sizeof(buf) - 1] == '\0', "the buffer stays terminated");
    CHECK(strlen(buf) == sizeof(buf) - 1, "the buffer is exactly full");

    size_t after = b.len;
    sb_addf(&b, "more");
    CHECK(b.len == after, "appending to a full buffer is a no-op");
    CHECK(b.full, "the buffer stays full");
}

static void test_sb_exact_fit(void) {
    char   buf[6];
    StrBuf b;

    sb_init(&b, buf, sizeof buf);
    sb_addf(&b, "%s", "12345");
    CHECK(!b.full, "a value that exactly fills cap - 1 is not truncation");
    CHECK(b.len == 5, "the length is the whole value");
    CHECK(strcmp(buf, "12345") == 0, "the value is intact");

    char   tight[6];
    StrBuf c;
    sb_init(&c, tight, sizeof tight);
    sb_addf(&c, "%s", "123456");
    CHECK(c.full, "one byte over cap - 1 is truncation");
    CHECK(strcmp(tight, "12345") == 0, "the value is cut, not overrun");
}

static void test_sb_zero_capacity(void) {
    StrBuf b;
    char   sentinel = '!';

    sb_init(&b, &sentinel, 0);
    CHECK(b.full, "a zero-capacity buffer starts full");
    sb_addf(&b, "anything");
    CHECK(b.len == 0, "nothing is written to a zero-capacity buffer");
    CHECK(sentinel == '!', "a zero-capacity buffer is never touched");
}

static void test_escaping(void) {
    char out[64];

    sb_escape(out, sizeof out, "plain");
    CHECK(strcmp(out, "plain") == 0, "plain text is unchanged");

    sb_escape(out, sizeof out, "say \"hi\"");
    CHECK(strcmp(out, "say \\\"hi\\\"") == 0, "double quotes are escaped");

    sb_escape(out, sizeof out, "back\\slash");
    CHECK(strcmp(out, "back\\\\slash") == 0, "backslashes are escaped");

    sb_escape(out, sizeof out, "a\nb\tc");
    CHECK(strcmp(out, "a\\u000ab\\u0009c") == 0, "control bytes become \\u");

    sb_escape(out, sizeof out, "\x01\x1f");
    CHECK(strcmp(out, "\\u0001\\u001f") == 0, "the \\u form is lower-case hex");

    sb_escape(out, sizeof out, "");
    CHECK(out[0] == '\0', "an empty string escapes to empty");
}

static void test_escaping_never_splits_a_sequence(void) {
    for (size_t cap = 1; cap <= 24; cap++) {
        char out[32];
        memset(out, '@', sizeof out);

        size_t n = sb_escape(out, cap, "ab\ncd\"ef");
        CHECK(n < cap, "cap %zu: the result fits", cap);
        CHECK(out[n] == '\0', "cap %zu: the result is terminated", cap);
        CHECK(out[cap] == '@', "cap %zu: nothing is written past cap", cap);

        int backslashes = 0;
        for (size_t i = 0; i < n; i++) if (out[i] == '\\') backslashes++;
        CHECK(out[n - (n ? 1 : 0)] != '\\' || n == 0,
              "cap %zu: the result never ends on a lone backslash", cap);
        (void)backslashes;
    }
}

static void test_escaped_append_is_atomic_per_character(void) {
    for (size_t cap = 2; cap <= 20; cap++) {
        char   buf[24];
        StrBuf b;
        memset(buf, '@', sizeof buf);
        sb_init(&b, buf, cap);

        sb_add_escaped(&b, "x\ny\"z");

        CHECK(b.len < cap, "cap %zu: the length stays under cap", cap);
        CHECK(buf[b.len] == '\0', "cap %zu: the buffer is terminated", cap);
        CHECK(buf[cap] == '@', "cap %zu: nothing is written past cap", cap);
        CHECK(strlen(buf) == b.len, "cap %zu: len matches the content", cap);

        int trailing = 0;
        for (size_t i = b.len; i > 0 && buf[i - 1] == '\\'; i--) trailing++;
        CHECK(trailing % 2 == 0,
              "cap %zu: a truncated escape is dropped whole, not halved", cap);
    }
}

static IpcState small_state(IpcDesktop *dts, IpcMonitor *mons) {
    dts[0] = (IpcDesktop){"web", 1, 3, "tile", 0};
    dts[1] = (IpcDesktop){"code", 2, 1, "monocle", 1};

    mons[0] = (IpcMonitor){0, "\\\\.\\DISPLAY1", "web",
                           0, 0, 2560, 1440, 144, 165, 0, IPC_HDR_ON, true};
    mons[1] = (IpcMonitor){1, "\\\\.\\DISPLAY2", "code",
                           2560, 0, 1920, 1080, 96, 60, 90,
                           IPC_HDR_UNSUPPORTED, false};

    IpcState st = {"9.9.9", dts, 2, 1, mons, 2, "Terminal"};
    return st;
}

static void test_full_document(void) {
    IpcDesktop dts[2];
    IpcMonitor mons[2];
    IpcState   st = small_state(dts, mons);

    char buf[IPC_REPLY_MAX];
    CHECK(ipc_state_build(buf, sizeof buf, &st), "a small state fits");
    CHECK(balanced_json(buf), "the document is balanced");

    CHECK(strstr(buf, "\"version\":\"9.9.9\"") != NULL, "the version is present");
    CHECK(strstr(buf, "\"name\":\"web\",\"current\":true,\"windows\":3,"
                      "\"layout\":\"tile\",\"monitor\":0") != NULL,
          "the current desktop is marked current");
    CHECK(strstr(buf, "\"name\":\"code\",\"current\":false") != NULL,
          "the other desktop is not marked current");
    CHECK(strstr(buf, "\"hdr\":true") != NULL, "HDR on serialises as true");
    CHECK(strstr(buf, "\"hdr\":null") != NULL,
          "unsupported HDR serialises as null");
    CHECK(strstr(buf, "\"device\":\"\\\\\\\\.\\\\DISPLAY1\"") != NULL,
          "the device path is escaped");
    CHECK(strstr(buf, "\"focused\":\"Terminal\"}") != NULL,
          "the focused title closes the document");
    CHECK(count_occurrences(buf, "{\"name\":") == 2, "both desktops appear");
    CHECK(count_occurrences(buf, "{\"index\":") == 2, "both monitors appear");
}

static void test_hdr_off_and_no_focus(void) {
    IpcDesktop dts[2];
    IpcMonitor mons[2];
    IpcState   st = small_state(dts, mons);
    mons[0].hdr      = IPC_HDR_OFF;
    st.focused_title = NULL;

    char buf[IPC_REPLY_MAX];
    CHECK(ipc_state_build(buf, sizeof buf, &st), "the state fits");
    CHECK(strstr(buf, "\"hdr\":false") != NULL, "HDR off serialises as false");
    CHECK(strstr(buf, "\"focused\":null}") != NULL,
          "no focused window serialises as null");
    CHECK(balanced_json(buf), "the document is still balanced");
}

static void test_empty_state(void) {
    IpcState st = {"0.0.0", NULL, 0, 0, NULL, 0, NULL};

    char buf[IPC_REPLY_MAX];
    CHECK(ipc_state_build(buf, sizeof buf, &st), "an empty state fits");
    CHECK(strcmp(buf, "{\"version\":\"0.0.0\",\"desktops\":[],"
                      "\"monitors\":[],\"focused\":null}") == 0,
          "an empty state is exactly the empty document, got %s", buf);
    CHECK(balanced_json(buf), "the empty document is balanced");
}

static void test_titles_with_quotes_stay_parseable(void) {
    IpcDesktop dts[2];
    IpcMonitor mons[2];
    IpcState   st = small_state(dts, mons);
    st.focused_title = "he said \"go\" \\ then\nleft";

    char buf[IPC_REPLY_MAX];
    CHECK(ipc_state_build(buf, sizeof buf, &st), "the state fits");
    CHECK(balanced_json(buf), "quotes in a title do not break the document");
    CHECK(strstr(buf, "he said \\\"go\\\" \\\\ then\\u000aleft") != NULL,
          "the title is escaped in place");
}

static void long_name(char *out, size_t cap, char fill, int index) {
    size_t n = cap - 1;
    memset(out, fill, n);
    out[n] = '\0';
    out[0] = (char)('A' + (index % 26));
}

static void test_thirty_two_long_desktops_overflow_cleanly(void) {
    static IpcDesktop dts[MAX_DESKTOPS];
    static char       names[MAX_DESKTOPS][DESKTOP_NAME_MAX];
    static IpcMonitor mons[8];

    for (int i = 0; i < MAX_DESKTOPS; i++) {
        long_name(names[i], sizeof names[i], 'x', i);
        dts[i] = (IpcDesktop){names[i], i + 1, 99, "spiral", i % 8};
    }
    for (int i = 0; i < 8; i++)
        mons[i] = (IpcMonitor){i, "\\\\.\\DISPLAY-WITH-A-VERY-LONG-NAME",
                               names[i], 0, 0, 3840, 2160, 192, 240, 0,
                               IPC_HDR_ON, i == 0};

    IpcState st = {"0.15.3", dts, MAX_DESKTOPS, 1, mons, 8, names[0]};

    char buf[IPC_REPLY_MAX];
    memset(buf, '@', sizeof buf);
    bool ok = ipc_state_build(buf, sizeof buf, &st);

    CHECK(nul_terminated_within(buf, sizeof buf),
          "the reply is terminated inside the buffer");
    CHECK(strlen(buf) < sizeof buf, "the reply never runs past the buffer");
    if (!ok)
        CHECK(strlen(buf) == sizeof(buf) - 1,
              "a truncated reply fills the buffer exactly");
}

static void test_every_capacity_is_safe(void) {
    static IpcDesktop dts[MAX_DESKTOPS];
    static char       names[MAX_DESKTOPS][DESKTOP_NAME_MAX];
    static IpcMonitor mons[4];

    for (int i = 0; i < MAX_DESKTOPS; i++) {
        long_name(names[i], sizeof names[i], '"', i);
        dts[i] = (IpcDesktop){names[i], i + 1, i, "tile", i % 4};
    }
    for (int i = 0; i < 4; i++)
        mons[i] = (IpcMonitor){i, "dev\\ice", names[i], -1920, -1080,
                               3840, 2160, 96, 60, 270, IPC_HDR_OFF, false};

    IpcState st = {"0.15.3", dts, MAX_DESKTOPS, 3, mons, 4, names[1]};

    static char arena[IPC_REPLY_MAX + 64];
    for (size_t cap = 1; cap <= 4096; cap++) {
        memset(arena, '@', sizeof arena);
        bool ok = ipc_state_build(arena, cap, &st);

        CHECK(nul_terminated_within(arena, cap),
              "cap %zu: the reply is terminated inside the buffer", cap);
        CHECK(strlen(arena) < cap, "cap %zu: the reply fits", cap);
        CHECK(arena[cap] == '@', "cap %zu: nothing is written past cap", cap);
        CHECK(!ok, "cap %zu: a state this big cannot fit", cap);
    }
}

static void test_growing_capacity_reaches_a_complete_document(void) {
    IpcDesktop dts[2];
    IpcMonitor mons[2];
    IpcState   st = small_state(dts, mons);

    char full[IPC_REPLY_MAX];
    CHECK(ipc_state_build(full, sizeof full, &st), "the reference reply fits");
    size_t need = strlen(full);

    static char arena[IPC_REPLY_MAX + 64];
    bool        first_ok = false;
    size_t      first_ok_cap = 0;

    for (size_t cap = 1; cap <= need + 8; cap++) {
        memset(arena, '@', sizeof arena);
        bool ok = ipc_state_build(arena, cap, &st);

        CHECK(strlen(arena) < cap, "cap %zu: the reply fits", cap);
        CHECK(arena[cap] == '@', "cap %zu: nothing is written past cap", cap);

        if (ok && !first_ok) { first_ok = true; first_ok_cap = cap; }
        if (ok) {
            CHECK(strcmp(arena, full) == 0,
                  "cap %zu: a complete reply matches the reference", cap);
            CHECK(balanced_json(arena), "cap %zu: a complete reply parses", cap);
        }
    }

    CHECK(first_ok, "some capacity produces a complete reply");
    CHECK(first_ok_cap == need + 1,
          "the smallest complete capacity is the length plus the terminator, "
          "got %zu want %zu", first_ok_cap, need + 1);
}

static void test_truncation_stops_the_loops(void) {
    static IpcDesktop dts[MAX_DESKTOPS];
    static char       names[MAX_DESKTOPS][DESKTOP_NAME_MAX];

    for (int i = 0; i < MAX_DESKTOPS; i++) {
        long_name(names[i], sizeof names[i], 'z', i);
        dts[i] = (IpcDesktop){names[i], i + 1, 0, "tile", 0};
    }

    IpcState st = {"1.0.0", dts, MAX_DESKTOPS, 1, NULL, 0, NULL};

    char buf[512];
    CHECK(!ipc_state_build(buf, sizeof buf, &st), "the state does not fit");
    CHECK(strlen(buf) == sizeof(buf) - 1, "the buffer is filled, not overrun");

    int emitted = count_occurrences(buf, "{\"name\":");
    CHECK(emitted > 0, "at least one desktop made it in");
    CHECK(emitted < MAX_DESKTOPS, "the loop stopped once the buffer was full");
}

static void test_null_state(void) {
    char buf[64];
    CHECK(ipc_state_build(buf, sizeof buf, NULL), "a NULL state is handled");
    CHECK(strcmp(buf, "{}") == 0, "a NULL state is the empty object");
}

int main(void) {
    test_sb_respects_capacity();
    test_sb_exact_fit();
    test_sb_zero_capacity();
    test_escaping();
    test_escaping_never_splits_a_sequence();
    test_escaped_append_is_atomic_per_character();
    test_full_document();
    test_hdr_off_and_no_focus();
    test_empty_state();
    test_titles_with_quotes_stay_parseable();
    test_thirty_two_long_desktops_overflow_cleanly();
    test_every_capacity_is_safe();
    test_growing_capacity_reaches_a_complete_document();
    test_truncation_stops_the_loops();
    test_null_state();
    return tests_report("ipc_state");
}
