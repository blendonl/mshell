#include "tests.h"
#include "../src/api_spec.h"
#include "../src/action_table.h"

#include <stdlib.h>

static int deliberately_unnamed(Action a) {
    return a == ACTION_LUA_CALL || a == ACTION_ENTER_SUBMAP;
}

#define ACTION_ROW(action, fn) [action] = 1,
static const char action_has_handler[ACTION_COUNT] = { ACTION_TABLE(ACTION_ROW) };
#undef ACTION_ROW

static void test_every_action_has_a_handler(void) {
    for (int i = 0; i < api_spec_count(); i++) {
        const ApiEntry *e = api_spec_at(i);
        if (e->kind != API_ACTION) continue;
        CHECK(action_has_handler[e->action],
              "'%s' has a spec row but no entry in ACTION_TABLE", e->path);
    }
}

static void test_handlers_are_named_actions(void) {
    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        if (!action_has_handler[a]) continue;
        CHECK(deliberately_unnamed((Action)a) ||
                  api_spec_by_action((Action)a) != NULL,
              "action %d has a handler but no spec row", a);
    }
}

static void test_every_action_has_a_row(void) {
    for (int a = ACTION_NONE + 1; a < ACTION_COUNT; a++) {
        if (deliberately_unnamed((Action)a)) {
            CHECK(api_spec_by_action((Action)a) == NULL,
                  "action %d should have no spec row", a);
            continue;
        }

        int n = 0;
        for (int i = 0; i < api_spec_count(); i++) {
            const ApiEntry *e = api_spec_at(i);
            if (e->kind == API_ACTION && e->action == (Action)a) n++;
        }
        CHECK(n == 1, "action %d has %d spec rows, want exactly 1", a, n);
    }
}

static void test_paths_are_unique(void) {
    for (int i = 0; i < api_spec_count(); i++) {
        const ApiEntry *e = api_spec_at(i);
        CHECK(e->path && e->path[0], "row %d has no path", i);
        CHECK(api_spec_by_path(e->path) == e,
              "path '%s' does not resolve back to its own row", e->path);

        for (int j = i + 1; j < api_spec_count(); j++)
            CHECK(strcmp(e->path, api_spec_at(j)->path) != 0,
                  "duplicate path '%s'", e->path);
    }
}

static void test_legacy_names(void) {
    for (int i = 0; i < api_spec_count(); i++) {
        const ApiEntry *e = api_spec_at(i);
        if (!e->legacy_name) continue;

        CHECK(e->kind == API_ACTION,
              "'%s' carries the legacy name '%s' but is not an action",
              e->path, e->legacy_name);
        CHECK(strchr(e->legacy_name, '.') == NULL,
              "legacy name '%s' should be flat", e->legacy_name);

        for (int j = i + 1; j < api_spec_count(); j++) {
            const char *other = api_spec_at(j)->legacy_name;
            CHECK(!other || strcmp(e->legacy_name, other) != 0,
                  "duplicate legacy name '%s'", e->legacy_name);
        }
    }
}

static void test_lookup_accepts_both_vocabularies(void) {
    for (int i = 0; i < api_spec_count(); i++) {
        const ApiEntry *e = api_spec_at(i);
        if (e->kind != API_ACTION) continue;

        CHECK(api_action_from_name(e->path) == e->action,
              "'%s' did not resolve to its own action", e->path);
        if (e->legacy_name)
            CHECK(api_action_from_name(e->legacy_name) == e->action,
                  "legacy '%s' did not resolve to its own action",
                  e->legacy_name);

        CHECK(api_action_path(e->action) != NULL,
              "action behind '%s' has no path back", e->path);
        CHECK(strcmp(api_action_path(e->action), e->path) == 0,
              "'%s' does not round-trip", e->path);
    }

    CHECK(api_action_from_name("no_such_action") == ACTION_NONE,
          "an unknown name should resolve to ACTION_NONE");
    CHECK(api_action_from_name("window.no_such_thing") == ACTION_NONE,
          "an unknown path should resolve to ACTION_NONE");
    CHECK(api_action_from_name("FOCUS_LEFT") != ACTION_NONE,
          "legacy lookup should stay case-insensitive");
}

static void test_parents_exist(void) {
    for (int i = 0; i < api_spec_count(); i++) {
        const char *path = api_spec_at(i)->path;

        char buf[128];
        CHECK(strlen(path) < sizeof buf, "path '%s' is too long", path);
        snprintf(buf, sizeof buf, "%s", path);

        char *dot = strrchr(buf, '.');
        if (!dot) continue;
        *dot = '\0';
        CHECK(api_spec_by_path(buf) != NULL,
              "'%s' has no row for its parent '%s'", path, buf);
    }
}

