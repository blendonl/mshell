# mshell — deep code review

Scope: all 45 files under `src/` (~16.5 kLOC of C), `Makefile`, `test/`,
`.github/workflows/`. Vendored Lua excluded. Reviewed at `c0d2cea`.

Verified during the review: `make test` passes (6 suites, ~2.4M assertions) and
`make CFLAGS_EXTRA=-Werror` builds clean. With `-Wshadow -Wformat=2` added there
are exactly 2 new warnings in first-party code.

---

## 1. What is already good

These are load-bearing and the plan below is designed to extend them, not
replace them.

**`api_spec.c/h` is the best idea in the codebase.** One declarative table
(`ApiEntry[]`) drives the Lua API tree, action-name lookup, IPC verb parsing,
repeat-count eligibility, config-vs-runtime enforcement, deprecation messages,
and — via `tools/gen_lua_meta.c` — the shipped editor type definitions, with CI
failing if `meta/` drifts. That is a genuinely well-factored registry.

**Pure logic is separated and unit-tested.** `layout_math`, `whichkey_math`,
`desktop_list`, `update_parse`, `match`, `api_spec` are free of Win32, compiled
with the host compiler, and covered by tests. `test/check_config.lua` even
executes the shipped configs and every README code block against a mock API.

**Failure messages are unusually good.** Errors explain the cause, the
consequence, and the remedy (`window_hide`'s SW_HIDE failure, the cloak
explanation, `window_placement_refused`). Most projects do not do this.

**Defensive backoff is applied consistently** — `foreground_bounce_allowed`,
`snap_tries`, `helper_note_timeout`, `dpi_settle_left`. The code repeatedly
chooses "stop fighting the app and log why" over infinite loops.

**`overlay.c` is a real abstraction**, reused by bar, notify, launcher,
whichkey, border, background, dim — double-buffered paint, DPI scaling, font
caching, class registration. No duplication there.

**CI is above average**: `-Werror`, unit tests, generated-artifact drift check,
and a dist smoke build on every push.

---

## 2. Correctness bugs

Ordered by severity. "Confirmed" = traced through the code; "latent" = correct
today only by accident of call order.

### 2.1 Out-of-bounds write in `ipc_build_state` — CONFIRMED

`src/ipc.c:44-105`. The function accumulates `o += (size_t)snprintf(out + o, cap - o, ...)`.
`snprintf` returns the length it *would* have written, so once one call
truncates, `o > cap`. The two loops guard with `o < cap`, but the four
unconditional calls (lines 61, 63, 92, 99) do not:

```c
o += (size_t)snprintf(out + o, cap - o, "],");   // o > cap  =>  cap - o underflows
```

`out + o` is past the end of `IpcRequest.reply[16384]` and `cap - o` wraps to
~`SIZE_MAX`, so `snprintf` writes 3 bytes into the heap past the allocation.

Reachable with enough desktops/monitors whose names expand under
`json_escape` (each name can escape to up to 1024 bytes, and there can be 32
desktops plus 8 monitors). Contrived but not impossible, and it is a plain
CWE-787.

Fix: a bounded append helper that saturates `o` at `cap` and refuses to write
once full — or return early. Every `snprintf`-accumulate site in the file has
the same shape.

### 2.2 `ConfigSnapshot` silently drops three config fields — CONFIRMED

`src/config.c:99-352`. Rollback of a failed config reload is implemented as a
100-field struct declared once, copied field-by-field in
`config_snapshot_save`, and copied back field-by-field in
`config_snapshot_restore` — three hand-maintained lists that must stay in sync
with `MShell` by eye.

They have already drifted. `mshell.mouse.setup` writes `g.mouse_speed`,
`g.mouse_accel` and `g.mouse_swap`; none of the three is in `ConfigSnapshot`.
So after a *failed* reload, `config_detach()` → `config_apply_defaults()` has
reset them to `0 / -1 / -1` and the restore does not put them back. The user's
configured pointer speed, acceleration and button-swap are silently lost, and
the next `mouse_sync_pointer()` writes the pre-mshell OS values back.

