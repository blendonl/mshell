# Changelog

All notable changes to mshell are documented here. This project adheres to
[Semantic Versioning](https://semver.org/) (pre-1.0: minor = features + fixes).

## Unreleased

## 0.15.5 — 2026-09-08

- refactor: split keyboard.c into keys, input_hook and actions
- refactor: dispatch actions through a table instead of a 450-line switch
- refactor: partition MShell into a nested MShellConfig
- fix: bound every append in the IPC state serialiser
- fix: serialise the helper pipe state across the restart thread
- fix: time mshelld client I/O out and only serve the mshell beside it
- docs: state what trust running the privileged helper grants
- fix: restore the whole config on a failed reload
- fix: layout and visibility bugs across tiling, borders and desktops
- fix: use config_apply_defaults() for WinMain's startup defaults
- perf(bar,lua): read window titles without blocking on hung apps
- refactor: split window.c into rules, decor, visibility, place and zorder
- perf(screenshot): encode the PNG on a worker thread
- perf(log): flush every error and warning, batch the rest
- perf(mouse): coalesce mod-drag posts and lock the shared drag state
- fix(update): require https and a verified sha256 before installing
- refactor: drive the four hide strategies from a table
- fix: load init.lua without holding the keyboard lock
- refactor: table-drive the window event dispatch and hook installation
- refactor: table-drive subsystem startup and split the CLI out of main.c
- test: add a sanitized run of the host test suite
- build: enable -Wshadow -Wformat=2 -Wvla on first-party sources
- ci: run cppcheck over src/ and fail on new findings
- build: ship debug symbols instead of stripping them away
- test: extract the layout tree node algebra behind a pure seam
- test: extract the IPC state serialiser behind a pure seam
- refactor: point layout_tree.c at the tested tree algebra
- refactor: point ipc.c at the tested state serialiser
- refactor: extract the hide-strategy selection behind a pure seam
- test: extract desktop_switch's monitor placement behind a pure seam
- chore: drop a stray symlink committed by accident

## 0.15.4 — 2026-09-08

- style: remove comments from source, build and packaging files

## 0.15.3 — 2026-09-08

### Added

- **Rotate a display — landscape, portrait, either one flipped.** A
  `monitor_rule` takes a `rotation`, as a name (`"landscape"`, `"portrait"`,
  `"landscape_flipped"`, `"portrait_flipped"`) or as degrees clockwise
  (`0`/`90`/`180`/`270`), alongside the `resolution`, `refresh` and `hdr` it
  already took. Two bindable actions go with it: `toggle_portrait` stands the
  focused display on its end and puts it back, and `cycle_rotation` steps
  through all four quarter turns (`1` clockwise, `-1` back, wrapping). Both
  name the new orientation in a notification.

  `resolution` still means the panel's own UNROTATED size, and so do the modes
  `--displays` prints — Windows states a rotated panel the other way round, as
  the swapped desktop size, and following it would have made a rule stop being
  true the moment the screen was turned. So `resolution = "2560x1440"` with
  `rotation = "portrait"` gives a 1440x2560 desktop and stays the right rule in
  either orientation; the two fields are set in one mode change rather than two,
  so the screen blanks once. A rotation goes through the same `CDS_TEST`
  validation and the same session-only path as a mode: nothing is written to
  Windows' stored display configuration, so booting without mshell gives the
  display back the way Windows has it.

  `--displays` now prints each display's orientation, and `--query` and
  `mshell.get_monitors()` report `rotation` per monitor.

- **Which display is primary, and where each one sits.** A `monitor_rule` takes
  `primary = true` and `position = {x, y}`, so the arrangement you would
  otherwise drag out in Settings is stated once and restated after a hotplug
  shuffles it. `--displays` prints each display's position in the same `+x+y`
  form the rule is written in.

  Positions are RELATIVE. Windows keeps the primary at (0,0) and states every
  other display from there, so mshell resolves the positions a config gives and
  then slides the whole arrangement until the primary lands on the origin —
  which is why making the right-hand monitor primary moves both of them, and why
  the coordinates can be written from whichever corner is easiest to think in.
  `CDS_TEST` bears the model out: moving the current primary off the origin on
  its own is refused, and the same move as part of a whole arrangement is not.

  This is the first thing here that is applied as one operation on the whole
  desktop rather than per display, and the second that PERSISTS. Batching the
  displays needs `CDS_NORESET`, which only means anything alongside
  `CDS_UPDATEREGISTRY`, and `CDS_SET_PRIMARY` has no transient form at all — so
  unlike a mode or a rotation, an arrangement is written to Windows' stored
  display configuration, exactly as `hdr` already was. Nothing is submitted
  unless the arrangement asked for differs from the one in force, so a satisfied
  rule costs nothing on reload; and a batch that is refused part-way is rolled
  back and re-applied, since the calls before it have already written the
  registry and abandoning them would leave an arrangement that appears at the
  next logon and never on screen.

- **Move a desktop to a monitor at runtime** — `desktop_to_monitor`, and
  `mshell.desktop_to_monitor([name, ] monitor)` from Lua. Which display a
  desktop lived on was decided once, by `desktop_rule`, at config-load time:
  `desktop_rule` refuses to run from a binding or a callback, and there was no
  action for it, so the one thing you could not do with a pin was change it
  without editing a file. The action takes a 0-based index and moves the desktop
  you are on; `--msg "desktop_to_monitor chat 1"` names one you are not standing
  on, which is the case a script wants. `-1` drops the pin and the rules decide
  again.

  A pin set this way outranks the rule and is kept alongside it rather than
  written over it, which is what lets it survive the two things that re-resolve
  a desktop: a config reload, and an unplug (the display going away lapses the
  pin for as long as it is gone, and replugging restores it). It lasts as long
  as mshell does — a restart goes back to what `desktop_rule` says.

### Changed

- **BREAKING: the config names actions as functions, not strings.** Every
  binding used to name its action with a string — `"focus_left"`, and
  `"switch_desktop"` with the desktop in a fourth argument. There were 96 of
  those names in one flat list, and 45 more `mshell.set_*` functions beside
  them. Nothing could complete them, nothing could check them, and a typo was
  discovered when the config loaded rather than when it was written.

  They are values now, grouped by what they act on:

  ```lua
  mshell.keys.bind({mod}, "h", mshell.window.focus.left)
  mshell.keys.bind({mod}, "3",
      function() mshell.desktop.focus("3") end, { desc = "3" })
  mshell.keys.bind({mod}, "x", "extra")            -- a string enters a submap
  ```

  Anything an action has to be TOLD — a desktop, a command — goes inside a
  function, which is where an argument can live. A function has no name for the
  which-key panel to label the key with, so those bindings take a `desc`; both
  shipped configs now pass one everywhere it matters, and a binding with no
  label shows `lua` instead of `?`.

  The old names are gone rather than aliased, but reading one says what
  replaced it — `mshell.set_gaps was removed — use mshell.layout.gaps` — and a
  config that fails still leaves the previous one running, so an upgrade cannot
  strand you. The full mapping is at the end of this entry.

- **The API is one table, and everything is generated from it.** The
  vocabulary used to be written out in four places that had to be kept in
  agreement by hand: the action-name table, a second switch listing which of
  those a repeat count applies to, the `luaL_Reg` table registering the Lua
  functions, and a sentence in the README. `src/api_spec.c` is now the only
  list. The Lua bindings, the `--msg` verbs, the which-key labels, the repeat
  filter and the editor type definitions are all derived from it, and
  `make test` fails if an action in the enum has no row.

  It deliberately depends on nothing but the C library, which is what lets the
  host compiler build it — the same reason `desktop_list.c` sits outside
  `mshell.h`.

- **`--msg` speaks both vocabularies.** `mshell.exe --msg close` still works,
  and `--msg window.close` now works too, because both resolve through the same
  table. Scripts written against the old names keep running.

### Added

- **Editor support: completion, signatures and diagnostics over the whole
  API.** mshell ships `meta/mshell.lua`, generated from `src/api_spec.c` by
  `tools/gen_lua_meta.c`, plus a hand-written `meta/types.lua` describing the
  option tables. `install.bat` puts both in `%APPDATA%\mshell\meta` with a
  `.luarc.json` pointing [lua-language-server][luals] at them, so opening the
  config folder in an editor gives completion over `mshell.*`, documentation on
  hover, and a warning on anything mshell does not have.

  Because they are generated from the table the binary dispatches through, they
  describe exactly the release you have installed. `make meta` regenerates
  them, and CI fails if the committed copy has drifted.

- **Windows are objects you can act on.** `mshell.window.get()` and the new
  `mshell.window.list(filter)` hand back windows rather than descriptions of
  them, and the window events do too. Every window verb takes the window to act
  on as an optional first argument, falling back to the focused one:

  ```lua
  for _, w in ipairs(mshell.window.list({ process = "firefox.exe" })) do
      w:move({ desktop = "web" })
  end
  ```

  Fields are read off the live window, so they are never stale, and every verb
  goes through the same dispatcher a keypress does — which is what keeps the
  tiling pass and the session bookkeeping from being skipped.

- **New queries and verbs**: `mshell.window.list`, `mshell.desktop.get`,
  `mshell.desktop.windows`, `mshell.monitor.current`, `mshell.monitor.focus`,
  `mshell.layout.get`, `mshell.display.modes`, `mshell.display.set_mode`,
  `mshell.window.center`, `mshell.window.promote`, and `mshell.exec` — the
  runtime twin of the config-time `mshell.exec.startup` (which was
  `mshell.spawn`), and the first way to start a program with arguments from
  anywhere but a keybinding.

- **The configs and the README are checked.** `make test` loads
  `config/init.lua`, `config/init.full.lua` and every Lua block in the README
  against a mock built from `api_spec.c`. Nothing validated them before, and
  the 1,300-line example config is exactly the kind of file that rots quietly.

[luals]: https://github.com/LuaLS/lua-language-server

#### Upgrading

Every removed name says what replaced it when a config reads it, so the
quickest migration is to load your config and follow the errors. The full
mapping, for reference:

<details>
<summary>Actions (the third argument to a binding)</summary>

| was | now |
|---|---|
| `bar_floating` | `mshell.bar.floating` |
| `bar_top` | `mshell.bar.top` |
| `close` | `mshell.window.close` |
| `container_next` | `mshell.layout.container.next` |
| `container_prev` | `mshell.layout.container.prev` |
| `cycle_layout` | `mshell.layout.cycle` |
| `cycle_refresh` | `mshell.display.refresh.cycle` |
| `cycle_rotation` | `mshell.display.rotation.cycle` |
| `dec_cfact` | `mshell.layout.cfact.shrink` |
| `dec_master` | `mshell.layout.master.ratio.shrink` |
| `dec_nmaster` | `mshell.layout.master.count.dec` |
| `desktop_to_monitor` | `mshell.desktop.to_monitor` |
| `focus_down` | `mshell.window.focus.down` |
| `focus_left` | `mshell.window.focus.left` |
| `focus_monitor_next` | `mshell.monitor.focus.next` |
| `focus_monitor_prev` | `mshell.monitor.focus.prev` |
| `focus_next` | `mshell.window.focus.next` |
| `focus_prev` | `mshell.window.focus.prev` |
| `focus_right` | `mshell.window.focus.right` |
| `focus_up` | `mshell.window.focus.up` |
| `fullscreen` | `mshell.window.fullscreen.window` |
| `fullscreen_both` | `mshell.window.fullscreen.both` |
| `fullscreen_content` | `mshell.window.fullscreen.content` |
| `hibernate` | `mshell.system.hibernate` |
| `inc_cfact` | `mshell.layout.cfact.grow` |
| `inc_master` | `mshell.layout.master.ratio.grow` |
| `inc_nmaster` | `mshell.layout.master.count.inc` |
| `jump_urgent` | `mshell.window.urgent.jump` |
| `kill` | `mshell.window.kill` |
| `last_desktop` | `mshell.desktop.focus.last` |
| `last_window` | `mshell.window.focus.last` |
| `launcher` | `mshell.launcher.open` |
| `layout_bsp` | `mshell.layout.bsp` |
| `layout_bstack` | `mshell.layout.bstack` |
| `layout_centered` | `mshell.layout.centered` |
| `layout_columns` | `mshell.layout.columns` |
| `layout_grid` | `mshell.layout.grid` |
| `layout_monocle` | `mshell.layout.monocle` |
| `layout_spiral` | `mshell.layout.spiral` |
| `layout_tiling` | `mshell.layout.tiling` |
| `lock` | `mshell.system.lock` |
| `logoff` | `mshell.system.logoff` |
| `mark_scratchpad` | `mshell.window.scratchpad.mark` |
| `media_next` | `mshell.media.next` |
| `media_play` | `mshell.media.play` |
| `media_prev` | `mshell.media.prev` |
| `media_stop` | `mshell.media.stop` |
| `minimize` | `mshell.window.minimize` |
| `move_down` | `mshell.window.move.down` |
| `move_left` | `mshell.window.move.left` |
| `move_right` | `mshell.window.move.right` |
| `move_to_desktop` | `mshell.window.move.to_desktop` |
| `move_to_monitor_next` | `mshell.window.move.to_monitor.next` |
| `move_to_monitor_prev` | `mshell.window.move.to_monitor.prev` |
| `move_up` | `mshell.window.move.up` |
| `next_desktop` | `mshell.desktop.focus.next` |
| `notify` | `mshell.notify` |
| `panic` | `mshell.config.panic` |
| `prev_desktop` | `mshell.desktop.focus.prev` |
| `promote_master` | `mshell.layout.master.promote` |
| `quit` | `mshell.config.quit` |
| `reboot` | `mshell.system.reboot` |
| `reload` | `mshell.config.reload` |
| `reset_cfact` | `mshell.layout.cfact.reset` |
| `resize_down` | `mshell.window.resize.down` |
| `resize_left` | `mshell.window.resize.left` |
| `resize_right` | `mshell.window.resize.right` |
| `resize_up` | `mshell.window.resize.up` |
| `restart_helper` | `mshell.config.restart_helper` |
| `restore` | `mshell.window.restore` |
| `rotate_split` | `mshell.layout.split.rotate` |
| `screenshot` | `mshell.screenshot.screen` |
| `screenshot_window` | `mshell.screenshot.window` |
| `shutdown` | `mshell.system.shutdown` |
| `sleep` | `mshell.system.sleep` |
| `spawn` | `mshell.exec` |
| `split_grow` | `mshell.layout.split.grow` |
| `split_h` | `mshell.layout.split.h` |
| `split_shrink` | `mshell.layout.split.shrink` |
| `split_v` | `mshell.layout.split.v` |
| `switch_desktop` | `mshell.desktop.focus` |
| `toggle_always_on_top` | `mshell.window.on_top.toggle` |
| `toggle_bar` | `mshell.bar.toggle` |
| `toggle_float` | `mshell.window.float.toggle` |
| `toggle_hdr` | `mshell.display.hdr.toggle` |
| `toggle_portrait` | `mshell.display.portrait.toggle` |
| `toggle_scratchpad` | `mshell.window.scratchpad.toggle` |
| `toggle_stacked` | `mshell.layout.container.stacked` |
| `toggle_sticky` | `mshell.window.sticky.toggle` |
| `toggle_tabbed` | `mshell.layout.container.tabbed` |
| `update` | `mshell.config.update` |
| `volume_down` | `mshell.media.volume.down` |
| `volume_mute` | `mshell.media.volume.mute` |
| `volume_up` | `mshell.media.volume.up` |
| `zoom` | `mshell.layout.master.zoom` |

</details>

<details>
<summary>Configuration functions</summary>

| was | now |
|---|---|
| `bind` | `mshell.keys.bind` |
| `block_system_keys` | `mshell.keys.block_system` |
| `desktop_rule` | `mshell.desktop.rule` |
| `desktop_to_monitor` | `mshell.desktop.to_monitor` |
| `get_current_desktop` | `mshell.desktop.current` |
| `get_desktops` | `mshell.desktop.list` |
| `get_focused_window` | `mshell.window.get` |
| `get_monitors` | `mshell.monitor.list` |
| `monitor_rule` | `mshell.monitor.rule` |
| `rule` | `mshell.window.rule` |
| `set_animation` | `mshell.appearance.animation` |
| `set_attach` | `mshell.desktop.attach` |
| `set_auto_reload` | `mshell.config.auto_reload` |
| `set_background` | `mshell.appearance.background` |
| `set_bar` | `mshell.bar.setup` |
| `set_border` | `mshell.appearance.border` |
| `set_dim` | `mshell.appearance.dim` |
| `set_float_on_top` | `mshell.window.float_on_top` |
| `set_float_placement` | `mshell.window.policy.placement` |
| `set_float_policy` | `mshell.window.policy.float` |
| `set_fullscreen_policy` | `mshell.window.policy.fullscreen` |
| `set_gaps` | `mshell.layout.gaps` |
| `set_hide_policy` | `mshell.window.policy.hide` |
| `set_layout` | `mshell.layout.set` |
| `set_leader` | `mshell.keys.leader` |
| `set_log_level` | `mshell.log.level` |
| `set_manage_owned` | `mshell.window.manage_owned` |
| `set_master_ratio` | `mshell.layout.master.ratio` |
| `set_min_window_size` | `mshell.window.min_size` |
| `set_minimize_policy` | `mshell.window.policy.minimize` |
| `set_mouse` | `mshell.mouse.setup` |
| `set_nmaster` | `mshell.layout.master.count` |
| `set_notify` | `mshell.notify.setup` |
| `set_smart_borders` | `mshell.appearance.smart_borders` |
| `set_smart_gaps` | `mshell.layout.smart_gaps` |
| `set_start_desktop` | `mshell.desktop.rule` |
| `set_update_check` | `mshell.config.update_check` |
| `set_urgency` | `mshell.appearance.urgency` |
| `set_verbose` | `mshell.log.verbose` |
| `set_whichkey` | `mshell.whichkey.setup` |
| `setenv` | `mshell.exec.setenv` |
| `spawn` | `mshell.exec.startup` |
| `submap` | `mshell.keys.submap` |

</details>

Three shapes changed beyond the rename:

- `mshell.bind(mods, key, action, payload)` lost its payload argument.
  Wrap the call: `function() mshell.desktop.focus("web") end`, and add
  `{ desc = "web" }` so which-key still has a label.
- `{"enter_submap", "name"}` is now just `"name"`, in a binding or a submap.
- A submap entry that carried a label was `{"action", {desc = "…"}}`; it is
  `{ mshell.path.to.action, desc = "…" }` — `desc` beside the action, not
  nested in a table of its own.


- **The `launcher` action prefers [mrun](https://github.com/notpc/mrun) when it
  is installed**, falling back to the built-in box when it is not. mrun is a
  separate application — its own binary, its own Lua config, a module system
  where app launching is the first module and clipboard history and emoji are
  the obvious next ones. mshell ships nothing of it and depends on nothing in
  it; it looks for `mrun.exe` beside `mshell.exe` and then on `PATH`. An
  existing binding needs no change to start using it, and none to keep working
  without it.

  Its window class is in the adoption ignore list, so it is never tiled.

### Fixed

- **A desktop moved to a monitor of a different scale arrived the wrong size.**
  Reported against Chrome, and it is every per-monitor-DPI-aware app: after
  `desktop_to_monitor`, or after switching to a desktop already up on the other
  display (the swap), a browser came back covering half of a portrait screen, or
  blown up past the edges of the other one and painted flat grey with nothing in
  it — and stayed that way until the browser was restarted.

  Moving a window to a display with a different scale factor raises
  `WM_DPICHANGED`, and the rect Windows suggests with it is the rect we asked
  for, *scaled by the ratio between the two DPIs*. Every Chromium window applies
  that suggestion, so a tile computed for the new monitor came out 1.5x too big
  going one way and a third of its size coming back. Two things then made it
  permanent rather than self-correcting:

  - An oversized window straddles both displays, and `MonitorFromWindow` answers
    with whichever it covers more of — which can still be the one it came from.
    A snap-back from there crosses the DPI boundary again and is scaled again,
    so the window oscillates, hits the three-attempts-per-second cap that exists
    to stop the shell locking up, and is left exactly where it had landed.
  - The whole layout pass goes out as one `DeferWindowPos` batch, and the app's
    answer to `WM_DPICHANGED` is its own `SetWindowPos`, re-entrant, in the
    middle of `EndDeferWindowPos`.

  A placement that crosses a DPI boundary is now recognised
  (`window_placement_crosses_dpi`) and handled differently in three ways: it is
  kept out of the deferred batch, it is never animated — a tween across the
  boundary re-triggers the whole thing every frame — and it goes through
  `window_place_settled`, which re-asserts the rect (with fresh frame insets,
  which are themselves DPI-scaled) until the window's DWM frame is within 4px of
  what the layout asked for, up to four attempts. The app's answer to the DPI
  change arrives between two of those, which is what overwrites it. The window
  is also placed with `SWP_NOCOPYBITS` and asked to repaint with
  `RDW_ALLCHILDREN`, since a Chromium window's content is a child HWND and what
  it has cached for the old scale is stale. A window that still will not settle
  says so in the log, by handle and by the size it was refusing, instead of
  going quiet.

  If a browser window is *already* stuck grey, nothing outside the app repairs
  it — restart it once, and apply `mshell.exe --tweaks apply apps` from an
  administrator prompt so Chromium stops deciding for itself whether anyone can
  see its windows (INSTALL.md).

- **Chrome, Discord and every other app that draws its own frame had a grey
  border down the left, right and bottom.** Around 10px at 150% scale, wider on
  a more scaled display, and much wider after the window crossed between two
  displays of different scale — at which point only closing and reopening the
  window cleared it.

  It was not a repaint and it was not the backdrop. Measured on the window
  itself: a Chrome window filling a 3840x2160 tile had `GetWindowRect` and the
  DWM frame both exactly on the tile, and a CLIENT area of 3820x2150 at +10,+0
  — inset 10px left, right and bottom, 0 at the top, painted `#202020`. That is
  Chromium's own client inset, `SM_CXSIZEFRAME + SM_CXPADDEDBORDER` at the
  window's DPI, which it applies because a window with a resize frame has that
  much invisible border to hide it in. mshell was stripping `WS_THICKFRAME`
  along with the caption, so there was no invisible border left and the inset
  became a visible band. Bigger after a cross-scale move because the inset is
  computed per monitor DPI, and permanent because nothing re-runs it.

  Stripping now keeps `WS_THICKFRAME` on a window that draws its own non-client
  area, recognised by measurement rather than by a list of applications: a
  window whose client area starts at the top of its window rect is claiming the
  caption for itself, which is exactly what Chromium, Electron and every other
  custom-frame toolkit does and what no ordinary Win32 window does. The band
  goes back to being the invisible resize border, `window_adjust_for_frame`
  already compensates for it so the VISIBLE frame lands on the tile, and the
  window is edge to edge with 1px of its own border. An ordinary app is stripped
  exactly as before — measured after the change: `chrome.exe` and `Discord.exe`
  keep the frame and land on their tiles, `alacritty.exe` does not have one and
  its window, frame and client rects stay identical.

  `tools/probe_frame.exe <hwnd>` prints those three rects, their insets and the
  colour runs across a window's edge, and toggles the style to A/B it live.

- **A window moved across a scale boundary settled short of its tile and stayed
  there.** The other half of the case above, and what was left of it after the
  synchronous re-assert landed: Chrome and Discord ended up narrower than the
  cell the layout gave them, with the backdrop showing down each side, and
  nothing put them right until the window was closed and reopened.

  `window_place_settled` writes the rect and reads it straight back, up to four
  times. That catches an app that resizes itself *inside* the `SetWindowPos`,
  which most do. A Chromium-class app does not: it takes the `WM_DPICHANGED`,
  re-lays itself out on its own thread, and resizes itself when that finishes —
  after the last of those four reads. Nothing looked again. The tiling pass was
  over, and the drift detector answered the app's resize by snapping back in the
  same tick, against a browser still mid-transition, until its
  three-attempts-per-second guard gave up and left the window where it had
  landed. Reopening the window worked because a window that opens on the display
  it belongs to never crosses a boundary at all.

  A placement that crossed a boundary now owes the window a second look. The
  250ms janitor tick that already retries hides and re-asserts the sink order
  (`window_verify_placement`) re-places it once the app has stopped moving —
  by then the transition is over and it is an ordinary same-display resize, the
  kind the same app accepts on every layout change. Bounded like every other
  fight in the shell: four chances, then the size the app insists on is recorded
  as the truth, so the layout stops claiming a window is somewhere it is not.

  `tools/probe_dpiband.exe` measures what is actually at a window's edge — the
  window rect, the DWM frame, the client area, both DPIs, and the colour runs
  across the window's edge — which is what separates a window placed short of
  its tile (the backdrop's colour) from an app insetting its own content (a
  frame colour of the app's own).

- **`--tweaks list` called a tweak applied when the write had been refused.**
  Applying reads the existing value into `HKCU\Software\mshell\TweakBackup`
  BEFORE writing the new one, so a revert can put back exactly what was there.
  The listing then read that backup — and only that backup — to decide what was
  applied. So a tweak whose write was refused, which is every tweak under
  `HKCU\Software\Policies` run without an administrator prompt, left a backup
  behind and was reported as applied for the rest of the install's life.

  That is the `apps` group in particular: the one group whose whole purpose is
  to be checked, because the symptom it prevents (a browser window that comes
  back flat grey) shows up hours later and looks nothing like a missing registry
  value. `install.bat` run unelevated skips the group, says so once, and the
  listing then disagreed with it forever after.

  Two changes. `--tweaks list` now reads the LIVE value and compares it to what
  the tweak wants, so `applied` means the setting is in force; a tweak with a
  backup whose value is not ours reports `failed`, with a line underneath
  saying to re-run from an administrator prompt. And `--tweaks apply` drops a
  backup it has just taken when the write that followed it was refused, so a
  failed apply leaves no trace to be misread later — an earlier, genuine backup
  is untouched.

- **The focus ring drawn on the display next door.** The ring is painted just
  OUTSIDE the window's frame, in the outer gap. With `set_gaps(0, 0)` there is
  no outer gap to paint in, so every edge landed off the display: the top edge
  under the bar, the right and bottom edges past the desktop's own boundary —
  and the left edge on the monitor to the left of it, as a single line down its
  right-hand side. The focused window had no ring at all, and the other display
  had a stray one.

  The ring is now clamped to the work area of the display the focused window is
  on, unioned with the window's own frame so a float parked over the whole
  monitor or straddling two displays is unaffected. An edge with no room outside
  the window is drawn just inside it instead of off the screen, which is what
  makes a zero-gap layout show a complete ring rather than none.

- **Every hidden window flashing onto the screen for a frame.** Anything that
  changes the virtual screen — a display plugged in, a mode or a rotation
  applied, a DPI change, and each of the retries that follow one — re-places the
  backdrop, and re-placing it sent it to `HWND_BOTTOM`. Hidden windows live
  BELOW the backdrop, which is what hiding them means, so bottoming it lifted
  every window on every desktop you are not looking at onto the screen at once;
  `window_resink` then put them back one `SetWindowPos` at a time, each its own
  composed frame. What you see is the whole session's windows appearing and
  vanishing again a split second later.

  The backdrop now keeps its place in the z-order (`SWP_NOZORDER`) whenever
  anything is sunk under it, and is moved only by `window_resink`, which lowers
  it to sit directly above the topmost sunk window — the one position that
  covers everything hidden and nothing else. `HWND_BOTTOM` is still what it gets
  when nothing is sunk, which is the case that flag was written for.

- **A window from another desktop staring back from the one you are on.**
  Switching to a desktop with nothing on it yet — the game desktop before the
  game is up — showed Discord, or a terminal, or whatever else happened to be
  top of the pile behind the backdrop. Those windows were not on that desktop.
  They were on no desktop at all.

  mshell strips `WS_CAPTION`, `WS_SYSMENU`, `WS_THICKFRAME` and the min/max
  boxes off the windows it tiles, which leaves a style with none of the
  `WS_OVERLAPPEDWINDOW` bits and no `WS_POPUP` — Chrome tiled by mshell reads
  `0x16000000`. The adoption test demanded one of the two, so a window mshell
  had already stripped failed it: a fresh mshell coming up over windows a
  previous one had tiled refused to manage a single one of them. That is every
  restart that does not run the shutdown path — a crash, a kill, a rebuild
  during development — and it does not heal, because nothing looks at a window
  a second time. A window that is not managed is on no desktop, so no desktop
  switch ever hides it, and it stays on screen across all of them; you only
  see it where the current desktop's own windows do not cover it, which is why
  an empty desktop was where it showed up.

  Three changes, addressing the adoption, the frame and the second chance:

  - The adoption test rejects `WS_CHILD` rather than demanding
    `WS_OVERLAPPEDWINDOW` or `WS_POPUP`. `GetAncestor(GA_ROOT)` has already
    established the window is top-level by then, and the caption-less popup
    heuristic still drops menus and tooltips — both of which are `WS_POPUP`.
    Apps that draw their own frame and never had those bits are adopted now
    too.
  - The frame a window had is recorded ON THE WINDOW, as the
    `mshell.orig_style` / `mshell.orig_exstyle` properties, and a startup sweep
    (`window_recover_frames`) hands it back before anything is adopted. A
    window a dead mshell left stripped gets its title bar back at the next
    start instead of keeping the flat look until its app is restarted; the
    window is then stripped again by the rules, this time with something that
    remembers what it started as.
  - A window that takes the foreground while unmanaged is adopted there and
    then. `EVENT_OBJECT_CREATE` and `EVENT_OBJECT_SHOW` are both dropped while
    a tiling pass or a desktop switch has events suppressed, and nothing swept
    up afterwards, so a window could miss adoption without any of the above
    happening.

- **A half-second black screen, every minute or two.** mshell's handler for
  `WM_SETTINGCHANGE` was a catch-all: any settings broadcast from anywhere in
  the session — a power scheme, a theme, an environment variable — re-showed the
  backdrop across the whole virtual screen and re-placed every managed window.
  That is expensive, and next to a borderless-fullscreen game it is worse than
  expensive: re-stacking an opaque window the size of the desktop drops DWM out
  of independent-flip presentation, and the trip out and back is the black
  screen.

  mshell was also its own loudest broadcaster. `window_focus()` re-asserts the
  foreground-lock timeout whenever `SetForegroundWindow` does not take, and the
  pointer settings are re-applied on every config reload; all of them pass
  `SPIF_SENDCHANGE`, which sends `WM_SETTINGCHANGE` to every top-level window in
  the session, mshell's own included. A contested focus change — an app raising
  itself off a fullscreen game, which is exactly when focus is contested — thus
  ended in a full re-tile that mshell had asked for itself.

  Three changes, each of which would have been enough on its own and none of
  which is sufficient alone:

  - The handler now acts only on the settings that actually move the layout:
    `SPI_SETWORKAREA` and `SPI_SETNONCLIENTMETRICS`. Everything else is logged
    at debug level and dropped. Display and DPI changes were never delivered
    this way — they arrive as `WM_DISPLAYCHANGE` and `WM_DPICHANGED`, which are
    handled separately and unchanged.
  - Every `SPIF_SENDCHANGE` mshell issues now goes through `spi_set_broadcast()`,
    which marks the broadcast as ours for its duration. The broadcast is a
    `SendMessage`, so it is delivered to our own message window inside that
    window, and the handler ignores it. mshell no longer re-tiles in response to
    itself.
  - `background_update()` re-places the backdrop only when it is actually hidden
    or the virtual screen has changed size, and adds `SWP_SHOWWINDOW` only when
    it is genuinely hidden. Repainting the backdrop no longer costs a
    full-screen `SetWindowPos`, and the z-order assert that has to run either
    way is one call that moves nothing.

  Reported as an intermittent black screen while playing with a second app that
  raises itself; the backdrop is black by default, so "mshell re-asserted the
  backdrop" and "the screen went black" are indistinguishable by eye. Setting
  `mshell.set_background()` to anything but black tells the two apart.

- **The windows of the desktop you left no longer appear on the one you are
  on when a game starts.** Reported against Valorant and seen with other apps
  that take the display exclusively fullscreen. A window mshell takes off the
  screen is *sunk* — dropped below the opaque backdrop in the z-order — so
  anything that lowers the backdrop, or reshuffles the bottom of the z-order,
  lifts every one of them back into view. Two things did:

  - `background_update()` moves the backdrop with `HWND_BOTTOM`, which by
    definition puts it under the windows sunk beneath it. Only the tiling pass
    put them back, and it does not run on every path that repaints the backdrop.
  - A display mode change reshuffles the order itself, and can refuse a z-order
    `SetWindowPos` outright while the transition is in flight — so the assert
    that rides on `WM_DISPLAYCHANGE` could silently do nothing, with no further
    event due to try again. The desktop stayed on the screen until you switched
    away and back, which re-hid it.

  The assert is now its own entry point (`window_resink`), re-run right after
  the backdrop moves, whenever an app takes the foreground, and on a short
  leash for a few seconds after a display change so it outlasts the transition.
  Z-order-only moves: no repaint, no `WM_SIZE`, nothing the app hears about.

- **A hidden window that surfaces anyway now puts itself back, whatever lifted
  it.** The fix above adds three more places that re-assert the sink, which is
  three more patches on the same shape of hole: hiding fired once and hoped, and
  nothing ever checked that the window was *still* off the screen. It is checked
  now, four times a second, and the check is nearly free because of where sunk
  windows live — walking UP from the bottom of the z-order meets all of them and
  then the backdrop, so it stops after a handful of steps, and after none at all
  when nothing is hidden. Reaching the backdrop with sunk windows unaccounted
  for means one came back, and it goes straight back down.

  This is what makes the desktop-switch bug class self-healing rather than
  enumerated: a trigger nobody has found yet costs a quarter-second of a window
  being visible instead of staying visible until you switch desktops and back.

- **A fullscreen game could never move to the display it asked for.** A window
  with the game preset (`float` + `fullscreen = true`) is parked over the
  monitor recorded against it, and `EVENT_OBJECT_LOCATIONCHANGE` re-asserted
  that *before* reading which display the window had just moved to. So a game
  that put itself on your second monitor was hauled straight back — and the
  handler then recorded the display it had been dragged to, which confirmed the
  stale index and made it self-reinforcing. Valorant in Fullscreen Windowed
  bounced between two displays in under 300 ms and settled on the wrong one,
  every time.

  The visible damage was to the pointer, not the window. A game confines the
  cursor to its own window rect; moving that window out from under the
  confinement leaves the pointer locked to a rectangle the game is no longer
  drawn in, so the cursor sits outside the game's borders — on a screen the
  game is not on, mid-round. The display is now read before the re-assert, so
  the game's own choice is what mshell parks it over. Minimized is not a move:
  an iconic window reports a rect at the origin, which would otherwise re-pin
  the game to whichever display owns it.

- **A window on a desktop you are not on can no longer take the keyboard.**
  Where DWM refuses to cloak, a hidden window is sunk instead — still visible,
  still composited, which is the entire point and also what makes it
  activatable. An app that raises itself for its own reasons (a download
  finishing, a media session) won the foreground from a desktop you were not
  looking at. `window_resink` put it back at the bottom of the z-order but not
  the focus, so the window you could see kept the screen while the one you
  could not kept the keyboard — worst over a fullscreen game, which lost its
  cursor confinement with it. Such a window is now bounced: it goes back under
  the backdrop and the focus returns to the current desktop's window.

- **A display change no longer leaves windows sized for the display that went
  away.** `WM_DISPLAYCHANGE` arrives while the topology is still in flight, so
  the work area was measured against an intermediate state and the tiling pass
  got a single shot at it — leaving, in one case, a window still 1440x2560 on a
  1920x1080 screen. The short leash that already re-asserts the z-order after a
  display change now re-measures and re-tiles on the same ticks. Idempotent by
  construction: a window already at its rect is skipped, and a window's display
  is only re-homed when it actually changed.

  Windows on *other* desktops are deliberately not covered — the tiler skips
  sunk and stashed windows — and come right when their desktop returns and they
  are raised again.

- **The focus ring was drawn on the monitor you had just left.** With a desktop
  up on each display, moving the focus across displays left the ring hugging the
  window you were no longer in, over on the other monitor — most visibly with
  mouse-follow focus, where it happened on every pointer crossing.

  `current_desktop_id` is derived from the focused display, so the two have to
  move together. `focus_monitor()` had always paired them, but `window_focus()`
  assigned `g.focused_monitor` on its own and then called `border_refresh()` two
  lines later — which asks `desktop_get_focused()`, which answers from the
  desktop that is still recorded as current, i.e. the one on the display you
  came from. The ring was placed around that window, correctly, in the wrong
  place. `bar_refresh()` read the same stale pair for its layout and title.
  Pairing the two where `g.focused_monitor` is actually assigned covers every
  path that crosses a display rather than only the keybind that had remembered
  to: mouse-follow, a closing window handing focus to a sibling, summoning the
  scratchpad, and jumping to an urgent window.

- **An app that minimised itself to the tray on a desktop you were not looking
  at came straight back out of it.** Close Discord or Slack to the tray while
  you are on another desktop, come back, and there it was again — on screen,
  untrayed, as if you had clicked its icon.

  `window_hide()` has four ways of taking a window off the screen, and only one
  of them — `ShowWindow(SW_HIDE)` — clears `WS_VISIBLE`, which is the bit
  `EVENT_OBJECT_HIDE` reports. So only that one can produce the event for a
  window *mshell* hid; sinking, cloaking and stashing are silent. The handler
  that decides "did we hide this, or did the app?" was still testing the single
  mechanism that was the first choice when it was written. Since 0.14.9 made
  sinking the first choice, essentially every hidden window is sunk — so every
  genuine tray-hide on a background desktop was discarded as mshell's own,
  `app_hidden` was never set, and the next switch back revealed the window
  mshell had been asked to leave alone. The question is now asked as
  `window_hidden_by_showwindow()`, which names all four mechanisms.

- **Closing a window stole the focus to its neighbour.** With four windows on a
  desktop and the third one focused, closing the *first* moved the focus to the
  fourth. The desktop's window list is `memmove`d down over the window that
  left, so everything above it slides down a slot — but `focused` is an index
  into that list and was only ever clamped back inside the array, never
  adjusted for the shift. It therefore came to name the window *after* the one
  you were in, and `window_unmanage()` duly focused it. The same arithmetic ran
  when a window was moved to another desktop and when a sticky window followed
  you off one. `desktop_focus_clamp()` is replaced by
  `desktop_focus_after_remove()`, which knows which index was removed; `make
  test` covers it, including a sweep over every combination of list length,
  focused index and removed index.

- **A config reload undid every layout you had changed since mshell started —
  and ignored the `desktop_rule` you had just edited.** Both halves were the
  same missing link. The remembered per-desktop settings were read from
  `session.txt` once at startup and never refreshed, while the file itself was
  rewritten from the live desktops on every change: so the in-memory copy that
  a reload re-applies was always a snapshot of launch time. And it was applied
  *after* the config's rules rather than before, so it overrode them — editing
  `desktop_rule("web", { layout = "monocle" })` and reloading did nothing at
  all.

  The remembered set is now refreshed the moment a value changes, and applied
  between the global defaults and the rules. The precedence that falls out is
  the one the documentation already claimed: a field no rule mentions keeps
  whatever you last set it to, at runtime and across a restart, while a field a
  rule *does* name is the config's to state and a reload is how you change your
  mind about it. `mshell.set_layout(...)` is a default, not a rule;
  `desktop_rule("*", { layout = ... })` is how a config insists.

- **A display change threw windows from other desktops onto the screen.** When
  neither sinking nor cloaking is available — a window its own app pinned
  topmost, on a machine where DWM refuses cross-process cloaking, which is most
  of them — mshell hides it by *stashing* it: moving it 4000 px clear of every
  display, remembering where it came from. The hotplug path then walked every
  floating window looking for ones stranded off-screen and hauled them back,
  because "off every display" is exactly what a stashed window looks like. It
  arrived on top of whatever desktop you were on, still flagged hidden, so
  nothing put it away again — and `WM_DISPLAYCHANGE` retries that sweep six
  times over the next three seconds. A stashed window is now recognised as
  filed rather than stranded. A monitor pin still reaches it, by moving the
  rect it will come back *to*; and that rect is now checked against the
  displays that actually exist at the moment it is unstashed, so a window whose
  monitor was unplugged while it was away comes back somewhere you can see it.

- **A desktop switch cost three times the window moves it needed.** Every
  tiling pass re-asserted the z-order of every hidden window on *every* desktop
  — one cross-process `SetWindowPos` each, and each one a synchronous round
  trip into another application's message loop — even though the desktop switch
  had just put those windows exactly there. With six desktops of six windows a
  single switch issued about ninety of those calls where thirty were the actual
  work. The repair now runs only when the bottom of the z-order is not already
  what it would produce; the backdrop is still pinned under everything on every
  pass, which is one call on one of our own windows.

  The pass that keeps floating windows on top had a related problem: it reads
  the system-wide z-order, and its only bound was how many floats it could
  *collect*, never how far it would walk. It therefore enumerated every
  top-level window on the machine — hundreds — doing a linear lookup on each,
  twice per desktop switch. It now knows from the desktop's own list how many
  floats it is looking for and stops when it has them.

- **One frozen application could freeze the whole shell mid-switch.** Every way
  of taking a window off the screen sends to that window's own thread and waits
  for it. A single application that had stopped pumping messages — on either
  the desktop you were leaving or the one you were arriving at — therefore
  blocked the desktop switch, and with it mshell's only message loop. Under a
  shell with no taskbar there is nothing left to escape to. Such a window is now
  skipped rather than waited on, and the hide or show it was owed is delivered
  by the same quarter-second housekeeping tick that watches the backdrop, as
  soon as its application answers again.

- **Which desktop you are on is no longer written to disk during the switch
  itself.** The session file was rewritten inline on every desktop switch —
  `fopen`, format, `fclose` on the message loop, which under an on-access virus
  scanner is tens of milliseconds on the most latency-sensitive thing mshell
  does. The write is now coalesced onto a two-second timer, and flushed
  outright on shutdown, on logoff and from the crash handler. Two seconds is
  the whole of what that trades: the file exists because `install.bat` upgrades
  by `taskkill /F`, and that is now the window in which such a kill can still
  lose your last change. The orderly shutdown never wrote the session at all
  before this — it did not have to, because every change wrote inline — so that
  flush is new.

### Changed

- **The "cloak was refused" diagnostic said the wrong thing, and sent you to
  install a helper that cannot fix it.** It read a refusal as an integrity
  boundary `mshelld.exe` should be able to cross. It is not one:
  `DWMWA_CLOAK` is **owner-only**, so no process cloaks another process's window
  with it, elevated or not. The cloak Windows' own virtual desktops use is a
  different API — `IApplicationView::SetCloak`, reached through
  `CLSID_ImmersiveShell` — and that class is registered at runtime by
  **explorer.exe**, which mshell replaces. Its registry key carries no
  `LocalServer32` and no `InprocServer32`, so COM has nothing to launch and
  `CoCreateInstance` answers `REGDB_E_CLASSNOTREG`. Measured on Windows 11
  build 26100 by `tools/probe_shellcloak.c`, which is in the tree so the result
  can be re-checked after a Windows update.

  The upshot: cloaking is permanently unavailable to mshell by construction, and
  sinking under the backdrop is not a fallback but the mechanism. Both the
  window.c and helper.c messages now say so instead of pointing at INSTALL.md.

### Added

- **`restart_helper` — a key that makes the `mshelld.exe` on disk the one that
  is running.** mshelld is started by a logon task and holds a singleton mutex,
  so dropping a new binary beside the old one changes nothing until the running
  one dies: a second copy launched over it exits immediately and leaves the OLD
  build serving. An update already handles that inside `install.bat`
  (`:helper_refresh`); this is the same stop/start for a binary put in place by
  hand, and for a helper that is alive but has stopped answering.

  No administrator prompt, despite the task being registered `/rl highest` — it
  belongs to this user, so Task Scheduler starts it at its registered level on
  mshell's behalf. It runs on its own thread (two waited-on `schtasks` calls with
  a settle between them is seconds, and that thread answers the keyboard), drops
  the pipe handle before the old helper dies, reconnects afterwards, and reports
  which of those happened. Bound to `Win` `x` `h` in `init.full.lua`.

- **`make probe`** builds `tools/probe_shellcloak.exe`, a console diagnostic that
  reports whether the immersive shell (and therefore the shell cloak and the
  native virtual-desktop API) is reachable from this process. It checks the
  `IApplicationView` IID before calling anything on the interface, so it never
  blind-calls a vtable slot that may have moved, and it always uncloaks what it
  cloaked — including from a console-Ctrl handler.

### Removed

- **BREAKING: session persistence is gone.** mshell no longer remembers a
  desktop's layout, master ratio, master count or monitor pin across a restart,
  and no longer returns you to the desktop you were last on. `session.c`, the
  2 s save timer and `%APPDATA%\mshell\session.txt` are all removed; an existing
  `session.txt` is simply ignored and can be deleted.

  Every start is now the one `init.lua` describes. A layout you change with
  `Win+Space` still lasts as long as mshell does, and a config reload still
  keeps it unless a `desktop_rule` names that field — what changed is only that
  a restart no longer carries it over.

  Two smaller things follow from it. A `desktop_rule`'s `default` now decides
  **every** start rather than only a first run, so `default = "always"` and
  `default = "remember"` no longer mean anything and are rejected with a message
  saying so — write `default = true`. And the config watcher no longer has to
  tell mshell's own writes apart from a config edit, because there are none:
  every change in the config folder is a config edit again.

## 0.15.2 — 2026-08-11

### Added

- **`bar_top` / `bar_floating` actions — switch the bar between modes at
  runtime.** Bindable like any other action; the shipped config wires them to
  `Win+x b t` and `Win+x b f` via a submap. Either action enables the bar if
  it was hidden.

### Fixed

- **Remove the dark grey border DWM draws on tiled windows.** Stripping
  decorations used to add `WS_BORDER` to give a thin separator between adjacent
  windows. DWM renders that as a ~4 px dark grey line on the left, right and
  bottom edges — visible on Chrome, Discord and every other tiled window. Gaps
  and the focus ring already handle visual separation, so the border is now
  removed unconditionally.

## 0.15.1 — 2026-08-11

### Fixed

- **A desktop and its windows can no longer disagree about who is on it.** Every
  window records the desktop it belongs to, and every desktop records the
  windows on it. Those are two halves of one fact, and four bugs came from
  writing one half without the other, or from acting on a window that was not
  on the screen at all.

  **A summoned scratchpad left a desktop behind that never went away.**
  `toggle_scratchpad` brought the window to the desktop you were looking at by
  changing which desktop the window *claimed*, and nothing else — so the desktop
  it came from still listed it. Closing it there never removed it from that
  list, which meant the desktop never ran out of windows, was never collected,
  and sat in the status bar for the rest of the session. Every summon-then-close
  cost another one.

  **A stowed scratchpad came back on its own.** Put it away, visit another
  desktop, come back, and it was on screen again. Returning to a desktop shows
  everything on it and lets the tiling pass put back whatever should not be
  visible — which works for tiled windows and cannot work for the scratchpad,
  because the tiler never places floating windows. It now stays where you put
  it until you ask for it, and re-marking a *different* window as the scratchpad
  hands the old one its visibility back rather than leaving it stranded.

  **Switching desktops un-minimised and un-trayed things.** Coming back to a
  desktop hands the keyboard to the window you were last using there — and did
  so without checking whether that window was still on the screen. Focusing a
  window restores it if it is minimised, and reaches for a stronger API when the
  first one is refused, which un-hides it. So a window you had minimised came
  back restored, and an app you had closed to the tray (Discord, Slack, Steam)
  was pulled back out of it. Focus now skips anything that is minimised or
  trayed; the `restore` binding still reaches a minimised window, which is what
  it is for.

  **A window opened onto a full desktop disappeared.** At 256 windows the
  desktop stopped accepting them silently: the window was adopted, told which
  desktop it was on, hidden if that desktop was not the one in front of you —
  and left off the list that brings a desktop back. Nothing could ever show it
  again, and under a shell with no taskbar there is nothing to click. mshell now
  declines to manage it and says so, leaving an ordinary window you can still
  use.

- **Sticky windows follow a pinned desktop onto its display.** A desktop pinned
  with `monitor = N` puts its windows on that display; a window that came along
  with you was not treated as one of them and stayed on whichever screen it was
  already on. It also now joins that desktop's history, so `last_window` can
  reach it — and if the desktop it is following you to is full, the log says so
  instead of the window quietly vanishing.

- **A monitor pin moves floating windows too.** Pinning a desktop recorded the
  new display for every window on it and left the tiling pass to do the moving,
  which is fine for tiled windows and does nothing at all for floats — the tiler
  never places them. A float on a pinned desktop stayed on the old screen while
  everything else moved. Each one is now moved the shortest distance that puts
  it fully on the right display, so several of them do not end up stacked in the
  same spot.

## 0.15.0 — 2026-08-11

### Added

- **`mshell.set_smart_borders(true)` — no focus ring when there is nothing to
  tell apart.** The ring exists to answer "which window has the keyboard". On a
  monitor showing a single window that question has no second candidate, and
  the ring is reduced to a coloured line drawn around the screen's contents.
  With this on, that case draws nothing; open a second window and the ring is
  back on the focused one.

  Counted per monitor, like `set_smart_gaps`, and over the current desktop's
  windows — a window on another desktop or another display never keeps the ring
  alive. Unlike smart gaps, **floats count**: a float sitting over one tiled
  window is a second window you can see, and telling those two apart is exactly
  the ring's job. Anything off the screen does not count, however it went away —
  minimised, hidden by its app to the tray, held back by the layout, or taken
  off the screen by mshell for a desktop switch.

  `monocle` shows one window by design, so the ring is off there for as long as
  that layout is up. Off by default; a config that never calls it sees no
  change.

## 0.14.9 — 2026-08-11

- fix: hide a window by sinking it under the backdrop, not by removing it

## 0.14.8 — 2026-08-11

### Fixed

- **Hidden windows are now sunk under the backdrop, and the blank browser is
  gone.** A window mshell takes off the screen is no longer removed from the
  desktop at all: it is dropped **below the backdrop in the z-order**. It does
  not move, it keeps `WS_VISIBLE`, its surface, its size and its position, and
  it goes on drawing — it is simply covered by an opaque window that fills the
  virtual screen. Coming back is one `SetWindowPos` to the top.

  This is the answer to "why does Chrome work under Explorer and not here", and
  it took measuring five arms on the same machine — a freshly launched Chrome,
  tiled, cycled by desktop switches, sampling the window's own pixels:

  | how the window was taken off the screen | result |
  | --- | --- |
  | `ShowWindow(SW_HIDE)` | blank on the **first** round trip, permanently |
  | moved off every display (0.14.6 stashing) | blank on the **first** round trip |
  | `--disable-gpu`, moved off every display | blank on the **first** round trip |
  | sticky — never taken off at all | fine |
  | **sunk below the backdrop** | **fine** |

  "Blank" means the whole window painted in its own frame colour with nothing in
  it, and nothing gets it back: not `RedrawWindow`, not `RDW_UPDATENOW`, not a
  real resize, not `SetForegroundWindow`, not `SwitchToThisWindow`, not
  minimise/restore. Only restarting the browser. So this was never a repaint
  that could be nudged — a Chromium window that leaves the composited desktop
  does not come back, and the fix is to stop taking it out. Explorer's own
  virtual desktops cloak, which keeps the window in place; being merely
  *covered* is what happens every time you focus something else, which is why it
  is the one path every app on Windows is tested against.

  Sinking also costs less to get right than the mechanisms it replaces. There is
  no rect to remember, nothing to strand — if mshell dies its backdrop dies with
  it and every sunk window is on screen again — and the drift detector has
  nothing to argue with, because nothing moved.

  Cloaking is still tried when sinking cannot be used (a window the app pinned
  topmost itself, or no backdrop), then stashing, then `SW_HIDE`.
  `set_hide_policy("hide")` still means literal `SW_HIDE`.

- **Correction to 0.14.8.** That release said the `apps` tweak — Chromium's
  `NativeWindowOcclusionEnabled` policy — was the fix for the blank window. It
  is not: an instance launched with `--disable-features=CalculateNativeWinOcclusion`
  blanked on the first round trip exactly like a default one. Turning occlusion
  detection off is still worth having in a tiling shell (it is what stops a
  covered window being throttled to a standstill), and the tweak stays, but the
  blank window was the hide mechanism and is fixed above.

### Fixed

- **The browser window that comes back as one flat grey rectangle.** Not the
  drifting frame 0.14.6 fixed — this one is the whole window: frame painted,
  nothing inside it, permanent. Chromium decides for itself whether anyone can
  see each of its windows ("native window occlusion") and stops presenting the
  ones it believes are hidden, and in a tiling shell that is most windows most
  of the time: cloaked, `SW_HIDE`'d, stashed off every display, or simply
  covered by whatever you are looking at. Sometimes it does not start again.

  Measured against such a window, live: `RedrawWindow` async, `RedrawWindow`
  with `RDW_UPDATENOW`, a 1px move, a real resize, `SetForegroundWindow`,
  `SwitchToThisWindow`, and a full minimise/restore. **None** of them brought a
  pixel back. The browser process, its GPU process and all five renderers were
  alive and the window answered `WM_NULL`. Only restarting the browser fixed it,
  which means there is nothing for mshell to fix on the way back — the fix is to
  stop the browser guessing.

  New tweak group, `apps`, holding the policy that does that for Chrome,
  Chromium and Edge:

  ```
  mshell.exe --tweaks apply apps          (from an administrator prompt)
  ```

  `install.bat` applies it alongside the hardening, and needs the same
  administrator prompt for the same reason: the key is under
  `HKCU\Software\Policies`, which Windows ACLs read-only for the user who owns
  the hive. Unelevated it is skipped and reported. The no-admin route — and the
  only route for an Electron app, which has no policy — is on the launch:

  ```
  chrome.exe --disable-features=CalculateNativeWinOcclusion
  ```

  `uninstall.bat` reverts this group along with the rest, back to whatever the
  value was before rather than to a default.

## 0.14.7 — 2026-08-11

- fix: the tiling pass believed every move it was refused
- fix: a float that could not say which display it was on
- fix: a helper that stopped answering took the whole shell with it
- docs: changelog for the window-model fixes

## 0.14.6 — 2026-08-10

### Fixed

- **The tiling pass believed every move it was refused.** `window_set_pos`
  exists because a placement can be refused — that is the entire reason
  `mshelld.exe` exists — and it returned a `bool` that no caller read. Every
  call site then wrote `applied_rect` and `has_applied` regardless.

  One refused `SetWindowPos` therefore became permanent wrong state.
  `applied_rect` is what the tiler's no-op skip, the drift detector and the
  drag-to-swap hit test all read, so a window that never moved was recorded as
  being where the layout wanted it, was never offered that rect again, and left
  a phantom cell behind that a drop could land on.

  The worst of it was the batch. `DeferWindowPos` only queues;
  `EndDeferWindowPos` is what moves anything, and a batch holding one window we
  are not allowed to move fails **as a whole**. Its return was never checked,
  and the loop had already recorded every window in it. Open Task Manager as the
  first window on a desktop and press a layout key: nothing moved, everything
  claimed it had, and the skip matched that claim forever. The layout was frozen
  with no log line.

  The outcome is now a value — placed locally, placed by the helper, or refused
  by both — and recording is one function that runs *after* the placement rather
  than six copies that ran regardless. The batch records once it commits and
  replays individually when it does not, which is what makes it
  self-correcting: the replay marks the window that refused, so the next pass
  keeps that one out of the batch and the rest of the desktop is never held
  hostage to it again.

  Three things fall out of having the answer. A window that needed the helper
  can now stop needing it, where before one transient refusal pinned it to the
  pipe for life. A window nobody can place no longer keeps a tile — with no
  helper running it floats, which is what INSTALL.md and the helper's own
  startup warning have always said happens, and its cell goes back to the
  windows that can use it. And `window_hide` stops claiming hides it did not
  achieve: cloak, stash and `SW_HIDE` can all be refused, only the first two
  were checked, and `window_on_screen` was answering "gone" about a window still
  sitting on the display.

- **A float could not say which display it was on.** Only the keyboard move keys
  updated a floating window's monitor. Mod+drag, a title-bar drag and the app
  moving itself all left the index naming the display the window opened on.

  That index is not bookkeeping: it picks the screen for centring and for
  fullscreen, it becomes the focused monitor on the next focus, and it is what
  the one-fullscreen-per-monitor check compares. Drag a float to your second
  display and fullscreen it, and it grew on the first one. It is now read where
  every move is observed.

  Floats also survive losing a display. Re-homing on a hotplug clears the flag
  that makes the next tiling pass re-place a window, and the tiler never places
  floats — so a float whose monitor was unplugged kept a rect in that monitor's
  coordinate space, which under a shell with no taskbar means gone. Those are
  pulled back now, as is a `geometry` rule written for an arrangement the
  machine no longer has. Clamped rather than centred, because several can be
  rescued at once and centring would stack every one of them in the same spot.

- **`float_on_top` was promoting windows mshell had promised not to touch.**
  Windows adopted at the *tracked* tier — an owned dialog nobody opted into, one
  too small to tile, one held disabled by its own modal — are marked floating to
  keep them out of the layout, not because anybody chose to float them. The
  z-order pass read that as the float tier and parked every one of them in the
  always-on-top band, so a stray error box sat above everything on the desktop.
  The two meanings are now distinguished, and the ring colour asks the same
  question.

- **Fullscreen and the float toggle disagreed about where a window goes.** Where
  a float sits was two questions that each did nothing when the other applied: a
  fullscreen *rule*, and centring, which bails on any kind of fullscreen. A
  window fullscreened from the keyboard while tiled satisfies neither, so
  `Win+f` on it did nothing at all and left it at the tile it no longer owned
  with every flag claiming otherwise — and pressing fullscreen again restored a
  rect that had never been saved. It is one question now. `start_fullscreen` on
  a floating window also works, which it did not: it set the mode and nothing
  acted on it.

  A pre-fullscreen rect also belongs to the float it was saved from. It used to
  survive a float → tile → float round trip, after which the next fullscreen
  refused to save its own and restored one from two arrangements ago.

- **A helper that stopped answering took the whole shell with it.** The requests
  to `mshelld.exe` were synchronous reads on a blocking pipe, made from the
  thread that pumps messages. A helper that *dies* was always handled; one that
  is alive and simply not reading — suspended, in a Windows Error Reporting
  dialog, blocked inside its own cross-process DWM call — was not, and the read
  never returned. This process is the shell, so there is nothing behind it.

  Requests are now bounded at 250 ms, and three consecutive timeouts stop mshell
  asking for five seconds. What that degrades to is exactly the documented
  no-helper behaviour: elevated windows float and stay put. That is a fallback;
  a frozen session is not.

### Security

- **The helper's pipe was open to every interactively logged-on user.** Its DACL
  named the `IU` alias, on the reasoning that the helper's elevated token is the
  Administrators one and granting *that* would not let an unelevated mshell
  connect. The first half is true and the conclusion was not: elevation changes
  a token's integrity level and its groups, not its user. The helper already
  runs as the interactive user, so its own SID was the right answer all along —
  and `IU` is a far larger set than that. The pipe name is per-session and
  entirely predictable, so with fast user switching a second signed-in user
  could open the first user's helper and move, hide or close their windows,
  including the elevated ones it exists to reach.

  `mshell.exe`'s own `--msg` pipe had computed this correctly from its process
  token all along, so the two now share one implementation rather than two
  opinions. The helper logs which SID it granted, because when the shell cannot
  connect that line is the whole diagnosis — and a logon task edited to run as
  SYSTEM is now documented as unsupported rather than merely broken. If you
  registered the task with the command in INSTALL.md, nothing changes for you.

- **Both pipes accepted remote clients and allowed instance squatting.** A named
  pipe is reachable over SMB as `\\host\pipe\mshelld-1` unless it says
  otherwise, and anything holding `FILE_CREATE_PIPE_INSTANCE` on an existing
  pipe can add an instance beside it and take turns serving the shell's
  requests. Both are now refused: nothing on the network has any business
  driving these operations, and failing loudly on a name already in use beats
  sharing it quietly.

- **A refused cloak no longer means `SW_HIDE`, and browsers stop rotting.** The
  hide policy is cloaking for a reason: `ShowWindow(SW_HIDE)` clears
  `WS_VISIBLE`, DWM drops the window's redirection surface, and a Chromium-class
  app (Chrome, Edge, Electron) rebuilds its compositor from that — blank until
  something makes it draw, or with its page offset by one client origin, once per
  hide/show round trip, until you restart the app. Measured on a full-screen
  Chrome: **13 px further right per desktop switch**, the app's own frame colour
  filling the gap, which reads as a grey border that keeps growing.

  Cloaking, though, is not mshell's to have. `DwmSetWindowAttribute(DWMWA_CLOAK)`
  on another process's window is refused for an unelevated shell — every foreign
  window, not just elevated ones — and on this Windows 11 build it is refused for
  the **elevated helper too**, with `mshelld.exe` connected. So the fallback was
  not a rare degraded mode: it was what every desktop switch did.

  There is now a third mechanism between them. When cloaking is refused the
  window is **stashed**: moved clear of every display, keeping `WS_VISIBLE` and
  its surface, still composited and still drawing. Nothing is torn down, so
  there is nothing to rebuild wrongly — and the way back is a real
  `SetWindowPos` from off-screen to where it was, rather than the no-op move an
  app is free to ignore, which is the other half of why windows came back blank.
  `set_hide_policy("hide")` still means literal `SW_HIDE`; asking for it is
  asking for it.

  A stashed window is still a window — the OS lists it in Alt+Tab, and a
  capture-by-handle recorder still sees it — and one left behind by a crashed
  shell would be off-screen with nothing to bring it back, so the startup sweep
  that uncloaks strays now also pulls back any manageable window sitting
  entirely off every display, and shutdown unstashes as it uncloaks.

- **The cloak refusal says which refusal it was.** 0.14.3's message sent
  everyone to `install.bat /helper`, which is the right answer only when the
  helper is missing. It now logs the `HRESULT` and whether `mshelld.exe` was
  listening: refused with no helper is an install away, refused with one means
  DWM will not cloak a foreign window on this machine at all, and no amount of
  installing changes that.

## 0.14.5 — 2026-08-10

### Fixed

- **Changing layout froze the whole machine.** `Win+Space`, or any of the
  `layout_*` bindings, and everything stopped answering — mshell, the bar, and
  every window on the desktop — for as long as it took to kill the shell.

  Two loops, both of them the same shape: a tiling pass produces window moves,
  window moves produce `EVENT_OBJECT_LOCATIONCHANGE`, and the drift detector
  answers a location change with another tiling pass. The suppression counter
  does not break the cycle, because the hooks are `WINEVENT_OUTOFCONTEXT`: the
  system queues those events and delivers them on the next pump, long after the
  pass that caused them called `events_suppress_end()`. Changing layout is the
  one action that resizes *every* window at once, which is why it is where this
  bites.

  The first loop is a window that **cannot take the rect it is given** — an app
  with a minimum size larger than its new cell (Discord, Steam, Spotify at three
  columns), one that re-centres itself, one whose DWM frame does not round-trip
  across monitors of different DPI. It never lands inside the 4 px tolerance, so
  every snap-back earns another location change and another full pass, forever.
  The snap-back is now capped: three attempts inside a second, then the window is
  left where it insists on being and the log says which window and why.

  The second is **animation**. `anim_tick` moves each window every 16 ms, and
  every one of those frames arrived at the drift detector looking like escape,
  because an in-flight window is by definition not at the rect the layout
  assigned. `anim_is_animating()` existed for exactly this and nothing called
  it — the tiler fought the animation frame for frame, one whole tiling pass per
  window per frame. The detector now asks it, and the tiler hands a window that
  is already moving back to `anim_begin` instead of teleporting it.

- **Manual tiling (bsp) placed every window on every display.** The tiler lays a
  desktop out one monitor at a time — it groups the desktop's windows by the
  display they live on and gives each group that display's work area — but there
  was a single tree per desktop, holding all of them. So each monitor's pass fed
  the *whole* desktop through that monitor's rectangle: every window placed
  twice per tiling pass on two displays, ending up wherever the last pass put
  it, with the other screen's windows piled on top. Splits built on one display
  moved the other's windows.

  Trees are now keyed by desktop **and** monitor. Each pass sees only the
  windows on the display it is laying out; a window dragged across displays is
  pruned from the tree it left and splits the focused leaf of the one it
  arrived on; `rotate_split`, `split_grow` and the container bindings act on the
  focused window's display and leave the other alone. Unplugging a display
  releases its trees.

### Changed

- **`cycle_layout` no longer cycles into bsp.** The cycle is the seven dynamic
  layouts, each a pure function of the window list, where overshooting costs one
  more press. bsp is not: its structure is the record of where you were as each
  window opened, so arriving there by pressing `Win+Space` once too many put you
  in a tree you did not build — and pressing again abandoned it. It keeps its own
  binding (`layout_bsp`, the `b` submap). A desktop already in bsp still cycles
  out, so the key is never a dead end.

- Re-targeting a running animation continues from the frame **on screen** rather
  than from where the previous move started, so a layout change mid-motion no
  longer jumps the window backwards before it sets off again.

## 0.14.4 — 2026-08-10

### Fixed

- **An ordinary per-user install failed at the last step, and undid nothing it
  had already done.** `install.bat` finished copying the binaries, pointed the
  Winlogon Shell key at them — and then stopped dead on `reg import
  harden.reg` with `ERROR: Error accessing the registry`, printing
  **INSTALL FAILED** over an install that was, apart from two registry values,
  complete. Everything after that line was skipped: the other nine hardening
  tweaks, the helper's logon task under `/helper`, and the restart that puts you
  on the build you just installed. Re-running it did the same thing again.

  Two of harden.reg's values live under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\` — the blanket
  Win-key hotkey policy, and Win+L. Windows ACLs that subtree **read-only for
  the user who owns the hive**: SYSTEM and Administrators may write there, you
  may not, which is the point of a policy. `reg import` is all-or-nothing and
  stops at the first refusal, and that key happened to be the first in the file,
  so an unelevated install applied *none* of the hardening and then failed —
  including `LowLevelHooksTimeout`, the one that keeps Windows from dropping the
  keyboard hook mid-chord.

  `install.bat` now applies the same set through `mshell.exe --tweaks apply
  input`, which is generated from the same table harden.reg is and applies it a
  value at a time: it sets everything it is allowed to, skips what it is not,
  and says so instead of failing. Run it from an administrator prompt (or merge
  `harden.reg` by hand, accepting UAC) to pick up the last two. `uninstall.bat`
  reverts through `--tweaks revert input` when there is a backup to revert to,
  which restores the value *you* had rather than harden-undo.reg's Microsoft
  default, and falls back to the file when there is not.

## 0.14.3 — 2026-08-10

### Fixed

- **The browser that got smaller every time you came back to it.** Switch away
  from a desktop with Chrome on it, switch back, and a dark grey band appeared
  inside mshell's border — a little wider on every round trip, the page creeping
  further into the window until it was clipped off the right and bottom edges.
  Measured with Chrome tiled full screen: the page moved **13 px per switch**,
  which is exactly this window's client origin (the outer gap plus Chromium's
  own client inset). Nothing about the geometry mshell applies was wrong — the
  window rect, DWM's extended frame bounds and the client size were identical
  before and after every switch. The grey was Chrome's own frame colour, showing
  through where Chrome had stopped drawing.

  The cause was two steps upstream. `window_hide` cloaks by default precisely
  because `ShowWindow(SW_HIDE)` tears a window's redirection surface down and
  Chromium-class apps do not rebuild theirs where they left it — but **cloaking
  another process's window is privileged**. From an unelevated shell DWM refuses
  `DWMWA_CLOAK` for *every* foreign window, an ordinary same-user one included,
  so without `mshelld.exe` running the default hide policy was never the policy
  in effect: every desktop switch, every monocle pass and every scratchpad
  toggle went through `SW_HIDE`, and a browser paid a client origin for each one
  until it was restarted.

  Run the helper and the whole class of damage goes away —
  `install.bat /helper` from an administrator prompt, see INSTALL.md. The
  documentation said this mattered only for elevated windows; it matters for
  all of them, and now says so in INSTALL.md, in the sample config and next to
  `HidePolicy` itself.

- **The helper was asked about cloaking only when DWM said the one thing it
  never says.** `window_set_cloaked` forwarded to `mshelld.exe` on
  `E_ACCESSDENIED` — the code the *placement* path really does come back with.
  DWM answers differently: `0x80070005` for an ordinary window, and
  `0x80070006` for one owned by a higher-integrity process. So the single case
  the helper exists for — Task Manager, regedit, an admin terminal — was the
  case the test let through, and those windows stayed visible on every desktop
  even with the helper running. Any failure now forwards; with no helper around
  that costs one `WaitNamedPipe` with a zero timeout.

- **The fallback stopped being silent.** Losing cloaking cost one `WARN` line,
  in a file nobody opens, worded as though the shell had chosen a slightly
  different flicker for the session. It is now an `ERROR` and a toast, once,
  naming what breaks (Chromium apps drift on every switch) and the one command
  that fixes it.

## 0.14.2 — 2026-08-10

### Fixed

- **An elevated window can be kept on top too — with the helper.** 0.14.1 put
  floating windows in Windows' always-on-top band, which fixed every ordinary
  window and left exactly one behind: Task Manager, regedit, an admin terminal.
  Changing a window's z-order is blocked by the same integrity check (UIPI) as
  moving it, so an unelevated mshell's `SetWindowPos` was refused and the float
  stayed buriable. The privileged helper is the way across that boundary and
  already existed — but its protocol deliberately masked every z-order bit out
  of a placement, so it could not do this either.

  `mshelld` speaks **protocol v3** and takes one more request: put this window
  in, or out of, the always-on-top band. Deliberately a band and not an
  arbitrary "above that window" — the shell orders floats among themselves with
  the windows it is allowed to place, and the pipe never gains the power to
  restack the desktop. mshell tries locally first and only forwards on the
  refusal that means UIPI, the same shape the tiling path already had.

  This needs the helper: without `mshelld.exe` running, an elevated float is
  still buriable and there is nothing an unelevated shell can do about it —
  install it with `install.bat /helper` from an administrator prompt (see
  INSTALL.md). Upgrading replaces both binaries in step; a v2/v3 mismatch is
  refused loudly rather than half-working.

- **`update` now actually leaves you on the build it installed.** The action
  spawned `install.bat` in a console and returned, letting the script kill
  mshell and start the new binary. Both halves could fail silently — the kill
  needs rights a child process may not have, and the console can be closed
  before it gets there — and nothing checked, so the toast said "mshell will
  restart" and the log said the install had happened while the **old build kept
  running**. Every symptom of that is the symptom of a release that did not
  work.

  mshell now runs `install.bat /norestart` to completion, checks its exit code,
  and restarts *itself* — exiting is the one hand-over that cannot be refused
  for want of rights, and Winlogon brings the Shell value back up. A machine
  with `AutoRestartShell` off is told to sign out instead of being logged out.
  The script's output goes to `%LOCALAPPDATA%\mshell\install.log` rather than a
  console that has scrolled away, and the failure paths now say which file to
  read. The staged `mshell.exe.old` is deleted by the instance that comes up.

## 0.14.1 — 2026-08-10

### Fixed

- **A floating window can no longer be buried by anything you click.** Keeping
  floats over the tiled grid was done by re-raising them to the top of the
  ordinary z-order band on every focus change mshell heard about — which is not
  every focus change there is. An app that raises itself a moment *after* its
  activation (Chrome opening a window, an Electron app moving focus into a
  child), or a window mshell does not manage taking the foreground, left the
  float behind it with nothing left in the loop to fix it. Floats now go into
  Windows' always-on-top band instead, which the OS maintains with no event of
  ours involved: no ordinary window can cover one, whatever it does and whether
  or not we hear about it.

  Ranking inside that band is still mshell's: the status bar, launcher,
  which-key panel and toasts sit above the floats — the bar stays furniture you
  cannot bury — and a window covering its whole monitor sits above both, so a
  fullscreen video is not interrupted by an overlay stranded in the middle of
  it. Two overlapping floats keep their relative order across a focus change,
  as before, and `mshell.set_float_on_top(false)` still opts out entirely.
  Un-floating a window (`Win+f`) takes it back out of the band at once instead
  of at the next tiling pass.

## 0.14.0 — 2026-08-09

### Added

- **The display itself is now part of the config.** `mshell.monitor_rule` takes
  `resolution`, `refresh` and `hdr` alongside the tiling overrides it already
  had, so a display's mode is stated in `init.lua` next to that display's gaps
  and layout. Replacing Explorer takes Settings → System → Display with it —
  the page still opens by URI, but reaching it from a shell with no Start menu
  means spawning it and driving it with the mouse, for something a rule can
  state once and mshell can re-assert on every start.

  ```lua
  mshell.monitor_rule("*DISPLAY1", {
      resolution = "2560x1440",   -- or { 2560, 1440 }
      refresh    = 165,
      hdr        = true,
      layout     = "columns",     -- and its tiling habits, same rule
  })
  ```

  Rules are applied at startup, on every reload, and to a monitor plugged in
  mid-session — but deliberately **not** on top of a mode you changed yourself
  in Windows' display settings, which would otherwise be stamped back a second
  later. Every mode is validated with `CDS_TEST` before it is applied, so a
  resolution the panel cannot show costs a line in the log and leaves the
  display alone rather than blanking the screen of a machine whose shell this
  is. Changes are session-only — Windows' own stored display configuration is
  never written, so booting *without* mshell (the recovery path in INSTALL.md)
  hands back the display Windows was configured with. HDR is the exception and
  cannot be otherwise: advanced colour is a persistent system setting.

- **`mshell.exe --displays`** — the discovery half of the above. Prints every
  attached display's device name (what a rule matches on), its current mode,
  whether it supports HDR and whether HDR is on, and every mode it will
  actually accept. Runs standalone, like `--tweaks`: it does not need mshell to
  be running, or to be your shell.

- **Two bindable display actions**, both acting on the *focused* monitor:
  `toggle_hdr`, and `cycle_refresh` (`1` / `-1` steps through the rates that
  display offers at its current resolution, wrapping — the resolution is held
  deliberately). Both report what they did in a notification. These are the two
  display settings people change per task rather than once: HDR only while a
  game is up, a lower refresh rate on battery.

- **`--query` and `mshell.get_monitors()` report the display**, not just its
  geometry: each monitor now carries its `device` name, its `refresh` rate and
  its `hdr` state (`null`/`nil` when the panel cannot do HDR — a different
  answer from "off"). Both are read live rather than cached, so they stay right
  when the mode is changed outside mshell.

## 0.13.6 — 2026-08-01

### Fixed

- **Elevated windows no longer appear on every desktop.** Task Manager, regedit
  or an admin terminal could be *tiled* through the privileged helper, but a
  desktop switch could never *hide* them: cloaking a window crosses the same
  integrity boundary (UIPI) as moving it, and the helper forwarded only
  `SetWindowPos` — so an elevated window sat on screen no matter which desktop
  you were on. The helper protocol is now v2 and also forwards cloak/uncloak
  and `WM_CLOSE`, with the shell keeping its "try locally first, ask only on
  refusal" shape. Elevated windows hide and show with their desktop like any
  other, and the close keybind reaches them. Upgrading replaces `mshelld.exe`
  in step and restarts it; a v1/v2 mismatch is refused loudly rather than
  silently half-working.

- **Windows that escaped management no longer float across every desktop.** A
  window that failed the manageability test at the instant it appeared was
  never adopted at all, and nothing ever retried — so it belonged to no
  desktop and stayed visible on all of them forever. That is how Steam (whose
  updater holds its main window *disabled* at the moment it first shows) ended
  up everywhere. Every window is now adopted at one of two tiers: *full*
  (tiled, decorated, ringed — what "managed" always meant) or *tracked*
  (desktop membership only). The tracked tier is what an owned dialog nobody
  opted into, a modal-disabled window, or a too-small window gets: it hides
  and shows with its desktop, is focusable, closable and movable between
  desktops, and is otherwise untouched. `toggle_float` promotes a tracked
  window to full management — landing it in the grid, which is also the new
  way to tile something mshell deliberately left alone.

- **Apps that open minimized are adopted.** A window that was already
  minimized when mshell first saw it used to be rejected and never recovered;
  it now joins its desktop and tiles when restored.

### Changed

- **Owned windows (dialogs, pop-ups) are always desktop-bound now.** They used
  to sit over whichever desktop you switched to unless `set_manage_owned` or a
  `dialog` rule claimed them. They now hide and show with the desktop they
  opened on; `set_manage_owned(true)` and `dialog` rules keep their meaning as
  the opt-in to *fully* manage (tile/float/decorate) them, and an `ignore`
  rule keeps its meaning as the way to leave a window on every desktop at
  once.

## 0.13.5 — 2026-07-28

### Added

- **An `update` action, on `Win+Shift+u`** (and `Win` `x` `u` in
  `init.full.lua`): fetch the latest GitHub release and install it. It
  downloads the release zip, hashes it against the SHA-256 the release
  published, unpacks it and runs the `install.bat` inside — which is the
  upgrade path already, renaming the running image rather than overwriting it
  and restarting mshell itself. Progress and every failure arrive as
  notifications. `mshell.exe --msg update` works too.

  This is deliberately the opposite of `set_update_check`, which only ever
  *tells* you a release exists and stays that way. An updater that swapped out
  the **shell** unattended would turn a bad release into a black screen at
  sign-in with no desktop left to fix it from — but a key you pressed, sitting
  in front of the machine, is a different proposition, not a smaller helping of
  the same one.

  It declines in two cases. It will not run twice at once, so holding the key
  down cannot race two downloads into one directory. And it will not install
  when the running mshell is **not the registered shell** — from a portable
  copy or `--test`, `install.bat` would not be upgrading anything, it would be
  taking over your shell for the first time. There it stops after unpacking and
  names the folder so you can run `install.bat` yourself.

### Fixed

- **The update check asked GitHub about a repository that does not exist.** It
  polled `/repos/mshell/mshell/releases/latest` — an owner that is not ours —
  so every check 404'd and `set_update_check(true)` had never once reported an
  available release.

## 0.13.4 — 2026-07-28

### Added

- **`default = "always"` — the config decides where you start, every time.**
  Naming a start desktop only ever settled a *first* run: the session file
  remembers the desktop you were last on and that beat the config on every
  subsequent start, so a config that said "I begin on `term`" was silently
  ignored from the second boot onwards. `default = true` keeps that behaviour
  (a restart leaves you where you are); `default = "always"` outranks the
  session and lands you on the named desktop every start. The session is still
  written either way, so going back to `default = true` returns you to wherever
  you actually were.

- **Pointer speed, acceleration and the left/right button swap** are now
  configurable — `set_mouse{ speed = 6, accel = false, swap_buttons = false }`,
  alongside the gesture settings already on that call. Ranges match the Windows
  UI: `speed` is the 1..20 slider with 10 as the middle notch, and `accel` is
  the "enhance pointer precision" checkbox. This is another of the things
  replacing Explorer takes away: they were reachable only from the Settings page
  that a machine running mshell no longer has, and turning acceleration off is
  not a niche request.

  They are **borrowed rather than set**, on the same terms as the foreground
  lock timeout: these are per-user Windows settings that every application on
  the machine sees, so mshell snapshots what was there before its first write,
  applies without `SPIF_UPDATEINIFILE` so nothing is written into your user
  profile, and puts the originals back at exit — including from the crash
  handler, since a config with `swap_buttons` on would otherwise leave a machine
  whose shell just died with its buttons the wrong way round and nothing left to
  change them from. Ownership is tracked per field, so deleting one line and
  saving hands that setting back on the reload and leaves the other two alone. A
  field the config never mentions is never touched.

- **A `kovaaks` desktop in the worked config**, on `k` in both desktop submaps:
  leader `g k` takes you there, leader `m k` sends the focused window. It
  auto-launches KovaaK's through Steam rather than by path —
  `steam://rungameid/824270`, which `ShellExecuteW` opens exactly like a `.lnk`,
  so it finds the game in whichever library folder it is installed in and keeps
  working when that moves. Floats like the `game` desktop does, and is separate
  from it on purpose: aim training happens *next to* a session, so having both
  open shouldn't mean closing one. Its window needs no rule of its own — the
  `*\steamapps\common\*` rule already covers every Steam game.

### Changed

- **The start desktop is a desktop rule now: `default = true`.** Where you begin
  is a property of a desktop, so it is declared where that desktop's app,
  layout, monitor and keys already are — one row of the config instead of a row
  plus a setter further down the file. It needs a literal name rather than a
  pattern (mshell has to create exactly one desktop at startup), and if two
  rules claim it the **last** one wins, which is the layering rule the other
  fields already follow.

  ```lua
  mshell.desktop_rule("term", { default = true, app = "alacritty.exe" })
  ```

  **`mshell.set_start_desktop` is gone.** Calling it now fails the config load
  with a message naming the replacement, rather than the "attempt to call a nil
  value" a removed function would otherwise give you.

## 0.13.3 — 2026-07-27

### Added

- **`Win+b` toggles the status bar** in the shipped `init.lua` — the
  `toggle_bar` action existed but nothing in the default config reached it.

### Fixed

- **A layout change no longer snaps back a quarter-second later.** Every layout
  action saves the session, and the session file lives in the config folder —
  which the auto-reload watcher watches. `FindFirstChangeNotification` reports
  that *something* in the folder changed but not *what*, so each `Win+Space`
  came back ~250 ms later as a phantom config edit. The reload's
  `desktop_apply_rules` re-applied the session snapshot taken at startup, and
  the layout reverted to wherever it had been when mshell launched: a single
  press flashed the next layout and undid it; holding the key cycled through
  every layout and then reset to the original one. The watcher now uses
  `ReadDirectoryChangesW`, which names the changed files, and batches touching
  only `session.txt` are ignored. A real `init.lua` save still reloads exactly
  as before.
- **`layout_bsp` now saves the session like every other layout action**, so a
  BSP layout survives a restart instead of being forgotten.
- **The focus ring now follows a floating window while it moves.** A native
  move/resize of the focused float and a mod+drag both left the ring behind at
  the old rect until something else refreshed it; both paths now refresh the
  ring as the window travels.

## 0.13.2 — 2026-07-27

### Fixed

- **Windows still came back black after switching desktops.** 0.13.0 changed
  how a window is taken off the screen — `ShowWindow(SW_HIDE)` became a DWM
  cloak — and shipped as though that were the fix. It was not, for a reason
  that is plain in hindsight: the repaint meant to accompany it was written as

  ```c
  if (!IsWindowVisible(mw->hwnd)) { ... RedrawWindow ...; has_applied = false; }
  ```

  and a cloaked window is still `WS_VISIBLE`. On the new default path that
  branch never ran. No `RedrawWindow`, no dropped `has_applied` — and with
  `has_applied` intact the tiler's no-op-move skip meant no `SetWindowPos`
  either, because the window was of course exactly where the layout had already
  put it. Switching away and back did nothing to the window at all beyond
  uncloaking it, and an app that had stopped presenting had no reason to start.

  Cloaking is not what makes the difference. An app that renders off the UI
  thread stops drawing whether it learns it is invisible from the hide or from
  its own occlusion tracking noticing the cloak; being **asked to draw** on the
  way back is what matters. So that now happens on every reveal, on either
  path, decided from mshell's own bookkeeping rather than from
  `IsWindowVisible` — which for the case that matters most answers "fine":

  - `RedrawWindow` with `RDW_ALLCHILDREN`, because a Chromium window's content
    lives in a child `HWND` and invalidating only the top level asks the wrong
    window. No `RDW_UPDATENOW`: synchronous cross-process painting would hang
    the WM on an app that is not answering.
  - `has_applied` dropped, plus a `needs_repaint` flag so the tiling pass
    cannot skip the placement as a no-op move and adds `SWP_NOCOPYBITS`, which
    stops Windows blitting the stale bits forward.
  - Floating windows get that placement inline, since the tiler never places
    them and would never consume the flag.

- **The desktop-switch show loop no longer skips windows the layout had
  hidden.** That skip saved an uncloak/recloak for monocle's held-back windows,
  and it was the one place in the reveal path that could decline to show a
  window at all — a bad trade in code whose failure mode is windows staying
  invisible. The tile pass re-hides them in the same turn of the message loop.

### Internal

- Each hide and show logs the mechanism it used at debug level, and a DWM that
  refuses to cloak now warns once. A silent downgrade to `SW_HIDE` is otherwise
  indistinguishable from cloaking that did not help, which is exactly the
  confusion that made the first attempt at this take two goes.

## 0.13.1 — 2026-07-27

### Fixed

- **A version bump no longer builds under the old number.** `VERSION` reaches
  the compiler as `-DMSHELL_VERSION` and windres as `-DVER_MAJOR` and friends,
  and make compares timestamps, not command lines: after a bump every object
  already on disk was still "up to date", so the new number reached only the
  files something else happened to have made stale. The build then succeeded and
  lied — `make dist` produced a zip named for one version holding a binary that
  reported another in its VERSIONINFO, its startup log line, the `--msg status`
  JSON and the update check, which compares that string against the latest
  release and would have offered an upgrade to a version the binary already was.
  0.13.1's own first build called itself 0.12.0. The objects that bake the
  version in now depend on a stamp file named after it, so a bump invalidates
  exactly those (Lua's 32, which never mention it, are left alone).

- **A window mshell hid could be disowned a moment later and lost for good.**
  The `EVENT_OBJECT_HIDE` handler has to decide who hid a window — the app
  minimising itself to the tray, or mshell taking it off the screen for a
  desktop switch, monocle or the scratchpad — and it read that off the
  suppression counter. The counter cannot answer it: the WinEvent hooks are
  out-of-context, so the system queues events across the process boundary and
  delivers them the next time the message loop pumps, long after the pass that
  hid the window released it. mshell's own hide then arrived looking exactly
  like the app's, the window was marked `app_hidden` — which means "not ours to
  reveal" — and from there nothing would ever show it again: `window_show()`
  refuses it, the desktop-switch show loop skips it, and the tiler leaves it out
  of the layout. Under mshell there is no taskbar, and a hidden window has no
  Alt+Tab entry, so the window was simply gone.

  Monocle was the reliable way to hit it — one hide, at the very end of the
  pass, with no later blocking call to let the queued event arrive while
  suppression was still up. `Win+Space` into monocle and the other window did
  not come back; it read as the layout key closing it. Who hid a window is now
  decided from state (`wm_hidden`, and not cloaked) rather than from timing,
  which is what the `LOCATIONCHANGE` handler already does for the same reason.

  Only the `SW_HIDE` path can produce the event at all, which is why 0.13.0
  making cloaking the default hid this rather than fixing it: it is still
  reachable there through `set_hide_policy("hide")` and through a DWM that
  refuses to cloak (composition off — some VMs and remote sessions), and it was
  unconditional in 0.12.0 and earlier, where every hide was `SW_HIDE`.

## 0.13.0 — 2026-07-27

### Added

- **The status bar has modes: `top_bar` and `floating`.** `set_bar{ mode =
  ... }` picks between them and nothing else about the call changes; `top_bar`
  is the default and is exactly the bar that already existed. `floating` is one
  panel in the middle of the FOCUSED monitor instead of a strip on every one:
  the time large, the date under it, the desktops and layout, the focused
  title. It reserves nothing out of the work area — it floats over the windows
  rather than pushing them down — is click-through, and follows the focus
  between displays rather than putting three copies of the same clock on three
  screens. The mode is a named enum rather than a bool because more shapes are
  expected here.
- **The floating panel lists notifications inline**, via a `"notifications"`
  module. That module is what the extra height buys: a one-line strip has
  nowhere to wrap a message, which is why notifications had to be their own
  window in the first place. When the panel is showing them, notify.c stands
  its own toasts down rather than showing everything twice; turn the module
  off, switch modes, or hide the bar and the toasts come straight back. The
  toast stack remains the state either way, so expiry and stacking behave
  identically on both surfaces.
- **`toggle_bar`** — show or hide the bar without a config reload, re-measuring
  the work area and re-tiling so a hidden `top_bar` gives its strip back. It
  exists mostly for floating mode, where the panel sits over the middle of the
  screen and wanting it gone for a moment is the normal case. Bindable, and
  reachable over the control channel as `mshell.exe --msg toggle_bar`.
- **The which-key panel is fully configurable.** It had four colours and a
  delay; everything that decided its shape was a `#define`, so the one overlay
  whose whole job is to be read was also the one you could not fit to your
  screen or your eyesight. `mshell.set_whichkey{}` now also takes:
  - `position` — `bottom` (the default and the old placement), `top`,
    `center`, `left`, `right`, `top_left`, `top_right`, `bottom_left`,
    `bottom_right`, and `margin` for the gap to the monitor edge (a negative
    margin keeps the old automatic 5%-of-the-height inset).
  - `max_width` / `max_height` — either a fraction of the monitor (`0.5`) or
    design pixels (`900`); `0` means the monitor is the only limit. Text that
    no longer fits is ellipsized rather than clipped mid-glyph, and a panel
    that has to drop bindings says which ones in the log instead of looking
    complete.
  - `max_rows` — rows in a column before a new column starts (was a fixed 12).
  - `padding`, `row_spacing`, `column_spacing`, `key_spacing`,
    `header_spacing` — every gap in the layout, in design pixels at 96 DPI and
    scaled per monitor like the rest.
  - `font` and `font_size` — any installed family, at any size.
  - `border_width`, `opacity` and `rounded` — the panel's chrome. A border
    thicker than a pixel is drawn as four fills rather than a wide pen, which
    GDI would centre on the path and clip in half.

  All of it applies on reload, without restarting. Defaults are unchanged, so
  an existing `init.lua` gets the same panel it had.

- **Every action the shell implements is now reachable from a key.** Power
  management, volume and media, screenshots, the launcher, notifications, the
  BSP/container set, `jump_urgent`, `last_window`, `toggle_always_on_top` and
  `panic` were all implemented, documented and bound to nothing — the only way
  to press one was to know it existed and write the binding yourself. The worked
  example (`init.full.lua`) now reaches all of them. `init.lua` is unchanged: it
  stays the minimal starter.
- **Five new sub-maps, leader-only**: `media` (persisting), `system` (one-shot),
  `power` (one-shot, nested under `system`), `capture` (one-shot) and `bsp`
  (persisting). Reached by tapping `Win` and then bare keys, so no new binding
  asks for two keys held at once — which is free, since sub-map keys carry no
  modifier at all. `Win+Shift+*` and `Win+Ctrl+*` chords that already existed
  are untouched.
- **The destructive session actions are nested a layer deeper** than the rest.
  mshell has no confirmation dialog, so `Win` `x` `p` `d` being four deliberate
  taps — with `Esc` bailing out at every one, and an unbound key in a one-shot
  map doing nothing at all — is what stands between a slip and a shutdown.
- Four actions folded into sub-maps that already existed: `last_window` and
  `toggle_always_on_top` on `window`, `jump_urgent` on `desktop`, and the
  built-in `launcher` on `launch`. The launcher belongs in a one-shot map: it
  takes every keystroke while open, and a persisting map would still be
  swallowing keys the moment it closed.
- `mshell.set_urgency(true)` is documented (commented out) beside the
  `jump_urgent` key that needs it — without it nothing is ever urgent and the
  key has nothing to jump to.

- **`mshell.set_hide_policy("cloak" | "hide")`** — how a window is removed from
  view for a desktop you are not on. Defaults to `"cloak"`, which is what stops
  windows coming back black; `"hide"` restores the old `ShowWindow(SW_HIDE)`.
  See the Fixed entry below.

### Fixed

- **Floating windows no longer sink behind tiled ones when the focus moves.**
  The z-order pass that raises floats ran only at the end of a tiling pass, and
  focusing a window is not a tiling pass: activation raises the window you moved
  to, so focusing a tiled window — with a keybind, with a click, or by
  focus-follows-mouse — put it straight over the float you had been looking at,
  with nothing left to put the float back. Every focus change now re-asserts it,
  from `window_focus()` and from the foreground WinEvent, which is the only
  place that hears about a click.

- **Floats keep their order among themselves.** The pass raised them in desktop
  order, so two overlapping floats swapped places whenever it ran. It now walks
  the system z-order and re-stacks them as they were, with the focused float on
  top. Floats already in the topmost band (`toggle_always_on_top`, fullscreen)
  are left to the topmost pass rather than threaded into that chain — placing a
  window after a topmost one promotes it, which would have dragged the others up
  with it.

- **The focus ring sits on its window, not at the top of the stack.** It was
  pinned to `HWND_TOP`, which with floats above the grid meant the ring of a
  covered tiled window painted a coloured line across the float on top of it.

- **Every app on a desktop came back black after switching away and back.**
  Desktops were implemented with `ShowWindow(SW_HIDE)` / `SW_SHOWNOACTIVATE`,
  and clearing a window's visible bit is not a neutral act: DWM destroys the
  window's redirection surface, and every app that renders off the UI thread —
  anything Chromium or Electron based (Chrome, VS Code, Discord, Spotify), WPF,
  Qt on D3D — additionally treats it as being occluded and shuts its renderer
  down. On the way back there was a brand-new empty surface and nothing had
  asked the app to draw into it. Nothing else nudged them either: the tiler
  skips windows that are already in the right place, and a re-shown window is
  by definition exactly where it was, so no resize arrived to shake it out of
  it. A whole desktop's worth of windows could come back blank at once.

  Windows are now taken off the screen by **cloaking** them through DWM
  (`DWMWA_CLOAK`) instead, which is the mechanism Windows' own virtual desktops
  use and keeps the window from being torn down while it is away. Monocle and
  the scratchpad hide windows the same way.

  `mshell.set_hide_policy("hide")` restores the old mechanism.

  **This did not actually fix it** — see the Unreleased entry above. Cloaking
  changed how a window is hidden without changing the thing that mattered,
  which is whether anything asks it to draw on the way back.

- **Leaving monocle could strand its hidden windows.** `layout_hidden` was set
  by monocle and the BSP tree but never cleared by the layouts that hide
  nothing, so it survived a layout change and each pass hid the window again
  before re-showing it. It is now recomputed on every tiling pass, which also
  removes a hide/show flicker on every desktop switch.

- **An app un-traying itself while you were elsewhere put its window on the
  desktop you were looking at.** Clicking Discord's tray icon from another
  desktop showed it over your current one instead of on the desktop it lives
  on. It is now put back off-screen where it belongs.

- **Floating a window monocle was holding back lost it for good.** The tiler
  skips floating windows on both the hide and the show side, so the flag that
  kept it off the screen had nothing left to clear it. Floating a window now
  clears it and reveals the window.

- **Switching to an empty desktop now defocuses deliberately.** It used to
  happen as a side effect of hiding the last window — Windows hands the
  foreground on when a window is hidden, but not when it is cloaked, so the
  window you left would have kept taking your keystrokes invisibly. The
  foreground is parked on the backdrop instead, as Windows' own virtual desktops
  do. Same for sending the last window off the desktop you are on.

### Changed

- **`set_float_on_top` now defaults to true.** A window you floated is an
  overlay — a picture-in-picture, a calculator, a dialog — and having it
  disappear behind the grid on the next keystroke is not what floating it
  meant. `mshell.set_float_on_top(false)` restores the old behaviour.

- **Floating windows are centred on their monitor.** Where a float SITS was the
  one thing about it nobody owned: its size is the app's business and the layout
  never touches its rect, so it opened wherever that app last happened to be or
  at the next step of Windows' cascade — which, on a shell with no taskbar and
  no desktop behind it, reads as "somewhere near the top left, for no reason".
  The window deliberately kept out of the grid is also the one being looked at,
  so it now goes in the middle: both the window that opens floating (a `"float"`
  rule, a `dialog` rule, a desktop with `float = true`) and the one `Win+f` just
  took out of the grid.

  Position only — the size stays whatever the app asked for, clamped to fit. The
  monitor's *work area*, not its full bounds, so a centred window never slides
  under the bar. A rule's `geometry` and `fullscreen = true` both place the
  window themselves and are unaffected, as are minimised, maximised and
  fullscreen windows.

  `mshell.set_float_placement("none")` restores the old behaviour, and
  `center = false` in a rule's opts answers for one app — worth setting on an
  overlay that already positions itself, which is why the example config now
  passes it to the Flow Launcher rule. `center = true` opts a single app in
  under a config that set `"none"`.

### Internal

- **`whichkey_math.c`** — the which-key panel's grid arithmetic (how many
  columns, what gives way to a maximum size, where an anchor lands) split out
  with no Windows in it, and covered by `make test` alongside `match.c` and
  `layout_math.c`. The panel is drawn on a screen nobody is watching while the
  config that shapes it is being written, which makes "it looked right" the one
  check that was never available.
- `overlay_font_face()` extends the shared overlay font cache with a family
  name; the cache key gains the face, so the other overlays keep their font and
  their single rebuild-on-DPI-change.

## 0.12.0 — 2026-07-27

The release that fills in what a tiling WM is expected to have and what a shell
replacement is expected to survive: manual tiling with containers, a launcher,
notifications, and the pieces that make a bad day recoverable — a log that is
still there after a crash, a safe mode when the config is what crashed, and a
panic key for everything else.

**Upgrading from 0.11.0:** nothing in your `init.lua` has to change. One thing
moved: the log is now `%LOCALAPPDATA%\mshell\mshell.log` rather than
`%TEMP%\mshell.log`, and it is appended to rather than truncated on every start.
`mshell.set_verbose(true)` still works and now means `set_log_level("debug")`.

### Changed

- **The log is appended to rather than truncated, and it moved.** It was opened
  `"w"` on every start, so the run that mattered — the one that crashed — had
  its evidence deleted by the restart that followed it. It is now opened for
  append at `%LOCALAPPDATA%\mshell\mshell.log`, out of `%TEMP%` where cleaners
  reach, and rotates at 5 MB keeping two older generations.
- **Lines carry a timestamp and a level.** `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] `.
  There were previously two levels expressed as a boolean, which left no way to
  record something noteworthy-but-not-broken: startup, config loaded and
  shutdown all had to borrow the error channel to be written at all.
- **Writes are synchronised.** The IPC server already logged from its own
  thread, so two threads could interleave mid-line.

### Added

- **On-screen notifications.** With no Explorer there is no toast host, no tray
  balloon and no taskbar, so anything mshell had to say went to a log file
  nobody has reason to be reading. The case that matters is a failed config
  reload: keeping the previous config running is right *and* completely silent,
  so a broken edit was indistinguishable from one that worked. It now says so on
  screen, with the Lua error. `mshell.notify(text [, kind [, ms]])`,
  `mshell.set_notify{}`, and a `notify` action so `--msg` can raise one too.

  Deliberately mshell's own messages only — real Windows toasts are WinRT/WNS
  and need a registered Explorer-class shell, which a replacement shell is not.

- **Session and power actions:** `lock`, `logoff`, `reboot`, `shutdown`,
  `sleep`, `hibernate`. Replacing Explorer removes every other route to these,
  and `quit` is not a substitute — as the shell, exiting ends the session
  whatever you meant by it.

- **Media keys.** `volume_up`, `volume_down`, `volume_mute`, `media_play`,
  `media_next`, `media_prev`, `media_stop`. A keyboard *with* dedicated volume
  keys already worked (Windows handles those below our hook), but one without
  had no route to volume at all, since every `Win+key` belongs to mshell. The
  media and browser VKs are also bindable now, for a keyboard that has them.

- **Screenshots:** `screenshot` and `screenshot_window`, written to
  `Pictures\Screenshots` and left on the clipboard. PrintScreen is remapped to
  Snip by a shell setting and `Win+Shift+S` is a Win chord and therefore ours,
  so there was previously no screenshot at all.

- **`toggle_always_on_top`**, **`last_window`** (the window-level counterpart of
  `last_desktop`, backed by a per-desktop focus history), and
  **`resize_left/down/up/right`** for floating windows. `move_*` now literally
  moves a floating window instead of being a no-op — a tiled window has no
  position of its own, so there it still swaps places.

- **A panic key.** Starts Explorer alongside mshell and stops the hook binding
  anything, so a misbehaving shell does not need Task Manager. It does not quit,
  because exiting as the shell ends the session — which is what someone reaching
  for a panic key is trying to avoid. Undone by any reload.

- **Vim-style repeat counts inside submaps:** `3j` focuses down three times.
  Only for a digit the map does not already bind, so existing configs (desktops
  on `1`..`9` in the `go` map) are unaffected. Which actions repeat is a
  whitelist — `3q` must not be three quits.

- **`spawn` takes a working directory**, threaded through keybindings, submaps,
  startup programs and a desktop rule's `app`. **`mshell.setenv(name, value)`**
  is the environment half, process-wide so one call covers every launch.

- **Per-state border colours and configurable corners:**
  `set_border{ width, focused, floating, urgent, corners }`. Naming only
  `focused` behaves exactly as the old positional form did.

- **Opt-in urgency tracking** (`mshell.set_urgency(true)`) plus a `jump_urgent`
  action. Off by default because noticing a window flash needs a hook on
  `EVENT_OBJECT_STATECHANGE`, which fires for every control on the system — and
  0.8.0 narrowed the object range specifically to stop that traffic.

- **`rule{ title = "..." }`** — often the only thing separating two windows of
  one app. Fetched lazily and through `SendMessageTimeoutW`, since rule lookup
  runs for every window on the system and a hung app must not block the thread
  that services keybinds.

- **`desktop_rule{ gaps = ... }`** — per-desktop gap overrides.

- **Crash-loop detection.** As the shell a startup crash is a black screen,
  AutoRestartShell relaunches us, and the loop has no exit that does not involve
  Task Manager. Three launches inside a minute start the next run in safe mode
  with `init.lua` skipped. A run that survives a minute clears the counter.
  Skipped under `--test`, and mshell also warns when `AutoRestartShell` is `0`.

- **`mshell.set_log_level("error"|"warn"|"info"|"debug"|"trace")`.** `"info"` is
  the default and `"debug"` is what `--verbose` has always given you.
  `mshell.set_verbose(true)` still works and now means `"debug"`.
- `mshelld.exe` shares the same logger, so `mshelld.log` gets timestamps,
  levels and rotation too. It stays a separate file: the helper may hold a
  different token than the shell, so one file would mean two processes
  appending under different ACLs.

- **Manual (BSP) tiling with tabbed and stacked containers.** `layout_bsp`, plus
  `split_h`/`split_v` (which state where the *next* window goes — there is
  nothing to split until one arrives), `rotate_split`, `toggle_tabbed`,
  `toggle_stacked`, `container_next`/`prev`, `split_grow`/`shrink`.

  The tree deliberately does **not** own the windows: `Desktop.windows[]` stays
  the membership store and the tree is an index reconciled against it on every
  pass. Making it authoritative would have meant rewriting attach policy,
  promote, zoom and the mouse swap, and leaving one layout that works
  differently from the other seven.

- **An app launcher** (`launcher`). Typed through a keyboard-hook capture mode,
  because no overlay here takes focus and the hook swallows keys system-wide —
  taking the foreground instead is the thing Windows makes hardest. Escape, the
  panic key and a sanity timer are the three guards against a stuck capture.

- **Window animation** (`set_animation(ms)`) and **unfocused-window dimming**
  (`set_dim{}`), both off by default. Dimming never makes another process's
  window layered — that changes how it is composited and can leave a
  GPU-accelerated app rendering black; it is a scrim per monitor with the
  focused window punched out of its region.

- **Per-monitor overrides** (`monitor_rule`) for gaps, nmaster, master ratio and
  layout — matched by device name, which is also the hotplug fix: unplugging a
  display renumbers the rest, so windows now remember their display by name and
  return to it on replug instead of piling up on the primary.

- **Rules can place a window**, not just describe it: `desktop`, `monitor`,
  `geometry` and `start_fullscreen`.

- **`set_minimize_policy("never")`** — off by default, since 0.8.0 added
  minimize precisely so a window *could* be got out of the way with no taskbar.
  App-initiated tray hides stay exempt.

- **Focus-follows-mouse and `Mod`+drag** (`set_mouse{follow=, mod_drag=}`).
  Follow is polled rather than hooked; only `mod_drag` installs a WH_MOUSE_LL
  hook, and only while it is enabled.

- **Which-key descriptions** (`desc` in a binding payload) and sorted rows.

- **Reversible registry tweaks** (`mshell --tweaks list|apply|revert`). Applying
  records the previous value first, so reverting restores what you had —
  including "the value did not exist", which a `.reg` undo cannot express. The
  `.reg` files are now generated from the same table by `make regs`.

- **`install.bat /machine`** for a machine-wide (HKLM) install, behind an
  elevation check and a confirmation.

- **An MSI** (`make msi`, via wixl on Linux) that installs the files and
  deliberately does *not* set the Winlogon key — those are different decisions.
  Unsigned.

- **An opt-in update check** (`set_update_check(true)`), notify-only: nothing is
  ever downloaded or applied, because a bad automatic update to the *shell* is a
  black screen at sign-in.

### Internal

- **`overlay.c`** — the backdrop, focus ring, which-key panel and status bar had
  each hand-rolled the same class registration, DPI scaling, font cache and
  double-buffered paint. Shared now, before the notification surface became a
  fifth copy.
- **`layout_hidden`** — monocle's inline `ShowWindow(SW_HIDE)` becomes a flag the
  layout sets and `flush_placements` applies, shared with tabbed containers. It
  stays distinct from `app_hidden`, which is the app hiding *itself* to the tray.
- **`LayoutParams`** — the layout functions read `dt->n_master` and
  `dt->master_ratio` directly, so every knob was per-desktop by construction and
  a per-monitor value had nowhere to come from. They now take parameters
  resolved once in `tile_monitor`, which is what made per-desktop gaps a
  two-line change.

## 0.11.0 — 2026-07-26

The features a tiling WM is expected to have, and the end of the elevated-config
trade-off.

### Added

- **`mshelld.exe`, a privileged helper** — so you no longer have to choose
  between tiling elevated windows and keeping your config out of the
  administrator's hands. It is elevated; mshell is not. It has no config, no
  Lua, no rules, no layout and no keyboard hook: it accepts "put this window at
  this rectangle" and performs it. 19 KB. Entirely opt-in — without it, mshell
  behaves exactly as before and elevated windows float. See INSTALL.md.

  `install.bat` installs it alongside `mshell.exe` and replaces the two in
  lockstep, which is what keeps them from drifting apart across an upgrade: they
  shake hands on a protocol version and refuse a mismatch. Running it stays
  opt-in — `install.bat /helper`, from an administrator prompt, registers the
  logon task and starts the helper without a sign-out; once the task exists,
  later upgrades restart the helper on their own. `uninstall.bat` removes the
  task, so an uninstall does not leave an elevated autostart pointing into a
  folder you have just been told to delete by hand.
- **Sticky windows** (`toggle_sticky`) — a window that follows you to every
  desktop.
- **A scratchpad** — mark a window (`mark_scratchpad`), then summon and dismiss
  it from anywhere (`toggle_scratchpad`).
- **`zoom`** — dwm's swap-with-master, which unlike the existing
  `promote_master` is a toggle.
- **Session persistence.** Layouts, master ratios and master counts survive a
  restart, keyed by desktop name, and you come back to the desktop you left.
  Saved eagerly rather than at shutdown, because the case this exists for —
  `install.bat` upgrading by `taskkill /F` — never reaches a shutdown path.
- **`mshell.exe --check`** validates a config and reports what it produced,
  without starting a shell. A config error is atomic and the fallback keymap has
  six bindings, so sign-in is a bad moment to discover a typo.
- **Mouse drags swap tiles.** A tiled window cannot really be moved — the layout
  owns its geometry — so a drag is interpreted: dropped on another tile, the two
  swap; dropped anywhere else, it snaps back. `mshell.set_mouse(false)` disables
  it.
- **A crash no longer loses your windows.** Desktops and monocle are implemented
  by hiding, so an unclean death used to strand every hidden window exactly the
  way an unclean exit did before 0.8.0. The handler gives them back and saves
  the session before letting the process die.
- **CI** — cross-compiles, runs the host tests, and builds the release zip, all
  on Linux, with warnings as errors.

### Decided

**Desktops span every monitor**, and dwm-style per-monitor tags were considered
and declined. A desktop here is a name you invent rather than a slot you own, so
per-monitor tags would layer a second, differently-shaped namespace over an
already-dynamic set and make `switch_desktop "web"` mean different things
depending on which display had focus. `desktop_rule("name", { monitor = 1 })`
covers the case that motivates it. Recorded in the README so it reads as a
choice rather than an omission.

### Notes on the privilege split

The keyboard hook stays in mshell, deliberately. A low-level hook must return
its verdict inside `LowLevelHooksTimeout`, which a per-keystroke pipe round-trip
cannot promise — and a hook that misses the deadline leaks the swallowed Win
key, the exact failure the dedicated hook thread exists to prevent. The
alternative, compiling the submap state machine into the helper, would put a
config-derived automaton behind the privilege boundary and defeat the point of
keeping the elevated half dumb.

So keybinds while an elevated window *has focus* remain the one reason left to
elevate mshell, and INSTALL.md says so plainly. Everything else — including
tiling those windows — now works from an unelevated shell.

## 0.10.0 — 2026-07-26

Two things you could not do before: see what mshell is doing, and tell it what
to do from outside.

### Added

- **A status bar**, one per monitor. mshell removes the taskbar and previously
  put nothing in its place — which matters more here than in most tiling WMs,
  because desktops are created and destroyed as you work, so the *set* of them
  was invisible too. It shows the live desktop list with the current one marked,
  the active layout, the focused window's title, and a clock.

  It reserves its strip from each monitor's work area, so tiled windows sit
  below it and a fullscreen window still covers it — that needed no new concept,
  since the tiler already lays out into the work area while the fullscreen paths
  use the monitor's full bounds.

  Configured with `mshell.set_bar{}`: `enabled`, `position` (`"top"`/`"bottom"`),
  `height`, `bg`/`fg`/`accent`/`dim`, and `modules`. The module list *replaces*
  the default set, so naming a subset turns the rest off.

- **A control channel.** `mshell.exe --msg "switch_desktop web"` runs any action
  in the already-running shell, and `mshell.exe --query` prints its state as
  JSON (desktops, monitors, focused window) — enough to drive a third-party
  status bar or script a workflow.

  The command vocabulary is the same action table the config uses rather than a
  second set of names, so the two cannot drift and the surface is exactly what a
  keybinding can already do. The pipe is per-session and its DACL is built from
  the process token's own SID, admitting only that user and SYSTEM; if the
  descriptor cannot be built, the server refuses to start rather than falling
  back to a pipe every local account can open.

### Notes

The IPC command surface is deliberately narrow because the next release puts a
privilege boundary on it: the elevated work (the keyboard hook, UIPI-privileged
`SetWindowPos`) moves into a small helper with no config and no scripting, which
removes the elevated-config trade-off documented in 0.8.0 rather than mitigating
it.

## 0.9.0 — 2026-07-26

The config stops being a list of settings and becomes something you can program.
Until now every function in the API was a setter: a config could describe the
world but never ask about it and never react to it, and Lua sat idle between
loads. Now a key can run a function, a function can ask what is going on, and
mshell can call you back when something happens.

**Upgrading from 0.8.0:** two breaking changes, both small.
`mshell.set_gap(n)` is gone — use `mshell.set_gaps(n)`, which already did the
same thing. And the shipped `config/init.lua` is now a ~130-line minimal
default; the previous one is `config/init.full.lua`, installed alongside it. Your
own `init.lua` is untouched, as always.

### Added

- **`mshell.spawn(cmd [, args])`** — programs can finally be given arguments.
  Every launch went through `ShellExecuteW` with a NULL parameter string, which
  is why the old default config had to start Discord and Valorant through
  Start-menu `.lnk` files: a shortcut carries arguments a spawn could not. The
  same `{command, arguments}` form works in a binding, in a submap, and in a
  desktop rule's `app`.
- **Lua functions as keybindings** — `mshell.bind({mod}, "x", function() … end)`,
  and `h = function() … end` inside a submap.
- **`mshell.on(event, fn)`** for `"window_open"`, `"window_close"`,
  `"desktop_switch"` and `"focus"`. The handler gets a table describing what
  happened.
- **State queries**: `mshell.get_monitors()`, `get_desktops()`,
  `get_current_desktop()`, `get_focused_window()`. Note that only the monitor
  list is populated during the *first* config load — the config runs before the
  first desktop exists. All of them are live on a reload and inside a handler.
- `restore` joins `minimize` as a bindable action, and the default config binds
  both — with no taskbar, `restore` is the only way back to a minimized window.

### Changed

- **BREAKING: `mshell.set_gap` removed.** It set both gaps to one value, which
  is what `set_gaps(n)` does — its second argument defaults to the first.
- **BREAKING: the default config is now minimal.** The old one was thoroughly
  commented but hardcoded Alacritty, Firefox, Flow Launcher, Discord, Valorant,
  Steam and Riot paths, so a new user's first boot produced a log full of launch
  failures for software they did not have. The new default assumes nothing but
  Windows and opens `cmd.exe`; one line at the top changes that. Everything else
  moved to `config/init.full.lua`, which `install.bat` always installs beside it
  and always refreshes.
- The three launch sites (keybinding, startup, desktop auto-launch) share one
  `spawn_command()`. They had three copies of the call and three different
  failure messages, one of which was invisible without `--verbose`.
- A malformed `{action, payload}` submap entry is now an error rather than being
  skipped in silence, matching the key handling tightened in 0.8.0.

### Notes on the Lua runtime

Calling into Lua from a running window manager needs a few guarantees, all of
which are in place: handlers run through `lua_pcall`, so a config error becomes a
log line instead of a `longjmp` out through C frames that hold locks; a
re-entrancy guard means an event raised from inside a handler is skipped rather
than recursing; the config-*building* calls (`bind`, `submap`, `rule`, `spawn`,
`on`, …) refuse to run from a handler, because rebuilding keymaps while the
keyboard hook reads them is what a reload takes a lock for; and a Lua binding
queued just before a reload is dropped rather than called, since its registry
reference belongs to a `lua_State` the reload has closed.

## 0.8.0 — 2026-07-26

The release that makes mshell safe to hand to somebody else. No new features to
speak of — this is the bugs that made it unwise to install, the first tests the
project has ever had, and DPI support, without which it simply does not work
correctly on a modern laptop.

**Upgrading from 0.7.0:** nothing in your `init.lua` has to change. Two
behaviours differ and are deliberate: mshell no longer relaunches itself as
administrator when you double-click it (it never elevates itself now), and if
you *do* run it elevated, auto-reload is disabled — see "Elevation" below.
`mshell.submap` is also stricter: a key it does not recognise is now an error
instead of being skipped in silence, so a typo that used to cost you one binding
now reports itself. If a submap key was quietly broken, this is where you find
out.

### Fixed

- **Windows hidden by mshell are no longer stranded on exit.** Virtual desktops
  and monocle are both implemented by hiding windows, and nothing ever brought
  them back — quitting left every window on every other desktop invisible, with
  no taskbar button and no Alt+Tab entry to reach it. Quit is a bound key, so
  this was one keystroke away, and a crash did the same thing.
- **A config reload could execute a freed keybinding.** The keyboard hook posted
  the matched `KeyBinding *` to the main thread, and a reload frees every
  binding — so `Win+Shift+R` followed by any other bound key before the queue
  drained dereferenced freed memory. Key autorepeat alone got you there. Actions
  are now copied by value into a ring buffer.
- **Minimise-to-tray works.** Closing Discord, Slack, Telegram or Steam to the
  tray put the window straight back on screen: the tiler kept it in the layout
  and force-showed it. mshell now tells its own hiding apart from the app's and
  leaves app-hidden windows alone until the app shows them again.
- **Minimized windows release their tile.** Minimizing left an empty cell in the
  layout, and with no taskbar there was no way to get the window back. The
  layout now reflows, and there are `minimize` and `restore` actions.
- **A second instance refuses to start.** The README claimed a single global
  instance; nothing enforced it, so double-clicking `mshell.exe` while it was
  already your shell gave you two keyboard hooks and two tilers fighting over
  the same windows.
- **Moving a window to a full desktop no longer strands it.** The capacity check
  happened after the window had been unlinked and hidden, leaving it owned by a
  desktop whose list did not contain it.
- **The foreground-lock timeout is restored on exit.** mshell zeroes a
  persisted, system-wide setting and never put it back, so uninstalling left
  every application on the machine able to steal focus.
- **`require` works from `init.lua`.** Lua searches only executable-relative
  directories and the working directory, and a Winlogon-launched shell has a
  working directory of `C:\Windows\system32`, so a module beside your config
  could never be found. The config folder is now on `package.path`.
- Left and right modifiers are tracked separately: releasing one Shift while
  holding the other no longer clears the modifier.
- `mshell.submap` no longer calls `lua_tostring` on a table key mid-iteration,
  which is undefined behaviour and reachable with a numeric key.

### Added

- **DPI awareness**, via an application manifest. Without it mshell ran
  DPI-unaware and Windows virtualised its coordinates, so on any scaled display
  every rect the tiler computed was wrong by the scale factor, and on a
  mixed-DPI multi-monitor setup the secondary display was simply incorrect. The
  which-key panel scales its font and metrics per monitor.
  Declared in the manifest rather than by calling `SetProcessDpiAwarenessContext`
  on purpose: that API needs Win10 1703, and importing it would stop the
  executable *loading* on older Windows — which, for the program registered as
  your shell, is a session that cannot start.
- **Version metadata.** The binary carries a name, description and version
  instead of showing a blank publisher everywhere.
- **`minimize` and `restore` actions.**
- **A test suite** — `make test`. The logic with no Windows in it (rule pattern
  matching, the tiling split arithmetic) is built with the host compiler and run
  directly, so it needs no emulator and no Windows machine. `MANUAL-TESTS.md`
  covers the rest, including a regression check for each fix above.

### Changed

- **mshell never elevates itself.** It used to relaunch as administrator on a
  plain double-click, which made elevated the ordinary way to run it. It is
  manifested `asInvoker` and runs with whatever token it was given.
- **Auto-reload is disabled when running elevated.** An elevated mshell executes
  `init.lua` — a Lua script with the full standard library — from
  `%APPDATA%\mshell\`, which the unelevated user can write. With auto-reload on,
  anything running as that user could write the file and get administrator-level
  code execution about 250 ms later with no user action at all. `Win+Shift+R`
  still reloads, keeping a deliberate keypress in the loop. This is a
  mitigation; the fix is to stop needing elevation, which a later release does
  by moving the privileged work into a small helper with no config and no
  scripting. See INSTALL.md.
- `mshell.submap` errors on an unknown or non-string key instead of skipping it.

### Performance

- **Far fewer WinEvents.** The object hook covered a range that silently
  included `REORDER`, `OBJECT_FOCUS`, four `SELECTION` events and `STATECHANGE`
  for every process on the system — events nothing handled, but which fire on
  every control focus and text selection everywhere, each costing a
  cross-process marshal onto the thread that also runs your keybinds.
- **Window rules are resolved once, not three times.** Each lookup opens the
  owning process and queries its image path, on the path that runs for every
  window that appears anywhere — every menu, tooltip and dropdown.
- The cloaked-window check (an RPC to dwm.exe) moved below the local checks that
  reject most windows for free.

## 0.7.0 — 2026-07-25

Desktops stop being a set you configure and become names you use: switch to one
that doesn't exist and it exists, leave it empty and it's gone. What each one
*does* — its app, layout, monitor, whether its windows float — moves into
`desktop_rule`, the desktop counterpart of `mshell.rule`. Window rules gain a
criterion that isn't a name, so file pickers, message boxes and credential
prompts float without anyone having to guess which app raised them.

**Upgrading from 0.6.0:** `set_desktops` and `set_desktop_app` are gone, and
`install.bat` deliberately keeps an existing `%APPDATA%\mshell\init.lua` — it
drops this release's default beside it as `init.lua.new` instead. A config still
calling those functions will *not* load: on reload mshell keeps the previous one
and writes the reason to `%TEMP%\mshell.log`, and at startup it falls back to the
built-in keymap. Copy `init.lua.new` over yours, or port by hand — `set_desktops`
disappears with nothing to replace it (desktops are created by switching to
them), each `set_desktop_app(name, app)` becomes
`desktop_rule(name, { app = app })`, and `switch_desktop 0` becomes
`switch_desktop "1"` if you meant the first desktop rather than one named `0`.

### Changed
- **Desktops are dynamic and identified by name.** There is no longer a desktop
  count, and no index addressing anywhere. A desktop *is* its name — a word
  (`"web"`) or a number (`"1"`), with no difference between the two — and it
  exists only while something is on it: switching to a name nothing is using
  creates that desktop, and leaving one with no windows on it destroys it. At
  startup exactly one desktop exists, the one you land on
  (`set_start_desktop`, default `"1"`).

  The old model made you choose a number up front and then paper over it: nine
  desktops existed whether or not you used them, names were *aliases* for
  indices rather than identities, and a tenth idea meant editing the config.
  Now `switch_desktop "scratch"` always works — whether or not `scratch` appears
  anywhere in your config — and costs nothing once you close its last window.

  Two consequences worth knowing: the desktop you are *standing on* is never
  destroyed however empty it is, so closing everything in front of you leaves
  you somewhere rather than nowhere; and `last_desktop` remembers a *name*, so
  it goes back to a desktop that was destroyed behind you by re-creating it.

  **Breaking:** `mshell.set_desktops(9)` and `mshell.set_desktops{...}` are
  gone, as is `mshell.set_desktop_app`. `switch_desktop` / `move_to_desktop` now
  take a **name** rather than a 0-based index — and since a number is read as
  the name it spells, an old `switch_desktop 0` now means the desktop *called*
  `"0"`. The `Win+1..9` bindings in the default config are now
  `switch_desktop "1"` .. `"9"`, which is what they always looked like they
  meant. A config that still calls the removed functions fails to load with a
  clear error rather than silently doing the wrong thing (and, as always, the
  previous config is kept).
- **`set_layout`, `set_nmaster` and `set_master_ratio` are now defaults**, not
  a stamp applied to every desktop at parse time. They seed each desktop as it
  is created; a `desktop_rule` overrides them per desktop.

### Added
- **Desktop rules — `mshell.desktop_rule(pattern, opts)`.** The desktop
  counterpart of `mshell.rule`, and the one place per-desktop behaviour is
  configured. `pattern` is a desktop name or a case-insensitive wildcard over
  names (`"game-*"`, `"*"`), matched with the same syntax window rules use:

  | field | effect |
  |---|---|
  | `app` | open this whenever you enter the desktop and it has no windows (replaces `set_desktop_app`) |
  | `float` | windows opened here start floating instead of tiled |
  | `layout` | this desktop's layout, overriding `set_layout` |
  | `master_ratio` | master area size for this desktop |
  | `nmaster` | windows in this desktop's master area |
  | `monitor` | pin the desktop to a display (0-based) |

  Rules **layer** rather than compete: every rule whose pattern matches is
  applied in declaration order and each overrides only the fields it names, so a
  `"*"` rule sets the house style and a specific one adjusts a field or two.
  They are resolved when a desktop is created and re-applied on every config
  reload, so editing one takes effect on desktops that already exist.

  `float = true` sets what new windows *start* as — `toggle_float` still works
  per window — and is deliberately checked *after* `set_float_policy("never")`
  vetoes a window rule's float, because a config that tiles aggressively and
  then carves out one floating desktop means it. `monitor` is not advisory:
  windows already on the desktop are moved to that display and switching to the
  desktop takes the focus there. A pin naming a display that isn't there
  (unplugged, or `monitor = 2` on a one-head machine) lapses to "wherever it
  opens" rather than tiling into nothing, and is re-resolved when displays
  change.
- **`next_desktop` / `prev_desktop` actions.** With the set created on demand,
  these are how you reach a desktop you invented on the fly and never bound a
  key to. They step through the desktops that exist *at that moment*, in name
  order — numbers first and numerically (`1, 2, 10`), then words alphabetically
  — and never create or destroy anything. Bound to `Win+]` / `Win+[` and to
  `]` / `[` in the leader's `go` map in the default config.
- **The which-key hint names the desktop.** `switch_desktop` / `move_to_desktop`
  rows now read `web` and `→ web` instead of the action name, which is the whole
  content of a map that is nothing but desktops.
- **Window rules can match what a window *is*, not only what it is called:
  `dialog = true`.** A file picker cannot be named. It is the *host app's*
  process wearing a class the OS handed it, so `process = "firefox.exe"` cannot
  tell Firefox's Open box from Firefox, and there is no third pattern to reach
  for. The new criterion asks Windows instead: a window with a title bar that is
  also any one of three things — the `#32770` class every Win32 common dialog
  carries (Open, Save As, Select Folder, message boxes, task dialogs, print and
  properties sheets), a `WS_EX_DLGMODALFRAME` frame with no maximize box (Qt,
  WinUI, .NET — the maximize test is what keeps ordinary main windows out, since
  setting that style is also the documented trick for hiding a title-bar icon),
  or an owner window (GTK, and most app-modal prompts). The title bar is load-
  bearing: menus, dropdowns, tooltips and autocomplete lists are owned popups
  too, and without it they would all match. It composes with the existing keys,
  so `{ process = "code.exe", dialog = true }` is one app's dialogs and
  `dialog = false` is everything that isn't one.
- **A `dialog` rule is the only thing that can pull in an owned window.** Owned
  windows were dropped before rules ever ran, and `set_manage_owned(true)` — the
  one existing opt-in — turns them *all* on for tiling, which is exactly what
  fixed-size dialogs are worst at. Being dropped looks like floating and mostly
  behaves like it, but such a window is invisible to the WM: it hangs over every
  desktop you switch to and no binding reaches it. A `dialog` rule makes it a
  real floating window instead — hidden with its desktop, focusable, closable.
  Only rules that explicitly ask about dialogs get this, so a broad `path` rule
  (the game-library ones) still can't start swallowing every splash screen and
  error box a game owns.
- **The default config floats system dialogs out of the box.** One
  `mshell.rule({ dialog = true }, "float")` covers pickers and prompts from
  every app, placed above the game rules so a Steam game's error box floats as
  the dialog it is instead of being stripped bare and stretched over the
  monitor. Alongside it: `consent.exe` (UAC — normally drawn on the secure
  desktop where no shell can see it, so the rule only bites where
  `PromptOnSecureDesktop` is off), `CredentialUIBroker.exe` (the "Windows
  Security" PIN/password/Hello box, which is its own process and not owned by
  whatever asked for it), and Explorer's `OperationStatusWindow` copy/move
  progress windows. The XAML capability prompts ("Let this app access your
  camera") need no rule — they are `Windows.UI.Core.CoreWindow` windows, which
  mshell never manages.

## 0.6.0 — 2026-07-25

Fullscreen in three flavours — the window, the app's own content fullscreen, or
both — with a policy for apps that fullscreen themselves. Plus the fix for
0.5.0's worst bug: a single `os.getenv` at the top of the default config could
reject the *entire* file, leaving no keybinds and no startup programs, and the
log that would have explained it was only written under `--verbose`.

**Upgrading from 0.5.0:** `install.bat` deliberately keeps an existing
`%APPDATA%\mshell\init.lua`, so it will *not* replace a config carrying the bug
below. Copy the shipped `config\init.lua` over yours, or apply the `envpath()`
change by hand. `%TEMP%\mshell.log` now says which it was.

### Fixed
- **None of the keybinds worked, and startup programs never launched (0.5.0).**
  The default `init.lua` resolved Flow Launcher's path with
  `os.getenv("LOCALAPPDATA") .. [[\FlowLauncher\...]]` at file scope, near the
  top of the file. When that variable is not set in mshell's environment the
  concatenation raises, and a config error is *atomic*: the entire file is
  rejected, not the failing line. Because the call sat above every `mshell.bind`,
  `mshell.submap` and `mshell.spawn`, nothing at all was registered — mshell fell
  back to its six-binding built-in keymap with an empty startup list, which
  presents exactly as "none of my keybinds work and my terminal didn't open".
  0.4.0 had no file-scope `os.getenv`, which is why it was unaffected. Every
  `os.getenv` path in the default config now goes through an `envpath()` helper
  that returns nil instead of raising, and each dependent feature (the Flow
  Launcher startup + `Win+o a` binding, the Valorant desktop app) drops out on
  its own. A missing optional app can no longer cost you your whole config.
- **A rejected config reported the reason into a file that didn't exist.**
  `log_w()` returns immediately unless `--verbose`, so `config: load failed: …`
  and the fallback-keymap notice were discarded on exactly the runs where they
  mattered — and as the shell there is no console, taskbar or tray to fall back
  on. `%TEMP%\mshell.log` is now always created, and genuine failures go through
  a new always-on `log_err()`: config load failure (with the Lua error), the
  fallback keymap and what it still binds, the live binding/keymap/startup
  counts, and any startup program whose `ShellExecute` failed (so an
  `alacritty.exe` that isn't on `PATH` says so). Per-keystroke tracing is still
  gated behind `--verbose`, so the file stays a few lines long in normal use.
- **`Win+Shift+k` (kill) could never fire.** It was bound twice: `move_up` from
  the `Win+Shift+h/j/k/l` move-window block claimed it first, and `keymap_find`
  returns the first match, so the later `kill` binding was dead. Kill moves to
  **`Win+Shift+x`** (it is also `k` inside the `Win+w` window submap), and
  `keymap_add_binding` now logs a warning when a chord is bound twice in one
  map, so a shadowed binding can't hide silently again.

### Added
- **Fullscreen, in three flavours.** Two different things can go fullscreen and
  mshell now keeps them apart: the *window* (geometry, which mshell owns) and
  the app's own *content* fullscreen — YouTube's fullscreen button, `F11` in a
  browser, where the app switches its own UI and resizes itself to the display.
  Three actions, each its own toggle, and pressing a different one switches
  modes directly: `fullscreen` gives the window its monitor edge to edge without
  telling the app anything (for apps with no fullscreen mode of their own);
  `fullscreen_content` pins the window to its tile so the app's fullscreen
  renders *inside* the window — a fullscreen video fills the tile, not the
  screen; `fullscreen_both` gives the window the monitor and stops policing its
  geometry, so the app's own fullscreen covers the display the way it would
  outside a tiling WM. Bound in the default config to `Win+Shift+f`,
  `Win+Ctrl+f` and `Win+Alt+f`, and to `w`/`i`/`a` in the **window** submap. A
  fullscreen window leaves the layout — the others tile underneath as if it
  weren't there — only one per monitor may cover the screen, and its focus ring
  is suppressed.
- **`mshell.set_fullscreen_policy("contain" | "monitor")`** — what an app that
  fullscreens *itself* gets when its window has no explicit mode. `"contain"`
  (the default, and mshell's behaviour to date) keeps the window in its tile;
  `"monitor"` recognises the app covering its monitor's full bounds, drops the
  window out of the layout for as long as that lasts, and puts it back the
  moment it returns to a smaller rect.
- **`go` and `move` submaps, and desktops declared in one table** (default
  config). Tapping `Win` then `g` enters **go** — one bare key per desktop, and
  you're there: `g b` browser, `g t` terminal, `g d` Discord, `g v` Valorant,
  plus `g 1..9` and `g Tab` for the last desktop. **move** is the same keyset on
  `m`, sending the focused *window* to that desktop instead of taking you there.
  Both are one-shot, so picking a destination is three keystrokes and drops you
  straight back to root with no `Esc`.

  What makes them worth having is that they're generated, along with the desktop
  names and the per-desktop auto-launch, from a single `desktops` table at the
  top of `init.lua` — each row is a name, a key and an optional app:

  ```lua
  { name = "web", key = "b", app = "firefox.exe" },   -- leader g b / m b
  ```

  So `Win`-tap `g b` doesn't just switch desktops, it lands you on a *running
  browser*: `set_desktop_app` opens the app when you enter that desktop while
  it's empty. Adding a desktop is one row — the name, both leader keys and the
  auto-launch follow from it, and the two maps can't drift out of sync. A row
  whose app can't be resolved (not installed, env var unset) loses only its
  auto-launch; the desktop and its keys stay.

  The default config now ships nine **named** desktops (`term`, `web`, `chat`,
  `game`, `files`, then five spares) instead of nine numbered ones. Naming is
  additive, so `Win+1..9` and `Win+Shift+1..9` are unchanged; `Win+v` /
  `Win+Shift+v` now name the `game` desktop rather than hard-coding index 8, so
  reordering the table can't silently repoint them.

  `go`/`move` are deliberately leader-only: `Win+g` and `Win+m` are
  `layout_grid` and `layout_monocle`, and the first binding for a chord wins, so
  binding them at root would have silently shadowed a layout. Both layouts
  remain on `Win+w g` / `Win+w m`; `config/init.lua` carries the commented swap
  if you'd rather have the chords.

  No engine changes — `submap`, `set_desktops{...}` and `set_desktop_app` were
  already there. This is what they compose into, in the config you actually get.
- **Discord on its own desktop, and a `first_existing()` config helper.** Apps
  whose launcher needs *arguments* can't be spawned directly: `spawn` and
  `set_desktop_app` both go through `ShellExecute` with no parameters, so
  Discord's `Update.exe --processStart Discord.exe` never starts. The default
  config points at the Start-menu `.lnk`, which carries the arguments itself —
  the same trick 0.6.0 already used for Valorant — and picks between the
  per-user and machine-wide copies with a new `first_existing(...)` helper that
  returns the first path that exists, or nil. Valorant now goes through it too,
  falling back to the Desktop shortcut. Probing beats guessing: an app you don't
  have yields nil and, exactly like `envpath()`, drops only its own feature.

### Changed
- **The `term` desktop carries no auto-launch app**, because the default config
  keeps starting the terminal with `mshell.spawn("alacritty.exe")`. Give an app
  a startup spawn *or* a desktop `app`, never both — they do **not** cancel out,
  which is worth knowing before you hit it yourself: mshell only auto-launches a
  desktop's app while that desktop is empty, and at startup the spawned
  terminal's window doesn't exist yet when that check runs, so the desktop still
  reads as empty and *both* launches go through, leaving two terminals on every
  boot. Rule of thumb: `mshell.spawn` for things not tied to a desktop that
  should exist from boot (the Flow Launcher startup is unchanged);
  `set_desktop_app` for anything that belongs to one desktop and should come
  back when you return to it. Swapping the terminal to the latter is a two-line
  edit spelled out in `config/init.lua`.

## 0.5.0 — 2026-07-25

Tap `Win` for a Lua-configured leader mode, persisting and one-shot submaps with
a custom exit key, a jump-to-last-desktop toggle, and wildcard window rules whose
new `path` criterion lets one line park a whole borderless-fullscreen game
library over the monitor.

### Added
- **Jump back to the last desktop.** The new `last_desktop` action returns to
  the desktop you switched away from; because every switch records where it came
  from, the pair is a toggle — press it twice and you are back where you
  started. Bound in the default config to ``Win+` `` and to `Tab` inside the
  **desktop** submap. Not `Win+Tab` on purpose: Windows detects that combo below
  mshell's keyboard hook, so Task View would open alongside the switch, while
  bare keys inside a submap never reach the OS. The action is inert until the
  first switch (nothing to go back to), and shrinking the desktop set clamps the
  remembered target so it always names a desktop that exists.
- **Tap `Win` for a leader mode, configured from Lua.** `mshell.set_leader(name)`
  makes a bare `Win` tap — pressed and released with nothing in between — enter
  the named submap, from which single bare keys reach everything (`w` → window,
  `r` → resize, and so on). Tap `Win` again, or press `Esc`, to back out to root.
  There is no built-in leader map and no magic name: the leader is entirely the
  config's choice, and without a `set_leader` call a `Win` tap does nothing. The
  held `Win+key` chords are unchanged and still fire directly.
- **Persisting vs one-shot submaps, with a custom exit key.** `mshell.submap`
  takes `persist = true|false` (default `false`). A one-shot map disables itself
  on the very next key — a bound key fires, anything else is swallowed, and
  either way you return to root. A persisting map stays active so you can fire
  key after key, and is left only by its exit key: `Esc` by default, or a custom
  `exit = "q"` that **replaces** `Esc`. `sticky = true` remains as an alias for
  `persist = true`. The which-key hint now shows each map's flavour and exit key.
- **Borderless-fullscreen game rules.** `mshell.rule` takes two new options
  alongside `ring`: `decorate = false` strips the title bar and adds no border
  at all (floating windows otherwise keep their own chrome), and
  `fullscreen = true` parks the window over its monitor's *full* bounds, gaps
  and work area ignored. With `"float"` that is the whole game preset in one
  rule — never tiled, borderless, covering the display, no ring over its edges:
  `mshell.rule({ path = [[*\steamapps\common\*]] }, "float",
  { ring = false, decorate = false, fullscreen = true })`.
- **Wildcard rule matching, and a new `path` criterion.** `class`, `process` and
  the new `path` (the process's full image path) are case-insensitive patterns:
  `*` matches any run of characters, `?` a single one, and `/` and `\` compare
  equal so paths can be written either way. A pattern with no wildcards is an
  exact match, so existing rules keep matching exactly what they used to — but
  one `path` rule can now cover an entire game library instead of one line per
  executable.

### Changed
- **Entering a submap is now modal (bare keys), not Win-held.** Previously a
  one-shot submap reached via a `Win+key` chord ended the instant you let go of
  `Win`; now you stay in it until the next key (or `Esc`), matching how the same
  map behaves when reached by tapping `Win`. `Win+w` opens the **window** submap
  (was `Win+x`), aligning the chord with the `w` key inside `normal`.

### Fixed
- **Borderless-fullscreen games were invisible to mshell.** Such a window is a
  `WS_POPUP` with no caption and no sizebox — byte-for-byte the style a menu or
  tooltip wears — so the transient-popup filter dropped it before its rule was
  ever consulted: the game stayed on screen through every desktop switch and
  `Win+Shift+c` couldn't reach it. Rules are now consulted *before* that
  heuristic, so naming a window in a rule is the user overriding the guess.
- **Rules were applied once and silently undone.** A game rebuilds its window
  when the graphics device comes up, and again on every resolution change or
  windowed/borderless flip, restoring the frame mshell stripped and moving off
  the geometry it was given. Rule chrome and fullscreen placement are now
  re-asserted whenever the window moves (drift-checked, so mshell's own
  placements don't feed back into a loop).
- **An unknown rule action silently meant `"manage"`.** `mshell.rule(m,
  "floating")` tiled the window instead of floating it, with no diagnostic;
  it now fails the config load with a clear error, which the atomic reload
  turns into "keep the previous config".

## 0.4.0 — 2026-07-24

Config moves into your user profile and reloads itself when you save it, plus
a which-key submap hint, named desktops, and per-rule ring suppression.

### Added
- **Per-rule ring suppression for games.** `mshell.rule(match, action, { ring =
  false })` takes an optional third table; passing `ring = false` stops the
  focus ring from being drawn around matched windows. Paired with the `"float"`
  action it makes a game window fully hands-off — never tiled, never stripped of
  (or given) a frame, kept at the resolution the game asks for, and with no ring
  painted over its edges. The window still opens on the current desktop. List
  one rule per game executable, e.g.
  `mshell.rule({ process = "eldenring.exe" }, "float", { ring = false })`.
- **Which-key submap hint.** Entering a submap (`Win+r`, `Win+x`, …) pops up a
  small panel listing that submap's keys and what each one does — the way
  which-key works in Vim/Emacs — and it clears the moment you leave the submap.
  On by default; configured with
  `mshell.set_whichkey{ enabled, delay, bg, fg, key_fg, border }`, where `delay`
  is the millisecond pause before it appears (`0` = instant). The panel is a
  non-activating layered overlay docked to the bottom-centre of the focused
  monitor; the keyboard hook only posts a message when the active map changes,
  so drawing never touches the input hot path.
- **Named desktops** — `mshell.set_desktops{"term", "web", "chat"}` declares
  desktops by name instead of by count, and the name works anywhere a desktop is
  expected: `switch_desktop`, `move_to_desktop` and `set_desktop_app`. Naming is
  additive — a named desktop still answers to its index, so `Win+1..9` keeps
  working. Names are case-insensitive and must be unique; they resolve while the
  config is parsed, so `set_desktops` must come before any binding that uses one.
  An unknown name now fails the config load with a clear error rather than
  silently resolving to desktop `0` (the old `atoi` behaviour for any
  non-numeric string). `mshell.set_desktops(9)` keeps its numeric meaning and
  clears any names, so reverting to numbers doesn't inherit stale names on
  reload.
- **Per-desktop auto-launch** — `mshell.set_desktop_app(index, "app.exe")`
  opens the assigned app whenever you enter that desktop while it is empty (on
  every `switch_desktop`, and once at startup for the initial desktop). `index`
  is 0-based, matching `switch_desktop`. The launch goes through the normal
  manage path, so window `rule`s and decoration stripping apply to the spawned
  window; an in-flight launch won't be double-spawned by switching away and
  back, and closing the app then returning re-opens it.
- **Auto-reload on save.** A watcher thread on the config's folder reloads the
  config 250 ms after the last write, so saving in your editor is enough —
  `Win+Shift+R` remains for an explicit reload. The debounce avoids reading a
  half-written file (and covers editors that save by write-temp-then-rename),
  the reload still rolls back atomically on a syntax error, and Lua modules
  kept beside `init.lua` count as config too. `mshell.set_auto_reload(false)`
  turns it off.

### Changed
- **The config file now lives in your user profile**, at
  `%APPDATA%\mshell\init.lua` (`C:\Users\<you>\AppData\Roaming\mshell\init.lua`)
  — the standard Windows location for per-user configuration. It is no longer
  part of the program directory, so reinstalling or upgrading mshell can't
  clobber your keybindings.
- `install.bat` installs the default `init.lua` there only when no config
  exists yet, and **migrates** a pre-existing `C:\mshell\config\init.lua` to the
  new location. `uninstall.bat` leaves your config alone.
- **Reinstalling now replaces the installed mshell and restarts it.** Running
  `install.bat` over an existing install always overwrites `C:\mshell\mshell.exe`
  — even when that exe is the shell you are currently running, whose image
  Windows locks (the old one is renamed to `mshell.exe.old`, which is deleted
  once the process using it is gone). The installer then stops the running
  mshell and brings the new build up, so an upgrade takes effect immediately
  instead of at the next sign-in. Winlogon usually relaunches the shell itself
  from the `Shell` value (`AutoRestartShell`); the installer compares pids to
  detect that and does not start a second instance, which would fight over the
  low-level keyboard hook. It refuses to restart at all when
  `AutoRestartShell` is `0` — there, a shell that exits logs the session off —
  and says so if it couldn't stop the old process, leaving the upgrade to take
  effect at the next sign-in. Nothing is restarted when mshell wasn't running.
- `config\init.lua` next to `mshell.exe` still works as a fallback when the
  AppData config is absent, so an unzipped release folder runs `--test` as-is.
- `Win+Shift+R` re-resolves the path before loading, so creating the AppData
  config mid-session takes effect without signing out. The path in use is
  logged at startup (`--verbose`).

### Fixed
- **Keybinds no longer die while a game is running.** A `WH_KEYBOARD_LL` event
  is silently dropped by Windows — passed straight through, our return value
  ignored — whenever the thread that installed the hook isn't scheduled to
  service it within `LowLevelHooksTimeout` (default 300 ms). A fullscreen game
  routinely raises its own priority and saturates the CPU/GPU, starving mshell's
  NORMAL-priority hook thread for far longer than that, so *every* keybind
  stopped working for as long as the game was open (the hook was never
  uninstalled — it came back the instant the game closed, with no restart — it
  was just timed out over and over). The dedicated hook thread now runs at
  `THREAD_PRIORITY_TIME_CRITICAL` (it does nothing but a fast keymap lookup, so
  this costs no real CPU) and the process runs at `ABOVE_NORMAL_PRIORITY_CLASS`
  so the thread that executes each keybind's action stays responsive under the
  same load. Neither is high enough to affect the game.
- **Focus keybinds now actually move the focus.** Two independent causes:
  - `EVENT_SYSTEM_FOREGROUND` (`0x0003`) was handled in the WinEvent callback
    but the hook was only registered for the `EVENT_OBJECT_*` range
    (`0x8000`–`0x800B`), so it was never delivered. The focused-window index
    therefore only changed when *we* changed it: focus given with the mouse (or
    grabbed by an app as it starts) left it stale, and the next `Win+h/j/k/l`
    resolved its target from the wrong window — usually landing on the window
    that already had focus, which looks exactly like the keybind doing nothing.
    Focus events now get their own hook, and the focus ring tracks mouse-driven
    focus too.
  - **`window_focus()` had no foreground rights to spend.** Windows only honors
    `SetForegroundWindow` from a process entitled to the foreground, and the
    entitlement that matters — "received the last input event" — is one a window
    manager never earns: our keys arrive through a low-level hook, which
    *observes* input rather than receiving it, so at the moment we move focus
    win32k sees an idle background process. `SetForegroundWindow` then silently
    downgrades to flashing the window and still reports success, which is
    exactly the reported symptom: the ring moves, the keyboard doesn't.
    `window_focus()` now claims the rights the way the OS wants them claimed, by
    injecting a single `VK 0` keystroke (no scan code, no character, nothing an
    app acts on) so we are the last process to touch the input stream. Unlike
    the usual `AttachThreadInput` workaround this never merges input queues, so
    it cannot strand the held Win key. The activation is then verified against
    `GetForegroundWindow()`, retried once after re-asserting the zero
    foreground-lock timeout, and finally falls back to `SwitchToThisWindow`.
    The injected event is tagged in `dwExtraInfo` so the keyboard hook passes it
    through instead of swallowing it (which would deny us the very credit it is
    sent to earn) or mistaking it for user input.
  - `AllowSetForegroundWindow(ASFW_ANY)` is gone from the focus path: it grants
    *other* processes the right to take the foreground from us, never helped our
    own call, and giving those rights away one line after acquiring them works
    against the fix.
  - A failed activation now logs *which* window kept the foreground, so the
    cause (a UIPI-protected elevated app, an app holding the lock) is visible.
- `SPI_SETFOREGROUNDLOCKTIMEOUT` is applied with `SPIF_SENDCHANGE`, and a
  failure is logged instead of silently leaving activation broken.
- The focused monitor follows every focus change, so `Win+,` / `Win+.` step from
  where you actually are rather than from the last monitor keybind.

## 0.3.0 — 2026-07-24

A tiling overhaul: more layouts, multi-monitor, force-tiled mode, and
flicker-free, pixel-accurate placement.

### Added
- **Four new layouts** on top of master-stack/monocle/grid: **spiral**
  (fibonacci/dwindle), **centered-master**, **bottom-stack**, and **columns**.
  `Win+Space` cycles through all of them; `mshell.set_layout(name)` picks the
  default.
- **`nmaster`** — configurable number of master windows (`Win+Ctrl+j/k`,
  `mshell.set_nmaster(n)`).
- **`cfact`** — per-window size factor within its stack (`Win+r` resize submap:
  `j`/`k` shrink/grow, `0` resets).
- **Multi-monitor tiling.** Each display is enumerated and tiled independently;
  `Win+,` / `Win+.` move focus across monitors and `Win+Shift+,` / `Win+Shift+.`
  send the focused window to another monitor.
- **Force-tiled mode**: `mshell.set_float_policy("never")` downgrades every
  `"float"` rule to `"manage"` and disables `Win+f`, so no window is ever
  stacked on top of a tiled one. Complementary knobs: `set_manage_owned`,
  `set_float_on_top`, `set_min_window_size`.
- **Independent inner/outer gaps** (`mshell.set_gaps(inner, outer)`) plus
  **smart gaps** (`set_smart_gaps` — no gaps when a monitor holds one window).
- **Attach policy** (`mshell.set_attach("end"|"master"|"after")`) — where a new
  window lands in the stacking order.

### Fixed / Changed
- **Placement is batched and flicker-free**: an entire layout pass is applied in
  one `BeginDeferWindowPos`/`EndDeferWindowPos` batch, and windows already at
  their target rect are skipped entirely.
- **Pixel-accurate gaps**: windows are positioned against DWM's real visible
  frame (`DWMWA_EXTENDED_FRAME_BOUNDS`), so the invisible resize border no
  longer makes gaps ~7px too wide on non-stripped windows (browsers, Electron).
- **Even inner/outer gaps**: the old single-gap model produced interior gaps
  twice the size of edge gaps; the inner/outer split is now honored exactly.
- **Drift detection instead of a hair-trigger re-tile**: a moved/resized tiled
  window is only snapped back when it genuinely drifted from the rect we
  assigned it, removing the move→event→re-tile feedback loop that fought apps
  resizing themselves during startup.
- **Self-maximizing tiled windows are restored** back into the grid.
- The `w<50/h<50` placement floor that could make cells overlap on small
  areas/deep stacks is gone; division always tiles the axis exactly.

## 0.2.0 — 2026-07-24

A correctness, robustness, and quality pass over the entire shell.

### Added
- **Focus-indicator ring** around the active window (`border.c`), configurable
  via `mshell.set_border(width, color)` — previously the color/width were
  stored but never drawn.
- **Solid desktop backdrop** (`background.c`) so an empty workspace isn't pure
  black; color via `mshell.set_background(0xRRGGBB)`.
- **Built-in fallback keymap**: if `init.lua` fails to load, a minimal keymap
  (terminal, reload, quit, focus, close) keeps the shell usable instead of
  stranding you at login.
- **First-class submaps**: submap values may be `{"action", arg}`, so submaps
  can `spawn`, `switch_desktop`, and nest — demoed by a new `launch` submap.
- `mshell.set_background()` and `mshell.set_verbose()` config functions.
- `make dist` target and a single-source `VERSION` baked into the binary.
- `README.md`, `LICENSE` (MIT + vendored-Lua notice), `.gitignore`.

### Fixed
- `focus_next` / `focus_prev` actions were declared but never handled — the
  default `desktop` submap did nothing. Now implemented.
- **Config reload is now atomic**: a syntax/runtime error rolls back to the
  previous config instead of silently wiping all keybindings.
- **Reload no longer orphans windows** from their desktops (`set_desktops` now
  preserves window membership instead of zeroing every desktop).
- **Monocle layout is usable**: hides non-focused windows, and focus cycling
  works and reveals the newly-focused window.
- Removed synchronous logging from inside the low-level keyboard hook, which
  risked blowing `LowLevelHooksTimeout` and getting the hook dropped.
- Focus now moves to a surviving sibling after the focused window closes.
- Master-stack no longer overlaps its columns on very narrow widths.
- Directional focus/move (`hjkl`) is now geometric (nearest window) with
  prev/next cycling as a fallback, instead of always cycling.
- Color channel order fixed: `set_border`/`set_background` interpret `0xRRGGBB`
  correctly (previously red/blue were swapped).

### Changed
- WinEvent suppression is a nesting counter, not a single flag (re-entrancy
  safe).
- The message-pump window is a real hidden top-level window, so it actually
  receives `WM_QUERYENDSESSION` / `WM_ENDSESSION`.
- All config string handling uses UTF-8 (`MultiByteToWideChar(CP_UTF8)`);
  spawn commands are no longer length-capped; config files load via `_wfopen`
  so Unicode paths work.
- Command-line flags are matched as whole tokens (`--test`, `--shell`,
  `--verbose`) rather than loose substrings.
- Debug logging is **off by default** (opt in with `--verbose` or
  `mshell.set_verbose(true)`); no `%TEMP%` log file unless enabled.
- `INSTALL.md` elevation guidance now recommends keeping UAC on and demotes
  `EnableLUA=0` to a clearly-warned last resort.

## 0.1.0

Initial release: tiling (master-stack/monocle/grid), virtual desktops, Lua
config, keyboard hook, window rules, install/uninstall + hardening scripts.
