# mshell — remediation plan

Findings and rationale: `docs/REVIEW.md`.

Six phases. Phases 1–4 touch disjoint file sets and can run in parallel.
Phase 5 is a mechanical restructure that conflicts with everything, so it
merges last. Phase 6 is additive.

Every phase must leave `make CFLAGS_EXTRA=-Werror` and `make test` green, and
must not change observable behaviour except where the phase explicitly says so.

| Phase | Theme | Files owned | Parallel with |
|---|---|---|---|
| 1 | Memory safety & concurrency | `ipc.c`, `helper.c`, `mshelld.c` | 2, 3, 4 |
| 2 | Config reload correctness | `config.c`, `mshell.h`, `main.c` (defaults only) | 1, 3, 4 |
| 3 | Layout & visibility bugs | `tiling.c`, `layout_tree.c`, `border.c`, `desktop.c` | 1, 2, 4 |
| 4 | Responsiveness & update security | `bar.c`, `screenshot.c`, `log.c`, `mouse.c`, `update.c` | 1, 2, 3 |
| 5 | Break up the god-files | `keyboard.c`, `window.c`, `events.c`, `main.c` | merge last |
| 6 | Testable seams & CI | `test/`, `Makefile`, `.github/` | additive |

---

## Phase 1 — Memory safety and concurrency

**1.1 Fix the out-of-bounds write in `ipc_build_state`** (REVIEW 2.1).
Introduce a small bounded-append helper used by every accumulate site in
`ipc.c`, e.g.

```c
typedef struct { char *buf; size_t cap; size_t len; bool full; } StrBuf;
static void sb_addf(StrBuf *b, const char *fmt, ...);
```

`sb_addf` must clamp `len` at `cap - 1`, set `full`, and become a no-op
afterwards. Replace all `o += (size_t)snprintf(out + o, cap - o, ...)` in
`ipc_build_state`. Verify the loops can no longer walk past the buffer.

**1.2 Serialise access to the helper connection** (REVIEW 2.4).
`g_pipe`, `g_event`, `g_tried`, `g_timeouts`, `g_blocked_until` are touched from
the main thread and from `helper_restart_thread`. Either:

- guard them with a `CRITICAL_SECTION` held across `helper_connect`,
  `helper_disconnect` and `helper_exchange`; or
- (preferred, matches existing style) have `helper_restart_thread` do only the
  `schtasks` work and post a message to `g.message_window` for the
  disconnect/reconnect, so all pipe state stays on the main thread.

**1.3 Give `mshelld` a read timeout and reject unknown clients**
(REVIEW 2.6). In `serve()`, use overlapped I/O with a timeout (mirror
`helper_io` in `helper.c`) so a client that connects and never writes cannot
wedge the helper. In the accept path, call `GetNamedPipeClientProcessId`,
resolve the client's image path with `QueryFullProcessImageNameW`, and refuse
any client that is not the `mshell.exe` sitting in the same directory as
`mshelld.exe`. Log refusals.

**1.4** Add a short note to `INSTALL.md` stating plainly what trust the helper
grants (any process running as the user can ask it to move/close elevated
windows), since that is the whole point of it and users should know.

**Done when:** `make test` and `-Werror` pass; `--query` still returns valid
JSON with 32 desktops present; the helper survives a client that connects and
sends nothing.

---

## Phase 2 — Config reload correctness

**2.1 Partition `MShell`** (REVIEW 2.2, 3.2). Move every field that
`config_apply_defaults()` sets, plus `keymaps`/`rules`/`desktop_rules`/
`monitor_rules`/`startup_commands`/`lua_hooks`/`start_desktop` and their counts,
into a nested struct:

```c
typedef struct { /* everything a config reload owns */ } MShellConfig;
typedef struct { MShellConfig cfg; /* runtime state, handles, hooks … */ } MShell;
```

Then `ConfigSnapshot` collapses to `MShellConfig` and save/restore become
`s->cfg = g.cfg;` / `g.cfg = s->cfg;`. Delete
`config_snapshot_save`/`config_snapshot_restore`'s field lists entirely.

This renames a large number of `g.foo` → `g.cfg.foo` across the tree. Do it in
one mechanical pass, compile, and do not mix in behaviour changes.