This is the single worst maintainability defect in the tree: ~250 lines of pure
duplication whose failure mode is silent. Every future config field is a coin
flip.

Fix: move every config-owned field of `MShell` into a nested `MShellConfig cfg`
struct. Save/restore then becomes `*s = g.cfg;` / `g.cfg = *s;` and cannot
drift. This is mechanical but touches every `g.<field>` reference, so it wants
its own phase.

### 2.3 The keyboard hook is held across arbitrary user Lua — CONFIRMED

`src/config.c:430-481`. `config_load()` takes `kb_lock()` and holds it across
`lua_register_api()`, `load_config_bytes()` and `lua_pcall()` — i.e. for the
entire execution of the user's `init.lua`.

`kb_hook_proc` takes the same lock on every key press. So while a config loads,
every keystroke in the session blocks. Windows enforces
`HKEY_CURRENT_USER\Control Panel\Desktop\LowLevelHooksTimeout` (default 300 ms):
if a low-level hook does not return in time, **Windows silently removes the
hook**. An `init.lua` that touches the network or a slow disk therefore has a
real chance of leaving mshell with no keyboard hook and no error.

Auto-reload makes this fire on every save of `init.lua`.

Fix: build the new keymaps into local storage with the lock *not* held, and take
`kb_lock()` only for the pointer swap at the end (and for the rollback path).

### 2.4 `helper_restart_thread` races the main thread over the pipe handle — CONFIRMED

`src/helper.c:269-302`. The restart worker calls `helper_disconnect()` (which
`CloseHandle(g_pipe)`), `helper_connect()` and writes `g_tried`, `g_timeouts`,
`g_blocked_until`. Meanwhile the main thread calls `helper_set_window_pos()` /
`helper_set_topmost()` from `tile_current()` and `window_enforce_zorder()`,
which read `g_pipe` and issue overlapped I/O on it.

`CloseHandle` on a handle another thread is using is a use-after-close; Windows
recycles handle values, so the main thread can end up writing a `ProtoMsg` into
an unrelated handle. No lock, no interlocked access, nothing.

Fix: guard the helper connection state with a critical section, or marshal the
reconnect back to the main thread via a window message (the codebase already
uses that pattern for `WM_MSHELL_UPDATE`).

### 2.5 The update path installs unverified code — CONFIRMED (security)

`src/update.c`:

- `http_get` only sets `WINHTTP_FLAG_SECURE` when the URL's scheme is HTTPS
  (line 54). A `browser_download_url` with `http://` is fetched in plaintext.
- If the release publishes no `digest`, the hash check is skipped entirely
  (line 437) and the download proceeds to unpack and run `install.bat`.
- An unrecognised digest prefix also skips verification (line 433).

So the worst case is: plaintext fetch, no integrity check, then
`cmd.exe /c "<unpacked>\install.bat"` — arbitrary code execution from a
network-position attacker. The asset name is also used unvalidated to build
`zip_path`/`root` (lines 449, 489), so a name containing path separators would
escape the temp directory. GitHub sanitises asset names today; the code should
not rely on that.

