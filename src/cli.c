#include "mshell.h"
#include "cli.h"

static const char *flag_scan(const char *cmdline, const char *flag, bool *found) {
    if (found) *found = false;
    if (!cmdline) return NULL;

    size_t flen = strlen(flag);
    for (const char *p = cmdline; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;

        if ((size_t)(p - start) != flen || strncmp(start, flag, flen) != 0)
            continue;

        if (found) *found = true;
        while (*p == ' ' || *p == '\t') p++;
        return *p ? p : NULL;
    }
    return NULL;
}

bool cli_has_flag(const char *cmdline, const char *flag) {
    bool found = false;
    flag_scan(cmdline, flag, &found);
    return found;
}

const char *cli_flag_value(const char *cmdline, const char *flag) {
    return flag_scan(cmdline, flag, NULL);
}

static int run_displays(const char *cmdline) {
    (void)cmdline;
    display_list();
    return 0;
}

static int run_tweaks(const char *cmdline) {
    const char *arg = cli_flag_value(cmdline, "--tweaks");
    wchar_t     group[64] = {0};
    char        verb[32]  = {0};

    if (arg) sscanf(arg, "%31s %63ls", verb, group);

    if (!verb[0] || !strcmp(verb, "list")) {
        tweaks_list();
        return 0;
    }
    if (!strcmp(verb, "apply")) {
        char msg[128];
        snprintf(msg, sizeof msg, "applied %d tweaks",
                 tweaks_apply(group[0] ? group : NULL));
        console_print(msg);
        return 0;
    }
    if (!strcmp(verb, "revert")) {
        char msg[128];
        snprintf(msg, sizeof msg, "reverted %d tweaks",
                 tweaks_revert(group[0] ? group : NULL));
        console_print(msg);
        return 0;
    }
    if (!strcmp(verb, "reg") || !strcmp(verb, "reg-undo")) {
        tweaks_emit_reg(group[0] ? group : NULL, strcmp(verb, "reg-undo") == 0);
        return 0;
    }
    console_print("usage: mshell --tweaks <list|apply|revert|reg|reg-undo> "
                  "[input|visual|quiet|apps|all]");
    return 1;
}

static int run_check(const char *cmdline) {
    (void)cmdline;

    kb_locks_init();
    resolve_config_path(g.config_path, MAX_PATH);

    char msg[1024];
    char path_u8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, g.config_path, -1, path_u8,
                        (int)sizeof path_u8, NULL, NULL);

    if (config_load(g.config_path)) {
        snprintf(msg, sizeof msg,
                 "ok: %s\n  %d root bindings, %d keymaps, %d window rules, "
                 "%d desktop rules, %d startup programs",
                 path_u8,
                 g.cfg.keymaps->root ? g.cfg.keymaps->root->count : 0,
                 g.cfg.keymaps->count, g.cfg.rule_count,
                 g.cfg.desktop_rule_count, g.cfg.startup_count);
        console_print(msg);
        return 0;
    }
    snprintf(msg, sizeof msg,
             "FAILED: %s\n  %s\n  Nothing in this file would take effect: "
             "a config error is atomic.", path_u8, g.config_error);
    console_print(msg);
    return 1;
}

typedef struct {
    const char *flag;
    int (*run)(const char *cmdline);
} Subcommand;

static const Subcommand subcommands[] = {
    { "--displays", run_displays },
    { "--tweaks",   run_tweaks   },
    { "--check",    run_check    },
};

bool cli_run_subcommand(const char *cmdline, int *exit_code) {
    for (size_t i = 0; i < sizeof subcommands / sizeof subcommands[0]; i++) {
        if (!cli_has_flag(cmdline, subcommands[i].flag)) continue;
        *exit_code = subcommands[i].run(cmdline);
        return true;
    }
    return false;
}