*If the full partition proves too disruptive in one go*, the minimum acceptable
fix is adding `mouse_speed`, `mouse_accel`, `mouse_swap` to `ConfigSnapshot`
plus a compile-time guard — but the partition is the real fix and is preferred.

**2.2 Delete the duplicated defaults in `main.c`** (REVIEW 2.14). `WinMain`
lines ~675–698 duplicate a subset of `config_apply_defaults()` and have drifted.
Call `config_apply_defaults()` instead and remove the block.

**2.3 Stop holding `kb_lock` across user Lua** (REVIEW 2.3). Restructure
`config_load` so the Lua state is created, the API registered and `lua_pcall`
run with the lock *not* held, writing into a staging `MShellConfig`. Take
`kb_lock()` only around the final commit (or the rollback). The hook must never
block on `init.lua`.

Note the ordering hazard: `keymap_new`/`keymap_add_binding` write into `g` today,
so they will need to take the staging struct as a parameter.

**Done when:** a config with a deliberate 2-second `os.clock()` spin loads
without the keyboard hook being dropped; a failed reload preserves
`mouse.setup{speed=…}`; `make test` passes.

---

## Phase 3 — Layout and visibility bugs

**3.1 `flush_placements` must always run the show/hide pass** (REVIEW 2.7).
Move the `layout_hidden` hide loop and the `window_show` loop above the
`if (s_place_n <= 0) return;` guard, or drop the guard. Also make `tile_monitor`
clear stale `layout_hidden` for the desktop's windows even when it collects zero
clients for the monitor.

**3.2 Remove the `s_inner` hidden global** (REVIEW 3.3). `emit()` should take
the inset from `LayoutParams`, which already carries `inner`. Thread a small
context struct through `emit`/`tree_emit_cb` instead of file statics.

**3.3 Fix `smart_borders` on multi-monitor** (REVIEW 2.8). In
`monitor_visible_count`, iterate `desktop_by_id(desktop_on_monitor(mon))` rather
than `desktop_current()`.

**3.4 Honour the attach policy everywhere** (REVIEW 2.9). Route
`desktop_move_window`'s insertion and `desktop_switch`'s sticky-window transfer
through `desktop_add_window` (or at least through `desktop_attach_index`) so
`mshell.desktop.attach` behaves consistently.

**3.5 Never leave a monitor without a desktop** (REVIEW 2.10). In
`desktop_switch`, when the swap target monitor holds no desktop
(`prev_id == 0`), call `desktop_fill_monitors()` after the swap.

**3.6 Preserve the split orientation across a tabbed toggle** (REVIEW 2.11).
Store the pre-tabbed `SplitMode` on `TreeNode` and restore it in
`layout_tree_set_container` instead of hardcoding `SPLIT_V`.

**3.7 Make `tree_sync` linear** (REVIEW 2.14). Replace the restart-on-removal
loop with a single pass that collects stale HWNDs into a small array and then
removes them.

**3.8 Honour `cfact` in the grid and spiral layouts, or document that it is
stack-only.** Pick one and make the behaviour match the docs — silently ignoring
it is the current worst option.

**Done when:** `make test` passes; switching to monocle and back on a
two-monitor setup leaves no window stuck hidden; a tabbed container toggled off
returns to its previous orientation.

---

## Phase 4 — Responsiveness and update security

**4.1 Never block the message thread on another process's title** (REVIEW 2.12).
Add one shared helper — `bool window_title(HWND, wchar_t *, size_t)` using
`SendMessageTimeoutW(..., SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, ...)`, matching
`window_rule_lookup` — and use it in `bar.c:rebuild_content`,
`lua_api.c:push_window_field` and `lua_api.c:lua_window_tostring`.

**4.2 Move PNG encoding off the message thread** (REVIEW 2.12). `capture_rect`
should keep the `BitBlt` + clipboard on the main thread (both are fast and
clipboard ownership is thread-affine) and hand the pixel buffer to a worker
thread for `write_png`, notifying via `WM_MSHELL_UPDATE` as `update.c` and
`helper.c` already do.

**4.3 Stop flushing the log on every line** (REVIEW 2.13). Flush unconditionally
only for `LOG_ERROR`/`LOG_WARN`; for `INFO`/`DEBUG`/`TRACE`, flush at most every
N ms or every N lines, plus on `log_shutdown` and in the crash handler.