Fix: refuse non-HTTPS download URLs; refuse to install without a verified
`sha256:` digest; reject asset names containing `\`, `/` or `..`.

### 2.6 `mshelld.exe` is a UIPI bypass with no client authentication — BY DESIGN, should be hardened (security)

`src/mshelld.c` runs elevated and accepts `SETPOS` / `ZORDER` / `CLOAK` /
`CLOSE` for *any* `HWND` from anyone who can open the pipe. `pipe_sd.c` grants
`GA` to the current user's SID and SYSTEM — so any medium-integrity process
running as that user can ask a privileged process to move or close windows
owned by elevated processes. `PROTO_HELLO` is not a credential.

That is inherent to what the helper is for, but it should be narrowed:
`GetNamedPipeClientProcessId()` → check the client's image path is the
`mshell.exe` beside it → ideally check the Authenticode signature.

Separately, `serve()` uses a blocking `ReadFile` with no timeout and handles one
client at a time, so any process can wedge the helper permanently by connecting
and never writing.

### 2.7 `flush_placements` skips the show/hide pass when nothing was placed — CONFIRMED

`src/tiling.c:300`. `if (s_place_n <= 0) return;` sits *above* the loop that
hides `layout_hidden` windows and the loop that shows placed ones.

`tile_monitor` can legitimately produce zero placements — e.g. every client on
the monitor is screen-fullscreen and already covering it (line 238-242
`continue`s without emitting). When that happens, windows that a previous
monocle pass marked `layout_hidden` are never hidden, and windows that should
be shown are never shown. The visibility state gets stuck until some later tile
happens to emit something.

Related: `tile_monitor` returns at line 230 when `n == 0`, so a desktop whose
windows have all moved to another monitor never clears their stale
`layout_hidden`.

### 2.8 `smart_borders` counts windows on the wrong desktop — CONFIRMED

`src/border.c:41-58`. `monitor_visible_count(mon)` iterates
`desktop_current()->windows`, but `mon` is the *focused window's* monitor. On a
multi-monitor setup those differ whenever the focused monitor shows a different
desktop from `g.current_desktop_id`, so the border is hidden or shown based on
another display's window count.

Should iterate `desktop_by_id(desktop_on_monitor(mon))`.

### 2.9 `desktop_move_window` ignores the attach policy — CONFIRMED

`src/desktop.c:549-552` appends directly:

```c
new_dt->windows[new_dt->count] = hwnd;
```

`desktop_add_window` (line 597) correctly uses
`desktop_attach_index(g.attach_policy, ...)`. So `mshell.desktop.attach("master")`
is honoured when a window opens but ignored when you move a window to another
desktop, and again in the sticky-window transfer in `desktop_switch`
(line 422).

### 2.10 A monitor can be left showing no desktop — CONFIRMED

`src/desktop.c:436-438`. When swapping two desktops between monitors,
`desktop_place_on_monitor(prev_id, other)` is called with `prev_id == 0` if the
target monitor had nothing on it. That sets `g.monitor_desktop[other] = 0` and
returns early, leaving that display with only the backdrop until something else
calls `desktop_fill_monitors()`.

### 2.11 `layout_tree_set_container` loses the split orientation — CONFIRMED

`src/layout_tree.c:290`:

```c
p->mode = (p->mode == mode) ? SPLIT_V : mode;
```

Toggling a tabbed/stacked container back off always lands on `SPLIT_V`, even if
the container was `SPLIT_H` before. The previous orientation is not stored.

### 2.12 Blocking cross-process calls on the shell's message thread — CONFIRMED

`GetWindowTextW` sends `WM_GETTEXT` and blocks when the target is in another
process. The codebase knows this — `window_rule_lookup` uses
`SendMessageTimeoutW(..., SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, ...)` for exactly
this reason — but three hot paths use the blocking form:

- `src/bar.c:124` in `rebuild_content`, reached from `bar_refresh()`, which is
  called from `tile_current`, `window_focus` and `desktop_switch`.
- `src/lua_api.c:1569` (`window.title`) and `:1635` (`__tostring`).

One hung app therefore freezes the whole shell on every focus change.

Also on the message thread: `screenshot_screen()` encodes a full virtual-screen
PNG synchronously (`src/screenshot.c:122`) — tens of MB through WIC while the
UI is blocked.

### 2.13 `log_vmsg` fflushes on every line — CONFIRMED

`src/log.c:153`. A synchronous `fflush` per message. At `LOG_DEBUG`, the
`EVENT_OBJECT_LOCATIONCHANGE` hook alone produces a message per mouse-drag
frame, each costing a disk flush on the message thread.

### 2.14 Smaller confirmed issues

| Where | Issue |
|---|---|
| `main.c:754,766` | `if (!kb_init()) return 1;` — early returns after `background_init()`/`displays_apply_rules(true)` skip all teardown, leaving `SPI_SETFOREGROUNDLOCKTIMEOUT` changed and display modes applied. No unwind on partial init. |
| `main.c:675-698` vs `config.c:5-78` | Defaults are initialised in two places and have already drifted (`corner_pref`, `hide_policy`, `smart_gaps` are set in one and not the other). |
| `keyboard.c:1009` | `ACTION_CYCLE_LAYOUT` uses `if (next >= LAYOUT_BSP)`, so BSP is excluded from the cycle and adding a layout after BSP silently breaks it. |
| `keyboard.c:1017,1022` | `INC_NMASTER` clamps to `dt->count`, `DEC_NMASTER` clamps to `20`. Asymmetric. |
| `keyboard.c:319-341` | While the launcher is open the hook returns `1` for *every* key including key-ups, which can leave modifiers stuck down in other apps. |
| `keyboard.c:120` | `RWin` maps to `MOD_LWIN`; left and right Win cannot be bound separately. |
| `bar.c:178` | `draw_desktop_chips` re-parses the formatted string and detects "current" via `start[len-1] == L'*'`. A desktop literally named `foo*` reads as current. |
| `mouse.c:62-64,102` | `s_grab`, `s_grab_rect`, `s_resizing`, `g.mod_drag_hwnd` are written on the hook thread and read on the main thread with no synchronisation. |
| `mouse.c:108` | Every `WM_MOUSEMOVE` posts a `WM_MSHELL_MOUSE`; unthrottled, so a fast mouse floods the queue with `SetWindowPos` calls. |
| `launcher.c:86` | `s_indexed` is set once and never invalidated — newly installed programs never appear without restarting mshell. |
| `lua_api.c:1778` | `window.resize{...}` calls `window_set_pos(h, x, y, w, hh, 0)` — flags `0` means it activates and re-orders z, and it never updates `mw->applied_rect`, so the next tile fights it. |
| `update.c` | No `_snwprintf` return value is checked anywhere in the file (~15 sites); truncation silently produces malformed commands. |
| `notify.c` | Three different caps for the same text: `NOTIFY_TEXT_MAX` 512, `NOTIFY_TEXT_CAP` 512, `NotifyItem.text[256]`. `notify_recent` silently truncates 512→256. |
| `anim.c:148` | 8 layered top-level dim windows are created unconditionally even when `dim_enabled` is false. |
| `layout_tree.c:27` | `s_trees[MAX_DESKTOPS]` is ~900 KB of BSS reserved whether or not BSP is ever used. |
| `layout_tree.c:161-191` | `tree_sync` restarts a full O(nodes²) scan after each stale removal — up to ~67M iterations per `tile_current()` in the worst case. |
| `events.c:139` | `__attribute__((fallthrough))` is GCC-specific; the tree is otherwise MSVC-buildable. |
| `system.c:19` | `GetLastError()` is read after `AdjustTokenPrivileges` without `SetLastError(0)` first, so a stale error can produce a spurious warning. |
| `Makefile:CFLAGS` | `-s` strips symbols, so the `ExceptionAddress` printed by `mshell_crash_handler` can never be resolved. |

---

## 3. Architecture, SOLID, separation of concerns

### 3.1 Yes, there are god-files

**`keyboard.c` (1272 lines) is six modules in a trench coat:**

1. the VK name/number table and its lookups,
2. the `KeyMap`/`KeyBinding` data structure and allocator,
3. the low-level keyboard hook and the modifier state machine,
4. the lock-free-ish pending-action ring buffer between the hook thread and the
   message thread,
5. **`execute_action_on` — a 450-line switch that is the command dispatcher for
   the entire application**, reaching into window, desktop, layout, display,
   system, bar, launcher, update, helper and Lua,
6. the *mouse* hook installation and the hook thread's message pump.

Item 5 is the real problem. Every new feature adds a `case` to one function in
the file that owns keyboard input. That is a Single Responsibility violation and
an Open/Closed violation at the same time: the file must be edited to extend
behaviour that has nothing to do with keyboards.

**`window.c` (1786 lines)** holds rule matching, decoration stripping, four
distinct hide strategies (sink / cloak / stash / SW_HIDE), z-order banding,
DPI-settling placement, fullscreen modes, focus stealing, monitor following and
off-screen rescue. Each is coherent internally; together they are far too much
for one translation unit.

**`main.c`'s `WinMain` is 270 lines** doing CLI parsing for five subcommands,
default initialisation, singleton acquisition, ordered initialisation of 14
subsystems, the message loop, and teardown — with no unwinding on partial
failure (see 2.14).

**`events.c`'s `events_win_event_proc`** is one 230-line switch over 11 event
kinds, with `EVENT_OBJECT_LOCATIONCHANGE` alone containing ~90 lines of nested
policy. It also defines `mouse_drag_begin`/`mouse_drag_end`, which belong in
`mouse.c`.

### 3.2 The global `MShell g`

A single 200-field global holds config, runtime state, window handles, hook
handles, the Lua state and the monitor list, and every module reads and writes
it directly. There is no ownership boundary anywhere. This is what makes 2.2
possible (nothing distinguishes "config" from "state") and what makes almost
none of the code testable.

The realistic fix is not to eliminate `g` — that is a rewrite — but to
*partition* it: `g.cfg` (reloadable config), `g.rt` (runtime state), `g.ui`
(window handles). That alone fixes the snapshot problem structurally and makes
the ownership obvious.

### 3.3 Patterns that should be used and are not

**The registry pattern already in `api_spec` should be extended.** The codebase
proves the pattern works, then does not use it for:

- `execute_action_on`'s switch → a `handler` function pointer on `ApiEntry`, or
  a parallel dispatch table.
- `events_init`'s five copy-pasted `SetWinEventHook` blocks → a table of
  `{event_min, event_max, HWINEVENTHOOK *slot, const wchar_t *degradation_msg}`.
- `config_apply_defaults` / `config_snapshot_save` / `config_snapshot_restore` →
  a field-descriptor table, or (better) the sub-struct in 3.2.
- Subsystem init/teardown in `WinMain` → a table of `{name, init, shutdown}` so
  failure unwinds what succeeded.

**Strategy is hand-rolled where a table would do.** `window_hide` picks between
four hide strategies with nested `if`s and four booleans on `ManagedWindow`
(`cloaked`, `sunk`, `stashed`, `wm_hidden`) that must be kept mutually
consistent by hand across `window_hide`, `window_show`,
`window_restore_all_visibility`, `window_verify_visibility` and
`window_rehide_surfaced`. A `HideStrategy` enum plus a table of
`{try_hide, try_show}` would make the invariant enforceable.

**Hidden temporal coupling via module globals.** `tiling.c` sets the file-static
`s_inner` immediately before dispatching a layout, and `emit()` reads it — even
though `LayoutParams` (which *is* passed) already carries `inner`. Same shape in
`whichkey.c`, where a dozen file-statics carry state from `whichkey_show` to
`WM_PAINT`.

**Layering inversions.** `tile_current()` (layout) calls `border_refresh()` and
`bar_refresh()` (chrome). `border_refresh()` calls `anim_dim_refresh()`.
`bar_refresh()` calls `notify_recent()`. The dependency graph between the
overlay modules is a cycle rather than a hierarchy.

### 3.4 Duplication worth removing

- `flag_value()` and `has_flag()` in `main.c` are the same tokeniser twice.
- The `snap_tries` / 1-second-window backoff is implemented three times
  (`events.c` LOCATIONCHANGE, `window_float_moved`, `window_verify_placement`).
- The "get a field, clamp it, store it" Lua block is repeated ~60 times in
  `lua_api.c`; the file already has table-driven helpers for ints and colours in
  `set_whichkey` and does not use them elsewhere.
- Monitor-index clamping (`if (mon < 0 || mon >= g.monitor_count) mon = ...`)
  appears ~20 times.

---

## 4. Missing or under-implemented features

- **Tabbed and stacked containers are not really implemented.**
  `layout_tree.c` stores `active` as a single bit and `tree_place` renders
  `n->active ? n->b : n->a` — so a container holds exactly two tabs, and
  `SPLIT_TABBED` and `SPLIT_STACKED` render *identically*. There is no tab bar,
  no title stack, no visual indication a container is tabbed at all. In i3/sway
  terms this is a placeholder.
- **`cfact` (per-window size factor) is silently ignored** by the grid, spiral
  and BSP layouts; only the stack layouts honour it.
- **`g.next_split` is global**, not per-desktop or per-monitor, so "next split
  vertical" applies everywhere at once.
- **The launcher has no mouse support** (`WS_EX_NOACTIVATE`, no click handling)
  and cannot be dismissed by clicking away.
- **No scrolling in the launcher**: `LAUNCH_MAX_SHOWN` is 9 and there is no way
  to reach hit 10.
- **Notifications cannot be dismissed** — the window is `WS_EX_TRANSPARENT`.
- **`update_version_cmp` has no pre-release handling**: `1.0.0-beta` compares
  equal to `1.0.0`.
- **No per-monitor `next_split` / BSP tree persistence across a config reload** —
  `layout_tree_forget` drops the tree when a desktop is GC'd, which is right,
  but a reload silently resets manual layouts.
- **No `--version` flag** despite `MSHELL_VERSION` being threaded everywhere.

---

## 5. Testing

`make test` covers 6 modules, all of them pure: `match`, `layout_math`,
`whichkey_math`, `update_parse`, `desktop_list`, `api_spec`. Coverage is good
*within* those (2.3M assertions).

Everything else — `window.c`, `desktop.c`, `tiling.c`, `config.c`,
`keyboard.c`, `ipc.c`, `layout_tree.c`, `events.c` — has **zero automated
tests**, because every one of them touches `MShell g` and Win32 directly. That
is roughly 90% of the logic and every bug in section 2 except 2.1 and 2.5 lives
there.

The gap is structural, not a matter of writing more tests: the code has no seam
to test against. The highest-leverage change is to extract the decision logic
from the Win32 calls — `layout_tree`'s node algebra, `desktop_switch`'s
placement decisions, `window_hide`'s strategy selection, `ipc_build_state`'s
serialisation — into pure functions over plain structs, the way
`desktop_list.c` already does. Those are then testable on the host with no
Windows at all.

Also missing from CI: ASan/UBSan runs of the host tests, and any static
analysis (`cppcheck` / `clang-tidy`).

---

## 6. Summary

This is a well-built project by the standards of a solo Win32 shell replacement.
The declarative API spec, the pure-math extraction, the diagnostic quality of
the error messages and the `-Werror` CI are all better than typical.

The problems are concentrated and fixable:

1. Three memory/concurrency defects that are real and should be fixed first
   (2.1 IPC heap write, 2.4 helper handle race, 2.3 hook held across Lua).
2. One structural defect — the hand-copied `ConfigSnapshot` — that has already
   silently lost user settings and will do so again.
3. A security posture on the update path and the elevated helper that needs
   tightening.
4. Four god-files whose growth is unbounded because the dispatch is a `switch`
   rather than a table — the codebase already contains the better pattern.
5. A test suite that covers the 10% of code that was easy to make testable.

See `docs/PLAN.md` for the phased remediation.
