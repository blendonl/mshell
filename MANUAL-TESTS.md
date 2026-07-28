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

With `mode = "top_bar"` (the default):

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

## Floating bar mode

With `mshell.set_bar{ mode = "floating" }` and `"notifications"` in `modules`:

- One panel, in the **middle of the screen**, showing the time large with the
  date under it, then the desktops and layout, then the focused title.
- Tiled windows fill the whole monitor: the panel reserves nothing and sits
  over them.
- Clicking where the panel is reaches the window **underneath** it — it never
  takes focus and never swallows a click.
- Only one panel on a multi-monitor desk. Focus a window on another display and
  it moves there.
- `mshell.exe --msg 'notify hello'` appears **in the panel**, not as a separate
  toast, and the panel grows to fit it and shrinks again when it expires. A
  warn/error notification's dot is yellow/red.
- Several messages list newest-first; long ones wrap rather than being clipped.
- Bind `toggle_bar`: the panel disappears and comes back. While it is hidden, a
  notification appears as an ordinary toast again.
- Switching to `mode = "top_bar"` and back at reload leaves no stray window and
  no duplicate notifications.
- On a scaled display, the panel and its type are proportionate.
- A window that opens floating is centred on the same spot the panel occupies —
  floats centre on the work area, and floating mode reserves none of it. The
  panel is drawn over it and passes clicks through, so this is a look, not a
  loss of function; `toggle_bar` gets it out of the way.

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
- Press `Win+Space` once and wait a second: the new layout **stays**. Then hold
  it so autorepeat cycles through every layout, and release: the layout you
  released on stays. (Regression: the session write that follows every layout
  change tripped the directory watcher, and the self-triggered reload re-applied
  the startup session snapshot — the layout visibly flipped, then snapped back
  ~250 ms later. The log must NOT show `config: file changed on disk` after a
  `Win+Space`; it must show it after actually saving `init.lua`.)

## The start desktop (`default`)

- With no rule claiming `default`, a first run (no `session.txt` beside
  `init.lua`) lands on `"1"`.
- `mshell.desktop_rule("term", { default = true })`, delete `session.txt`,
  restart: you land on `term`. The startup log line reads
  `starting on desktop 'term'`.
- Now switch to another desktop, restart mshell: you come back to that desktop,
  **not** `term` — `default = true` decides a first run only, and the session
  remembers where you were.
- Change it to `default = "always"` and repeat: every restart lands on `term`
  no matter where you were. Switch back to `default = true` and restart once
  more: you are returned to wherever you actually were, since the session was
  being written the whole time.
- Two rules claiming `default` (`"web"` then `"term"`): the **last** one wins.
- These fail the config load with a message that names the problem, and the
  previous config keeps running:
  - `mshell.desktop_rule("game-*", { default = true })` — a pattern, not a name.
  - `mshell.desktop_rule("term", { default = "sometimes" })` — unknown policy.
  - `mshell.set_start_desktop("term")` — removed; the error names the rule to
    write instead.
- Editing `default` and saving reloads the config without moving you: it decides
  where you *start*, and takes effect at the next launch.

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

## Submap routes for those actions

With `init.full.lua`. Every one of these starts with a bare `Win` TAP — nothing
below asks for two keys held at once, and any sequence can be abandoned with
`Esc`. The tests above say what each action should do; this says how to fire it.

- **which-key lists the new maps**: tap `Win` and confirm `+media`, `+system`,
  `+capture` and `+bsp` appear alongside `+window`, `+resize`, `+desktop`,
  `+launch`, `+go` and `+move`.
- **media** (`u`, persisting): `u` then `k`/`j` moves the volume with Windows'
  own indicator; `m` mutes; `Space` plays/pauses; `h`/`l` change track; `s`
  stops. Still in the map afterwards — `Esc` leaves. `u` then `10k` is ten
  volume steps (counts apply; `volume_up`/`down` are on the repeat allowlist).
- **system** (`x`, one-shot): `x` then `r` reloads, `q` quits, `x` panics (see
  "Panic and safe mode"), `i` raises a notification naming the current desktop,
  layout, window count and focused process. `i` is a function binding, so it is
  deliberately ABSENT from the which-key panel — the other four are listed.
- **power** (`x` `p`, one-shot, nested): the panel shows `+power` under `p`.
  `x p l` locks. `x p s`/`h` sleep/hibernate. `x p o`/`r`/`d` log off, reboot
  and shut down — test those only if you mean it. Confirm an unbound key inside
  the map (say `z`) drops back to root having done nothing, and that `Esc` at
  any depth returns to root rather than to the parent map.
- **capture** (`c`, one-shot): `c s` for the whole virtual screen, `c w` for the
  focused window; both land in `Pictures\Screenshots` and on the clipboard.
- **bsp** (`b`, persisting): `b b` puts the desktop in the manual layout, then
  `h`/`v` set the next split's direction, `r` rotates, `t`/`s` make the split a
  tabbed/stacked container, `n`/`p` cycle its children, `=`/`-` resize it. The
  hint panel labels those last two "grow split" / "shrink split".