**4.4 Coalesce mod-drag mouse posts** (REVIEW 2.14). Drop a `WM_MOUSEMOVE` post
if one is already pending (track a flag cleared by the handler), or throttle to
~120 Hz. Also protect `s_grab`, `s_grab_rect`, `s_resizing` and
`g.mod_drag_hwnd` with `kb_lock()` — they cross the hook/main thread boundary.

**4.5 Harden the update path** (REVIEW 2.5). In `update.c`:

- refuse any download URL whose scheme is not HTTPS;
- refuse to install when no `sha256:` digest is present or when the digest
  algorithm is unrecognised — log clearly and stop, rather than proceeding;
- reject asset names containing `\`, `/`, `:` or `..` before using them to build
  `zip_path` / `root`;
- check `_snwprintf` return values on the command-line construction sites and
  abort on truncation.

**Done when:** `make test` passes; a build with a hung notepad in the foreground
still switches desktops instantly; a release with no digest is refused with a
clear message.

---

## Phase 5 — Break up the god-files

Mechanical restructuring only. **No behaviour changes.** Compile and run
`make test` after each individual move so a regression can be bisected.

**5.1 Split `keyboard.c` (1272 lines)** into:

- `src/keys.c` — the VK name table, `key_name_to_vk`, `vk_to_key_name`,
  `mod_name_to_flag`, `keymap_new`, `keymap_add_binding`, `keymap_find`.
- `src/input_hook.c` — `kb_hook_proc`, the modifier state machine,
  `current_mods`, `notify_submap`, the `PendingAction` ring
  (`dispatch`/`kb_take_pending`), the hook thread and `mouse_hook_proc`,
  `kb_init`/`kb_shutdown`/`kb_locks_init`/`kb_reset_state`.
- `src/actions.c` — `execute_action`, `execute_action_on` and its helpers
  (`focus_monitor`, `move_focused_to_monitor`, `float_nudge`, `adjust_cfact`,
  `resolve_target`, `neighbor_in_dir`, `parse_desktop_monitor`,
  `spawn_command`).

**5.2 Replace `execute_action_on`'s switch with a dispatch table** (REVIEW 3.1,
3.3). This is the point of the phase. Define

```c
typedef struct { HWND target; int arg; const wchar_t *command, *args, *cwd;
                 Desktop *dt; HWND focus; int fi; } ActionCtx;
