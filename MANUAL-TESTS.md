# Manual test checklist

`make test` covers the logic with no Windows in it — rule pattern matching, the
tiling split arithmetic and the which-key panel's grid. Everything below needs
a real Windows machine,
because it involves the shell, the window manager, or hardware.

Run these against `mshell.exe --test` (alongside Explorer, so quitting exits
instead of logging you out) unless a step says otherwise.
`%LOCALAPPDATA%\mshell\mshell.log` is written on every run and is the first
place to look.

## Regression checks for the 0.8.0 fixes

Each of these **fails on 0.7.0** and must pass now. They are the reason 0.8.0
exists, so they are worth re-running before any release.

| # | Test | Expected |
|---|------|----------|
| 1 | Open windows on 3 desktops, then quit (`Win+Shift+Q`). | Every window is visible afterwards. On 0.7.0 the two background desktops' windows stayed hidden forever — no taskbar button, no Alt+Tab entry. |
| 2 | Hold an autorepeating bound key (e.g. `Win+j`) while pressing `Win+Shift+R`. Then save `init.lua` repeatedly while typing in another window. | No crash. On 0.7.0 the reload freed a keybinding that a queued message was about to dereference. |
| 3 | Close Discord (or Slack/Telegram/Steam) to the tray. | It stays hidden. On 0.7.0 it reappeared immediately. Then click its tray icon: it comes back and rejoins the layout. |
| 4 | Minimize a tiled window. | The others reflow to fill the space. Press the `restore` binding: it comes back. On 0.7.0 the tile stayed empty and there was no way back without a taskbar. |
| 5 | Send a window to another desktop, then switch there. | The window is visible. (Guards against the app-hidden detection misreading mshell's own hide.) |
| 6 | Launch a second `mshell.exe`. | It exits immediately and logs why; the first keeps working. |
| 7 | Run elevated. | The log says so and names the config path; editing `init.lua` does **not** auto-reload. `Win+Shift+R` still works. |
| 8 | Minimize a window, switch desktops, come back. | It is still minimized — not silently restored. |

## Status bar (0.10.0)

- It appears on **every** monitor, at the top, and tiled windows start below it
  rather than underneath it.
- The desktop list updates as desktops are created and destroyed; the current
  one is both coloured and marked with `*`.
- The layout indicator follows `Win+Space`; the title follows the focus; the
  clock advances.
- A **fullscreen** window covers the bar. A **floating** window does not.
- `position = "bottom"` moves it and the reserved strip together.
- On a scaled display the bar is proportionate, not tiny or huge.
- `modules = {"desktops"}` leaves only the desktop list.
- `enabled = false`, save: the bar disappears and windows reclaim the space.

## Control channel (0.10.0)

From a normal terminal, with mshell running:

- `mshell.exe --query` prints JSON and does not start a second shell.
- `mshell.exe --msg "switch_desktop web"` switches the running shell.
- `mshell.exe --msg "layout_monocle"`, `--msg "focus_next"` behave as the
  keybindings do.
- `mshell.exe --msg "nonsense"` prints an error naming the problem.
- With mshell **not** running, `--query` reports that rather than hanging.
- Signed in as a second user, that user's `--query` reaches their own mshell,
  not yours.

## Features added in 0.11.0

- **Sticky**: `toggle_sticky` on a window, switch desktops — it comes with you,
  and the bar's window count follows.
- **Scratchpad**: `mark_scratchpad` on a terminal, switch desktops, then
  `toggle_scratchpad` — it appears here, focused. Again — it hides.
- **Zoom**: from the stack it swaps into master; pressed again from master it
  swaps back out to where the old master went.
- **Session**: change a desktop's layout and master ratio, quit, restart —
  both are restored, and you land on the desktop you left. Then
  `taskkill /F /IM mshell.exe` and restart: still restored (this is the case
  shutdown-only saving would miss).
- **--check**: `mshell.exe --check` on a good config prints counts; on a broken
  one prints the Lua error. Run it while mshell is running and confirm
  `%LOCALAPPDATA%\mshell\mshell.log` is **not** truncated.
- **Mouse**: drag a tiled window onto another — they swap. Drag it onto empty
  space — it snaps back. Drag a floating window — it moves normally.
- **Crash**: not easily forced, but if mshell ever does die, check that windows
  on other desktops are visible afterwards.

## Privileged helper (0.11.0)

- Without `mshelld.exe` running: open Task Manager. It floats; the log notes
  once that a window could not be placed. Everything else tiles normally.
- Start `mshelld.exe` elevated, then reload: Task Manager now tiles.
- `%TEMP%\mshelld.log` records the connection.
- Kill `mshelld.exe` while mshell runs: mshell keeps working, and elevated
  windows go back to floating rather than mshell hanging or crashing.
- Mismatched builds (an old `mshelld.exe` against a new `mshell.exe`) refuse
  each other with a logged protocol-version message.

### Installing it

The mismatch case above is the one the installer exists to prevent, so check
that the pair really does move together.

- **Plain `install.bat`**: `C:\mshell\mshelld.exe` exists afterwards, no
  `mshelld` task is registered (`schtasks /query /tn mshelld` finds nothing),
  and the closing summary points at `install.bat /helper`.
- **`install.bat /helper` unelevated**: refuses with the "needs an administrator
  prompt" message, and `mshelld.exe` is still installed.
- **`install.bat /helper` as administrator**: the task is registered, the helper
  is running immediately (no sign-out), and Task Manager tiles.
- **Upgrade with the helper already running**: re-run plain `install.bat` (no
  flag) from a build with a different `MSHELLD_PROTO_VERSION`. Both binaries are
  replaced, the helper is restarted, and mshell connects — no handshake failure
  in `%TEMP%\mshell.log`. This is the case that silently broke before: the
  singleton mutex means a helper that fails to stop leaves the *old* build
  serving, so confirm the running `mshelld.exe` is the new one.
- **Upgrade unelevated with the helper running**: the copy is staged
  (`mshelld.exe.old` appears), the script says the new helper takes over at the
  next sign-in, and it does.
- **`uninstall.bat` as administrator**: the `mshelld` task is gone and the
  helper is not running. Unelevated, it says so and prints the `schtasks
  /delete` command instead of failing.

## DPI

Needs a scaled display; this is the fix most likely to regress silently.

- **125% / 150% / 200% single monitor.** Gaps are the configured size and equal
  on all sides. The focus ring hugs the window with no offset. The which-key
  panel is legible and proportioned as it is at 100%.
- **Mixed DPI, two monitors** (e.g. 100% + 150%). Tile on both. Geometry is
  correct on the secondary monitor, not just the primary. Move a window across
  with `Win+Shift+.` and it lands correctly.
- **Change the scale factor while running.** Layout and overlays follow.

## Multi-monitor

- `Win+,` / `Win+.` move focus between displays; `Win+Shift+,` / `.` move the
  window. Both monitors re-tile.
- Unplug a monitor while running: windows on it move to a surviving display and
  nothing is stranded off-screen.
- A desktop pinned with `monitor = 1` tiles there, and switching to it moves the
  focus there.

## Fullscreen

The three modes are distinct and each key is its own toggle:

- `fullscreen` — the window covers the monitor; the app is never told.
- `fullscreen_content` — the window keeps its tile, so a fullscreen YouTube
  video fills the tile rather than the screen.
- `fullscreen_both` — the app's own fullscreen covers the display.
- With `set_fullscreen_policy("monitor")`, pressing F11 in a browser takes the
  display and leaving fullscreen puts the window back in the layout.
- An always-on-top utility does **not** show through a fullscreen window, and in
  `--test` mode neither does Explorer's taskbar.
- Leaving fullscreen returns a *floating* window to its previous size, not to
  monitor size.

## Config

- A syntax error in `init.lua` keeps the previous config running and logs the
  reason. It does not strand you.
- A first-load failure falls back to the built-in keymap, and `Win+Shift+R`
  recovers once the file is fixed.
- Saving the file applies it (unelevated only — see check 7).
- `require` a module placed beside `init.lua`: it resolves.
- A submap with a numeric or unknown key errors loudly rather than silently
  ignoring that binding.

## Logging

- `%LOCALAPPDATA%\mshell\mshell.log` is created on first run, and the directory
  with it.
- Every line reads `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] …`.
- **Append, not truncate**: note the last line, restart mshell, and confirm the
  old lines are still above the new startup line. Then `taskkill /F /IM
  mshell.exe` and restart — still appended. This is the case that used to lose
  exactly the evidence a crash was worth having.
- At the default level there is no per-keystroke tracing. Add `--verbose`, or
  `mshell.set_log_level("debug")` and reload, and it appears without a restart.
- `mshell.set_log_level("nonsense")` is a config error naming the valid levels,
  and — being atomic — leaves the previous config running.
- `mshell.set_verbose(true)` still behaves as it always did.
- **Rotation**: run at `"debug"` until the file passes 5 MB (holding a key with
  a bound repeat gets there), then confirm `mshell.log.1` appears and
  `mshell.log` restarts small. Past two rotations, `mshell.log.2` exists and
  there is no `.3`.
- With the helper installed, `mshelld.log` sits beside it and follows the same
  rules.

## New actions

- **always-on-top**: `toggle_always_on_top` on a floating window keeps it over
  the tiled grid; toggling off demotes it. A window that was already topmost on
  its own account is never demoted.
- **last_window**: focus A, focus B, `last_window` -> A, again -> B. After
  closing A it goes to the next most recent instead, not to a dead window.
- **floating move/resize**: `move_*` moves a floating window and still swaps a
  tiled one; `resize_*` changes a floating window's size and is a no-op on a
  tiled one.
- **session**: `lock` locks. Test `logoff`/`reboot`/`shutdown`/`sleep` only if
  you mean it — they do exactly what they say.
- **media**: `volume_up`/`volume_down`/`volume_mute` move the volume and show
  Windows' own indicator. `media_play` controls a playing track.
- **screenshot**: `screenshot` writes a PNG to `Pictures\Screenshots` and puts
  the image on the clipboard (paste it somewhere to confirm).
  `screenshot_window` captures only the focused window, at the same bounds the
  focus ring hugs. A layered/translucent window is captured, not a hole.
- **counts**: in the leader map, `3j` focuses down three times. `3q` quits ONCE
  (counts do not repeat non-motion actions). In the `go` map, `1` still switches
  to desktop 1 rather than starting a count.
- **spawn cwd**: bind `{"spawn", {"cmd.exe", nil, "C:\\Windows"}}` and confirm
  the shell opens there.
- **setenv**: `mshell.setenv("FOO", "bar")`, then spawn `cmd.exe` and `echo
  %FOO%`.

## Notifications

- A syntax error in `init.lua` while mshell is running shows a red-striped toast
  naming the Lua error, and the previous config keeps working.
- `mshell.exe --msg 'notify hello'` raises one from outside.
- Several in quick succession stack, newest at the top, and expire
  independently.
- The toast sits below the status bar, not under it.
- With `set_notify{ desktop_switch = true }`, switching desktops announces it.

## Panic and safe mode

- **panic**: bind it, press it. Explorer appears, the Start menu and Alt+Tab
  work, and no mshell keybinding fires any more. `mshell.exe --msg reload`
  restores normal operation. (Confirm no keybinding can undo it — that is the
  design, not a bug.)
- **safe mode**: make `init.lua` crash or fail at startup, then start mshell
  three times inside a minute. The third run logs SAFE MODE and comes up on the
  built-in keymap without reading the config. Wait a minute with a good config
  and the counter resets.
- mshell warns in the log if `AutoRestartShell` is `0`.

## Borders, urgency and rules

- `set_border{ width = 2, focused = 0xffffff, floating = 0x89b4fa }` — the ring
  changes colour when the focused window is floating.
- `set_border{ corners = "round" }` rounds managed windows' corners; `"square"`
  is the default.
- With `set_urgency(true)`, make a background app flash for attention (a chat
  mention works): its ring turns the urgent colour and `jump_urgent` goes to it,
  switching desktops if needed. Focusing it clears the flag. With urgency off
  (the default), no STATECHANGE hook is installed — check the log.
- `rule({ title = "Picture-in-Picture" }, "float")` floats only that window of a
  browser, leaving the main window tiled.
- `desktop_rule("video", { gaps = 0 })` — that desktop tiles edge to edge while
  the others keep the global gaps.

## Manual tiling (BSP) and containers

- `layout_bsp`, then open three terminals: each splits the one that was focused,
  in the direction `split_h` / `split_v` last named.
- `rotate_split` flips the split holding the focused window.
- `split_grow` / `split_shrink` resize that split, and grow means grow from
  either side of it.
- `toggle_tabbed` on a split shows one window at a time; `container_next` swaps
  which, and focus follows the tab.
- Pressing `toggle_tabbed` again on the same split returns it to a plain split.
- Close a window inside a container: its sibling takes the space, no gap left.
- Switch to `tiling` and back to `bsp`: the dynamic layout works normally in
  between and the tree is rebuilt on return.
- Move a window to another desktop while in bsp — it leaves the tree cleanly.

## Which-key panel

`make test` covers the grid arithmetic (`whichkey_math`), so what is left here
is everything a number cannot tell you: whether it is where you asked for it,
and whether it is still readable.

- Enter a submap with no `set_whichkey` call in the config: the panel is at the
  bottom centre, as it has always been. This is the upgrade check — an existing
  config must look untouched.
- `position` through all nine values, saving between each: `top`, `center`,
  `left`, `right` and the four corners each land where the name says, with the
  same gap to the edge. On a **secondary** monitor too — focus a window there
  first, since the panel follows the focus, not the primary display.
- `margin = 0`: it sits flush against the edge. A large `margin` moves it in
  without letting it grow off the far side.
- `max_width = 0.3` on a wide submap: labels are ellipsized with "…" at a
  character boundary, never cut mid-glyph. Narrow it further until columns are
  dropped, then check the log — it must name how many bindings did not fit.
- `max_height = 0.2`: the panel wraps into more columns rather than growing
  past it.
- `max_rows = 4`: columns break every 4 rows.
- `font = "Consolas"` (or any installed family) and `font_size = 28`: the panel
  re-measures around them — nothing is clipped and the columns still line up.
  A **missing** family (`font = "Nope UI"`) falls back and still renders.
- `border_width = 6`: the outline is 6px on all four sides, none of it clipped.
  `border_width = 0`: no outline at all.
- `opacity = 120`: the desktop shows through. `rounded = false`: square corners.
- All of the above on a **scaled display** (150%+): spacing and font grow with
  it, and the same config gives the same proportions as at 100%.
- Change any of these and save — the panel picks them up on the next submap
  without a restart.

## Launcher

- `launcher` opens it; type "fire" and Firefox is selected.
- Up/Down move the selection, Return runs it, Escape closes.
- Backspace edits the query; the list refilters.
- A query matching nothing ("notepad" if unindexed, or a path) is run as typed.
- **The stuck-capture check**: while it is open, confirm no other keybinding
  fires. Then press Escape and confirm they all work again.
- Open it, then kill mshell from Task Manager and restart: the keyboard is
  normal (capture cannot outlive the process).

## Animation and dimming

- `set_animation(120)`: windows glide to their new tiles rather than jumping.
- During the motion, confirm windows are NOT snapped back — the drift detector
  must not fight the animation.
- `set_animation(0)` restores instant placement.
- `set_dim{enabled = true}`: everything but the focused window is dimmed, and
  the dimming follows the focus.
- Dim with a **GPU-accelerated app** focused (a game, a video, a browser playing
  video) and confirm it still renders — this is the failure mode the punched
  scrim exists to avoid.
- Clicking a dimmed window still reaches it (the scrim is click-through).

## Per-monitor rules and hotplug

- `monitor_rule("*DISPLAY2", { layout = "columns" })` — that display uses
  columns while the other keeps the desktop's layout.
- `monitor_rule(0, { gaps = 0 })` by index also works.
- **Hotplug**: put windows on a secondary display, unplug it — they move to the
  primary. Plug it back in — they RETURN. This is the case an index cannot
  survive.

## Tweaks

- `mshell.exe --tweaks list` prints each tweak, its group and why it exists.
- `--tweaks apply input`, then `list`: those rows say "applied".
- Set one of the tweaked values to something custom yourself first, then apply
  and revert: your custom value comes back, not Windows' default.
- Apply a tweak whose value did not previously exist, then revert: the value is
  **deleted**, not set to a default.
- `--tweaks reg input` prints a .reg file equivalent to what `apply` does.

## Shell-mode only

These cannot be tested with `--test` and need a real install. Have Task Manager
(`Ctrl+Shift+Esc`, never intercepted) ready.

- Sign out and back in: mshell starts, the backdrop paints, startup programs
  launch.
- Lock and unlock: no modifier is stuck afterwards (press a bare letter key and
  confirm it types rather than firing a `Win+` binding).
- Trigger a UAC prompt: same check on return.
- Run `install.bat` over a running install: it replaces the exe and restarts.
- `uninstall.bat`, sign out and in: Explorer returns, and the foreground-lock
  timeout is back to its previous value.
- Run a fullscreen game: keybinds keep responding while it has focus.
