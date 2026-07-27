# Manual test checklist

`make test` covers the logic with no Windows in it — rule pattern matching and
the tiling split arithmetic. Everything below needs a real Windows machine,
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