typedef void (*ActionFn)(const ActionCtx *);
```

and a `static const ActionFn action_table[ACTION_COUNT]` populated by designated
initialisers (`[ACTION_FOCUS_LEFT] = act_focus_left,`). Add a compile-time or
startup assertion that every `Action` in `api_spec` has a handler — the
`api_spec` test already walks the whole enum and can assert this. Group the
tiny handlers (layout setters, media keys, system actions) so the file does not
balloon.

Adding an action then means: one `api_spec` row + one handler function, with no
edit to any dispatcher.

**5.3 Split `window.c` (1786 lines)** into:

- `src/window_rules.c` — `window_adopt_tier`, `window_rule_lookup`,
  `window_is_manageable`, `window_is_dialog`, `window_manage`, `window_promote`,
  `window_unmanage`, `window_grant_full`.
- `src/window_visibility.c` — the four hide strategies and their verification:
  `window_hide`/`window_show`/`window_sink`/`window_unsink`/`window_stash`/
  `window_unstash`/`window_set_cloaked`/`window_verify_visibility`/
  `window_verify_sink`/`window_rehide_surfaced`/`window_restore_all_visibility`.
  While here, introduce a `HideStrategy` enum and a table of
  `{name, try_hide, try_show}` so the four booleans on `ManagedWindow` stop
  being an implicit state machine (REVIEW 3.3).
- `src/window_place.c` — `window_set_pos`, `window_apply_rect`,
  `window_place_settled`, the DPI settling, `rect_clamp_into_monitor`,
  `window_center_float`, `window_rescue_offscreen`, `window_follow_monitor`,
  fullscreen handling.
- `src/window_zorder.c` — `window_set_band`, `window_raise_floats`,
  `window_enforce_zorder`, `window_resink`, `window_sink_intact`,
  `zorder_*`, and `window_focus`/`window_focus_none`.
- `src/window_decor.c` — decoration stripping/restoring and the frame props.

**5.4 Tidy `main.c`:**

- move the five CLI subcommands (`--displays`, `--tweaks`, `--check`, flag
  parsing) into `src/cli.c`, and de-duplicate `flag_value`/`has_flag` into one
  tokeniser;
- replace the init sequence with a table of `{const char *name, bool (*init)(void), void (*shutdown)(void)}`
  so `WinMain` initialises in order and, on failure, unwinds exactly the
  subsystems that succeeded (REVIEW 2.14) instead of `return 1`;
- restore `SPI_SETFOREGROUNDLOCKTIMEOUT` on every exit path.

**5.5 Tidy `events.c`:**

- give each `EVENT_*` its own `static void on_xxx(HWND)` and reduce
  `events_win_event_proc` to a lookup + call;
- replace the five copy-pasted `SetWinEventHook` blocks in `events_init` with a
  table (REVIEW 3.3);
- move `mouse_drag_begin`/`mouse_drag_end` to `mouse.c`.

**5.6 Factor the repeated `snap_tries` backoff** (REVIEW 3.4) into one helper
used by `events.c`, `window_float_moved` and `window_verify_placement`.

**Done when:** no file in `src/` exceeds ~600 lines; `make CFLAGS_EXTRA=-Werror`
and `make test` pass; `git diff --stat` shows moves, not rewrites; behaviour is
unchanged.

---

## Phase 6 — Testable seams and CI

**6.1 Extract pure logic and test it** (REVIEW 5). Follow the pattern that
`desktop_list.c` / `layout_math.c` already establish — a `.c` with no Win32,
compiled by `HOST_CC`, with a `test_*.c` beside it. Priority order:

1. `layout_tree`'s node algebra — `tree_insert`, `tree_remove`, `tree_place`,
   `tree_find` over a plain `TreeNode` pool with `HWND` as an opaque `void *`.
   This is self-contained and currently untested despite being the trickiest
   data structure in the tree.
2. `ipc_build_state`'s serialisation — pass in plain arrays of
   `{name, id, count, layout, monitor}` instead of reading `g`, so the Phase 1
   bounded-append fix gets a regression test including the overflow case.
3. The hide-strategy selection from Phase 5.3 — pure given a
   `{can_sink, can_cloak, can_stash, policy}` input.
4. `desktop_switch`'s monitor placement decisions — which desktop lands on which
   monitor, as a pure function over `monitor_desktop[]`.

**6.2 Run the host tests under sanitizers in CI.** Add a `test-asan` target
building the existing `TEST_BINS` with `-fsanitize=address,undefined
-fno-omit-frame-pointer -g` and run it in `build.yml` alongside `make test`.

**6.3 Add static analysis to CI.** `cppcheck --enable=warning,portability
--suppress=missingIncludeSystem src/` (excluding `vendor/`), non-blocking at
first, then promoted to blocking once the existing findings are cleared.

**6.4 Turn on the extra warnings that are already clean.** The tree builds with
only 2 `-Wshadow` findings (`main.c:447`, `keyboard.c:767`) and none from
`-Wformat=2`. Fix those two and add `-Wshadow -Wformat=2 -Wvla` to `CFLAGS`.

**6.5 Ship debug symbols.** `-s` in `CFLAGS` strips the binary, so the
`ExceptionAddress` the crash handler logs is useless. Build unstripped, run
`objcopy --only-keep-debug` into a `.debug` file, strip the shipped exe, and
attach the symbols to the release.

**Done when:** `make test` and `make test-asan` both pass in CI; cppcheck runs;
`-Wshadow -Wformat=2` are in `CFLAGS` with `-Werror` still green.

---

## Suggested order

```
        ┌── Phase 1 ──┐
        ├── Phase 2 ──┤
main ───┼── Phase 3 ──┼──> merge 1-4 ──> Phase 5 ──> Phase 6
        ├── Phase 4 ──┤
        └── Phase 6.2-6.4 (CI only, any time)
```

Phases 1–4 are independent. Phase 5 should rebase on all of them because it
moves the code they edit. Phase 6's CI items (6.2–6.4) can land at any point;
6.1's new tests are easiest after Phase 5's seams exist, but items 6.1.1 and
6.1.2 do not depend on it.
