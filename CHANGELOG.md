# Changelog

All notable changes to mshell are documented here. This project adheres to
[Semantic Versioning](https://semver.org/) (pre-1.0: minor = features + fixes).

## Unreleased

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
