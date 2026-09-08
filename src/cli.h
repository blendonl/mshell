#pragma once

bool        cli_has_flag(const char *cmdline, const char *flag);
const char *cli_flag_value(const char *cmdline, const char *flag);

bool cli_run_subcommand(const char *cmdline, int *exit_code);