- **folded into existing maps**: `w Tab` = last window, `w o` = always on top,
  `d u` = jump to urgent (needs `mshell.set_urgency(true)` uncommented, or
  nothing is ever urgent), `o p` = the built-in launcher. Confirm the launcher
  takes your typing immediately — the `launch` map is one-shot, so it has
  already dropped to root by the time the search box is up.

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

## Floating placement

- Open a window with a `"float"` rule (Task Manager will do). It appears in the
  **middle** of the monitor at its own size, in one step — no frame in the
  corner followed by a jump.
- `Win+f` on a tiled window: it keeps the size of the tile it left and moves to
  the centre. `Win+f` again re-tiles it.
- Open a file picker (`Ctrl+O` in any app, with the default `dialog = true`
  rule): centred too.
- On a second monitor, a float centres on the monitor it opened on, not on the
  primary. With `desktop_rule(..., { monitor = 1 })` it centres on the pinned
  display.
- With the bar at the top (and again with `position = "bottom"`), a centred
  float sits in the middle of the space *beside* the bar, never under it.
- A window taller or wider than the work area is pinned to the top-left corner
  of it rather than hanging off two edges.
- `set_float_placement("none")`, save, open a float: it stays exactly where the
  app put it. Windows already open are unaffected until they float again.
- `rule({ process = "Flow.Launcher.exe" }, "float", { center = false })` — that
  overlay keeps its own position while other floats still centre.
- A rule with `geometry = {x, y, w, h}` still lands on that exact rect, and one
  with `fullscreen = true` still covers the monitor.
- Maximise a floating window (its own button, or `Win+Up`): it stays maximised
  rather than being shrunk to a centred rect. Same for a minimised one — it does
  not pop back open to be centred.
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

## Floating windows stay on top

Open one tiled window and one floating one (`Win+f`), overlapping.

- Focus the tiled window with `Win+h`/`Win+l`: the float stays visible on top.
- Click the tiled window where the float does *not* cover it: same — the float
  comes straight back over it rather than staying buried.
- Two overlapping floats: focusing the lower one raises it, and focusing a tiled
  window afterwards leaves the two floats in that same order instead of
  swapping them.
- `toggle_always_on_top` on one of two floats keeps it over the other one.
- `mshell.set_float_on_top(false)` and reload: the old behaviour is back — the
  float sinks behind whatever you focus.
- A float minimized and restored is still on top; one moved to another desktop
  does not raise itself over the desktop you are looking at.

## Hiding a desktop (cloak vs hide)

The bug this guards against is a whole desktop of windows coming back **black**,
so the apps that must appear here are the ones that render off the UI thread —
Chrome or Edge, VS Code, Discord or Spotify (Electron), and something WPF.

- Put a Chromium-based app and VS Code on desktop `1`, switch to `2`, switch
  back. Both draw their real content **immediately** — no black rectangle, no
  blank window that fills in a second later, no needing a resize or a click to
  come to life. Repeat the switch ten times: still clean every time.
- Do the same with a video playing in the browser. It is still playing and still
  visible on return.
- Switch to an **empty** desktop, then type. The keystrokes go nowhere — *not*
  into the window you just left. (Cloaking, unlike hiding, does not disturb the
  foreground, so this is handled deliberately.)
- Send the last window off the current desktop with `move_to_desktop`, then
  type. Same check.
- Monocle: with three windows, cycle focus repeatedly. Each one is fully drawn
  when it comes up. Then switch to another desktop and back — the same single
  window is showing, and no other window flashed on the way.
- Leave monocle for `tiling` (`Win+Space`). All three windows are visible; none
  stayed hidden.
- Close Discord to the tray **while on another desktop**, then click its tray
  icon from that other desktop. Its window does **not** appear over the desktop
  you are on. Switch to the desktop it lives on: it is there and tiled.
- Quit (`Win+Shift+Q`) with windows on three desktops. Every window is visible
  and usable afterwards — a cloaked window that outlives mshell keeps a taskbar
  button that does nothing, so this is the check that matters most.
- Kill `mshell.exe` from Task Manager with windows on three desktops (shell mode
  — Winlogon restarts it). The restarted mshell uncloaks what the dead one left
  behind; the log says `uncloaked N window(s)`.
- `mshell.set_hide_policy("hide")`, save, then switch desktops. Desktops still
  work. Windows may flicker and a GPU-heavy app may briefly blank — that is the
  mechanism, and it is why `"cloak"` is the default.
- Still on `"hide"`, with two windows on one desktop: `Win+Space` into monocle,
  `Win+Space` again to leave it. The second window comes back. Ten times over,
  switching desktops away and back in between. Then read the log: an `app hid
  its own window` line naming a window *mshell* hid is the bug — our own hide,
  delivered late and mistaken for a minimise-to-tray — and the window it names
  never returns, on any layout or desktop.

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