static void test_rows_with_children_are_tables(void) {
    for (int i = 0; i < api_spec_count(); i++) {
        const ApiEntry *e = api_spec_at(i);
        size_t n = strlen(e->path);

        int has_child = 0;
        for (int j = 0; j < api_spec_count(); j++) {
            const char *other = api_spec_at(j)->path;
            if (strncmp(other, e->path, n) == 0 && other[n] == '.') {
                has_child = 1;
                break;
            }
        }
        if (!has_child) continue;

        CHECK(e->kind == API_NAMESPACE || (e->flags & API_CALLABLE),
              "'%s' has children, so it must be a namespace or callable",
              e->path);
    }
}

static void test_flags_are_coherent(void) {
    for (int i = 0; i < api_spec_count(); i++) {
        const ApiEntry *e = api_spec_at(i);

        CHECK(!((e->flags & API_CONFIG_ONLY) && (e->flags & API_RUNTIME_ONLY)),
              "'%s' cannot be both config-only and runtime-only", e->path);
        CHECK(!((e->flags & API_PAYLOAD_STR) && (e->flags & API_PAYLOAD_NUM)),
              "'%s' cannot take both payload kinds", e->path);

        if (e->kind != API_ACTION) {
            CHECK(e->action == ACTION_NONE,
                  "'%s' is not an action but names one", e->path);
            CHECK(!(e->flags & API_REPEATABLE),
                  "'%s' is not an action, so a count means nothing", e->path);
        } else {
            CHECK(e->action != ACTION_NONE, "'%s' names no action", e->path);
        }

        CHECK(e->doc && e->doc[0], "'%s' has no documentation", e->path);
    }
}

static void test_repeatable_is_motion_only(void) {
    CHECK(api_action_repeatable(ACTION_FOCUS_LEFT), "focus should repeat");
    CHECK(api_action_repeatable(ACTION_NEXT_DESKTOP), "desktop step should repeat");
    CHECK(api_action_repeatable(ACTION_VOLUME_UP), "volume should repeat");
    CHECK(!api_action_repeatable(ACTION_QUIT), "quit must not repeat");
    CHECK(!api_action_repeatable(ACTION_SPAWN), "spawn must not repeat");
    CHECK(!api_action_repeatable(ACTION_CLOSE), "close must not repeat");
    CHECK(!api_action_repeatable(ACTION_SHUTDOWN), "shutdown must not repeat");
}

static void test_removed_names_point_somewhere_real(void) {
    for (const ApiRemoved *r = api_removed; r->removed; r++) {
        CHECK(r->replacement && r->replacement[0],
              "'%s' was removed with no replacement named", r->removed);
        CHECK(api_spec_by_path(r->replacement) != NULL,
              "'%s' points at '%s', which is not in the spec",
              r->removed, r->replacement);
        CHECK(api_spec_by_path(r->removed) == NULL,
              "'%s' is listed as removed but still exists", r->removed);
        CHECK(strcmp(api_removed_replacement(r->removed), r->replacement) == 0,
              "lookup of '%s' did not return its replacement", r->removed);
    }
    CHECK(api_removed_replacement("window.close") == NULL,
          "a live name must not look removed");
}

static void test_payload_index(void) {
    const ApiEntry *to_desktop = api_spec_by_path("window.move.to_desktop");
    CHECK(to_desktop != NULL, "window.move.to_desktop should exist");
    CHECK((to_desktop->flags & API_TAKES_WINDOW) &&
          (to_desktop->flags & API_PAYLOAD_STR),
          "window.move.to_desktop takes both a window and a name");
    CHECK(api_payload_index(to_desktop, false) == 1,
          "with no window passed, the name is the first argument");
    CHECK(api_payload_index(to_desktop, true) == 2,
          "with a window passed, the name follows it");

    const ApiEntry *focus = api_spec_by_path("desktop.focus");
    CHECK(focus && !(focus->flags & API_TAKES_WINDOW),
          "desktop.focus takes no window");
    CHECK(api_payload_index(focus, false) == 1,
          "desktop.focus reads its name first");
    CHECK(api_payload_index(focus, true) == 1,
          "a row that takes no window never skips an argument");

    const ApiEntry *close = api_spec_by_path("window.close");
    CHECK(api_payload_index(close, true) == 2 &&
          api_payload_index(close, false) == 1,
          "the rule does not depend on there being a payload");

    CHECK(api_payload_index(NULL, false) == 1, "a missing row must not crash");
}

int main(void) {
    test_every_action_has_a_row();
    test_every_action_has_a_handler();
    test_handlers_are_named_actions();
    test_paths_are_unique();
    test_legacy_names();
    test_lookup_accepts_both_vocabularies();
    test_parents_exist();
    test_rows_with_children_are_tables();
    test_flags_are_coherent();
    test_repeatable_is_motion_only();
    test_removed_names_point_somewhere_real();
    test_payload_index();
    return tests_report("api_spec");
}
