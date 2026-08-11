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

## Floating windows

- **A float remembers which display it is on.** Drag a floating window to your
  second monitor *with the mouse* (both a title-bar drag and a Mod+drag), then
  press the fullscreen key. It fills the display it is **on**, not the one it
  opened on. Same check with `Win+f`-toggling it and with the centring that
  follows — none of them should send it back to the first monitor. The keyboard
  move keys were the only path that used to update this.
- **A float survives losing its display.** Put two or three floats on a second
  monitor, then unplug it (or disable it in display settings). They reappear on
  the primary work area, fully on screen, and **not stacked on top of each
  other** — each is moved the least distance that gets it back. Plug the
  display back in and they return to it.
- **A `geometry` rule cannot strand a window.** Give a rule
  `geometry = { x = 9000, y = 9000, w = 800, h = 600 }` with no display there
  and open that app: the window lands on screen anyway, and the log says it was
  rescued.
- **Tracked windows are not floats.** With `float_on_top` on (the default),
  open an app that shows a small owned dialog or a window below `min_win_w` —
  the kind mshell only tracks. It must **not** sit above every other window,
  and it keeps the ordinary focus ring colour rather than the floating one.
- **A fullscreen float remembers the right rect.** Float a window, fullscreen
  it, leave fullscreen — it returns to where it was. Then: float → fullscreen →
  `Win+f` to tile → `Win+f` to float again → fullscreen → leave fullscreen. It
  returns to the rect it had *this* time round, not one from before it was
  tiled.
- **Fullscreen survives the float toggle.** Fullscreen a **tiled** window with
  the keybinding, then `Win+f`. It stays covering the monitor instead of
  sitting at the tile it no longer owns. Leaving fullscreen then gives it a
  sensible size back.
- **`start_fullscreen` works on a float.** A rule with
  `float = true, start_fullscreen = true` opens covering the monitor.
- **Crash**: not easily forced, but if mshell ever does die, check that windows
  on other desktops are visible afterwards.

## Privileged helper (0.11.0)

- Without `mshelld.exe` running: open Task Manager. It floats; the log notes
  once that a window could not be placed. Everything else tiles normally.
  Switch desktops: Task Manager stays visible — the helper is what lets an
  unelevated shell hide an elevated window at all, and without it this is the
  documented limitation.
- Start `mshelld.exe` elevated, then reload: Task Manager now tiles.
- `%TEMP%\mshelld.log` records the connection.
- Kill `mshelld.exe` while mshell runs: mshell keeps working, and elevated
  windows go back to floating rather than mshell hanging or crashing.
- **Suspend it rather than killing it** — this is the case a kill does not
  cover, and the one that used to hang the shell outright. Get its PID from
  `%TEMP%\mshelld.log` or Task Manager and suspend the process (Process
  Explorer, or `pssuspend`), then press a layout key. mshell must stay
  responsive: keybinds keep working, windows keep tiling, and the log shows the
  250 ms timeout followed once by the breaker message. Resume the process and
  placements start going through it again within a few seconds, with no reload.
- **Open Task Manager as the *first* window on a desktop**, with no helper
  running, then press a layout key. Everything *else* on that desktop tiles;
  only Task Manager is left alone, and the log says it was floated because it
  could not be placed. (Before, one such window failed the whole batch and
  nothing on the desktop moved at all, silently.)
- **Hide and re-show an elevated Chromium window** — an Edge or Chrome window
  started with "Run as administrator" — by switching desktops away and back
  with the helper running. It comes back painted, not blank or offset.
- **A second signed-in user cannot reach your helper.** With fast user
  switching, sign in as another account and confirm it cannot open
  `\\.\pipe\mshelld-<your session id>`. The helper's log names the SID it
  granted the pipe to, which should be yours.
- Mismatched builds (an old `mshelld.exe` against a new `mshell.exe`) refuse
  each other with a logged protocol-version message.

### Hiding and closing elevated windows (protocol v2)

With `mshelld.exe` running:

- Open Task Manager on desktop `1`, switch to `2`: it is gone. Switch back: it
  is back, drawn correctly (not black). Before v2 it stayed on every desktop.
- With Task Manager hidden on a background desktop, quit mshell
  (`Win+Shift+Q`): it is visible afterwards — an elevated window must not be
  stranded cloaked on exit.
- Focus Task Manager and press the close binding (`Win+Shift+c`): it closes.
- Same three with an *admin* terminal or regedit, and with an app run
  explicitly as administrator (right-click → Run as administrator).

## Tracked windows — desktop-bound without tiling

Every window is adopted now: anything mshell does not fully manage still
belongs to a desktop instead of sitting on all of them.

- Open an app's Open/Save dialog (no `dialog` rule in the config), then switch
  desktops: the dialog goes with the desktop you opened it on and comes back
  with it. Before, it stayed on screen everywhere.
- The dialog gets no focus ring and no tile. Focus it and press `Win+f`: it is
  promoted — ring, decorations stripped, in the grid. `Win+f` again floats it,
  exactly like any managed window.
- Open Steam fresh (so its "Updating/Connecting" modal is up while the main
  window appears): the main window stays on the desktop it opened on instead
  of appearing on all of them, and `Win+f` tiles it once the modal is gone.
- Launch an app that starts minimized (e.g. `start /min notepad`): it is
  adopted rather than invisible to the WM, and tiles when restored.
- An `ignore` rule still leaves a window completely alone: on screen across
  every desktop switch, no bindings reaching it.

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
- `set_smart_borders(true)` — with one window on a monitor there is no ring.
  Open a second: the ring appears on the focused one. Close it: the ring goes
  again. Repeat with the second window **floating** (it counts, so the ring
  stays), and on a second monitor holding its own single window (counted per
  monitor, so that one has no ring either). In `monocle` only one window is on
  screen, so no ring — expected. Minimising the second window is the same as
  closing it as far as the ring is concerned.
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
- `Win+Space` cycles the seven dynamic layouts and never lands in bsp; from bsp
  it cycles OUT, to tiling. `layout_bsp` (`b b`) is the only way in.

With **two monitors**, the desktop spanning both:

- `layout_bsp`, windows on both displays: each display holds **its own** splits.
  A window is placed once, on the display it lives on — nothing is placed twice
  per pass, and neither screen's windows appear stacked on the other's.
- Build a different structure per display (say tabbed on one, a three-way split
  on the other); both survive a switch to `tiling` and back.
- `rotate_split`, `split_grow` and `toggle_tabbed` act on the **focused
  window's** display and leave the other one alone.
- Drag or `move_to_monitor_next` a window across: it leaves one tree and splits
  the focused leaf of the other. Nothing is left behind on the display it left.
- Unplug the second display with bsp windows on it: they land on the primary and
  join its tree. Plug it back in — they return.

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
- Press `Win+Space` **twice in quick succession**, while the first move is still
  in flight. The windows re-aim from where they are; they must not jump back to
  where the first move started, and the shell must stay responsive.
- `set_animation(0)` restores instant placement.
- `set_dim{enabled = true}`: everything but the focused window is dimmed, and
  the dimming follows the focus.
- Dim with a **GPU-accelerated app** focused (a game, a video, a browser playing
  video) and confirm it still renders — this is the failure mode the punched
  scrim exists to avoid.
- Clicking a dimmed window still reaches it (the scrim is click-through).

## Changing layout does not lock the shell up

The freeze this guards against needs a window that cannot be made as small as
its cell — Discord, Steam and Spotify all have a minimum size — so open one of
those, not four terminals.

- Open the stubborn app plus three or four other windows on one desktop, then
  cycle layouts with `Win+Space` through all of them, and again with
  `set_animation(120)` on. mshell, the bar and the other windows keep answering
  throughout; the mouse does not stutter.
- The stubborn window ends up wherever it can fit and **stays there** — mshell
  must not keep pulling at it. `%LOCALAPPDATA%\mshell\mshell.log` says
  `a window will not stay where the layout puts it`, naming it, at most once per
  layout change (the guard re-arms a second later, so a line per attempt is
  expected; a line per frame is the bug coming back).
- Move that window (drag it, or `Win+Shift+j`) and change layout again: it is
  re-tiled normally. The guard must expire, not disable the window for good.
- Same run with two monitors at **different scaling factors**, windows on both.

## Per-monitor rules and hotplug

- `monitor_rule("*DISPLAY2", { layout = "columns" })` — that display uses
  columns while the other keeps the desktop's layout.
- `monitor_rule(0, { gaps = 0 })` by index also works.
- **Hotplug**: put windows on a secondary display, unplug it — they move to the
  primary. Plug it back in — they RETURN. This is the case an index cannot
  survive.

## Display settings (resolution, refresh, HDR)

Needs real hardware — a panel that offers more than one refresh rate for the
first half, an HDR-capable one for the second. Everything here changes the
physical display, so run it on a machine you can still reach a keyboard on.

- `mshell.exe --displays` lists each attached display: device name, current
  mode, HDR state, the monitor's own name, and the modes it accepts. Runs with
  mshell **not** running at all, and while it is your shell.
- Take a `WIDTHxHEIGHT@HZ` straight out of that listing, put it in
  `monitor_rule("*DISPLAY1", { resolution = "...", refresh = ... })`, save.
  The display changes on reload; the tiling reflows to the new size; the bar
  re-measures. `--displays` now reports the new mode.
- **Now ask for a mode that does not exist** (`resolution = "9999x9999"`).
  The display is UNCHANGED, and the log says the panel will not do it and
  points at `--displays`. This is the important one: the failure mode being
  guarded against is a black screen on a machine with no Explorer.
- Change the mode yourself in Windows' display settings while mshell runs.
  mshell does **not** put it back. Then `Win+Shift+R`: it does — a reload is
  the config saying so.
- **Session-only**: with a resolution rule in force, quit mshell and reboot to
  Explorer. The display comes back at the mode WINDOWS is configured with, not
  the one in `init.lua`. (Windows' own display settings were never written.)
- **Hotplug**: with a rule for a secondary display, unplug and replug it. The
  rule is applied to it when it returns; the primary is not re-asserted.
- `hdr = true` on an HDR-capable display turns advanced colour on (the desktop
  visibly shifts). On a display that cannot do HDR, the log says so once and
  nothing else happens. Unlike the mode, this one persists — it is a Windows
  setting.
- Bind `toggle_hdr` and press it: HDR flips on the display you are LOOKING at,
  with a notification saying which way. Press it on a monitor that cannot do
  HDR: a warning toast, no change.
- Bind `cycle_refresh` with `1` and `-1`: steps through that display's rates at
  the current resolution and wraps, with the new rate in a toast. The
  RESOLUTION must not change. On a 60Hz-only panel: a warning toast instead.
- `mshell.exe --query` and `mshell.get_monitors()` both report `device`,
  `refresh` and `hdr` per monitor; `hdr` is `null`/`nil` (not `false`) on a
  display that cannot do it. Change the rate outside mshell and query again —
  the new value is reported, not a cached one.

## Tweaks

- `mshell.exe --tweaks list` prints each tweak, its group and why it exists.
- `--tweaks apply input`, then `list`: those rows say "applied".
- Set one of the tweaked values to something custom yourself first, then apply
  and revert: your custom value comes back, not Windows' default.
- Apply a tweak whose value did not previously exist, then revert: the value is
  **deleted**, not set to a default.
- `--tweaks reg input` prints a .reg file equivalent to what `apply` does.

## Pointer settings (speed, acceleration, button swap)

These are Windows' settings rather than mshell's, so the whole point of the
tests is what is left behind. Note what Settings › Bluetooth & devices › Mouse
says **before** you start — the checks below are all against that.

- `mshell.set_mouse{ speed = 4 }` and save. The pointer slows down immediately,
  and the Settings slider shows 4 if you open it.
- Delete that line and save again. The pointer goes back to the speed you
  started with — *not* to Windows' middle notch, and not to 4.
- `mshell.set_mouse{ speed = 4, accel = false }`, save, then delete only the
  `accel` line and save. Acceleration comes back on; the speed stays at 4.
  (Per-field ownership: giving one back must not give the others back.)
- With `speed = 4` applied, quit mshell (`Win+Shift+Q`). The pointer returns to
  its original speed.
- With `speed = 4` applied, sign out and back in **without** quitting cleanly.
  The pointer is at its original speed: mshell never wrote the change into the
  user profile, so nothing survives the session.
- `accel = false`: "Enhance pointer precision" unticks in Settings, and a
  slow-then-fast drag of the same physical distance moves the pointer the same
  distance both times.
- `swap_buttons = true`: the right button becomes primary. Set it back to
  `false` (rather than deleting the line) and it reverts.
- A config that mentions **none** of the three: open Settings and confirm speed,
  precision and button order are all untouched after a full mshell run and quit.
- Crash restore covers an **unhandled exception** (the crash handler in main.c
  restores the pointer alongside the hidden windows). It cannot be exercised
  from Task Manager: `End task` is `TerminateProcess`, which bypasses every
  handler in the process, so nothing runs and nothing is restored. What covers
  that case instead is the setting never having been persisted — kill mshell
  with `swap_buttons = true` applied and the buttons stay swapped until you sign
  out, at which point Windows loads the profile value and they are normal again.

## Floating windows stay on top

Open one tiled window and one floating one (`Win+f`), overlapping.

- Focus the tiled window with `Win+h`/`Win+l`: the float stays visible on top.
- Click the tiled window where the float does *not* cover it: same — the float
  comes straight back over it rather than staying buried.
- Two overlapping floats: focusing the lower one raises it, and focusing a tiled
  window afterwards leaves the two floats in that same order instead of
  swapping them.
- `toggle_always_on_top` on one of two floats keeps it over the other one.
- The float is in the *topmost band*, so nothing has to re-assert it: activate a
  window mshell does not manage (a UAC-elevated console, an installer, an
  Explorer dialog) and the float still sits over it. An app that raises itself a
  moment after activation — Chrome opening a new window, an Electron app taking
  focus into a child — cannot bury it either.
- The status bar still wins: a float dragged over the strip goes *under* it,
  and the launcher, which-key panel and toasts all open over the float.
- A window covering its whole monitor still beats the float: play a video
  fullscreen and the float is gone until you leave fullscreen.
- With `dim_enabled`, focusing a float dims the wallpaper and the other windows
  but never the bar.
- `mshell.set_float_on_top(false)` and reload: the old behaviour is back — the
  float sinks behind whatever you focus. Un-floating with `Win+f` takes the
  window back out of the band immediately, without waiting for a re-tile.
- A float minimized and restored is still on top; one moved to another desktop
  does not raise itself over the desktop you are looking at.
- **Elevated float, helper running**: open Task Manager (high integrity),
  `Win+f` it, then click a tiled window — it stays on top like any other float.
  This is the case that needs `mshelld.exe`: stop the helper
  (`schtasks /end /tn mshelld`) and the same click buries it again, with one
  line in mshell.log saying a floating window could not be kept on top. That
  degradation is the expected behaviour, not a regression — UIPI leaves an
  unelevated shell no way to restack a higher-integrity window.
- Quit mshell with a float still on top: the window is handed the ordinary band
  back rather than staying pinned over everything after the shell is gone. With
  an elevated float, that hand-back goes through the helper too.

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

## The `update` action

`make test` covers the version comparison and the release-JSON reading
(`test_update_parse`). What it cannot cover is the network, the unpack, and the
hand-off to `install.bat` — after which mshell restarts itself, so the
shell-mode rows below need a real install rather than `--test`.

Bind it if your config has not: `mshell.bind({mod, shft}, "u", "update")`.

| # | Test | Expected |
|---|------|----------|
| 1 | Press it while already on the latest release. | One notification: "mshell *x.y.z* is the latest release." Nothing is downloaded and nothing restarts. |
| 2 | Press it with no network (disable the adapter). | "Could not reach GitHub to check for updates." No crash, no partial download left in `%TEMP%\mshell-update`. |
| 3 | Press it twice in quick succession, or hold the key down. | Only one run happens; the log says `update: already in progress`. |
| 4 | Press it from a **portable** copy (`--test`, or an exe outside the installed location). | It downloads, verifies and unpacks, then declines: a warning naming the unpacked folder, and `install.bat` is **not** run. Your shell is unchanged. |
| 5 | Temporarily edit the version in `Makefile` down (e.g. to `0.0.1`), rebuild, install, and press it. | It reports the real latest release as available and proceeds — this is how to exercise the whole path without waiting for a release. |
| 6 | During #5, watch `%LOCALAPPDATA%\mshell\mshell.log`. | `update: sha256 verified (…)` appears before anything is unpacked. |
| 7 | Corrupt the check: with a proxy or by pointing `UPDATE_URL` at a release whose digest will not match, run it. | "The download does not match the hash GitHub published. Nothing was installed." Nothing is unpacked. |

**Shell-mode only** (needs a real install, Task Manager ready):

- Run #5 as the installed shell. mshell waits for the install, then exits and
  Winlogon brings the new build back up. **The check that matters is the log
  banner**: `=== mshell vX.Y.Z starting ===` naming the version just installed.
  That line is the only proof the restart happened — an update that copies the
  exe and leaves the old process running looks identical from the desktop, and
  is the bug this path was rewritten to make impossible. `C:\mshell\
  mshell.exe.old` should be gone afterwards, deleted by the new instance.
  Windows open beforehand are all still there and visible.
- Read `%LOCALAPPDATA%\mshell\install.log` after it: the whole of install.bat's
  output is there, ending in "The caller asked to restart mshell itself".
- Make the install fail (make `C:\mshell` read-only, or delete `harden.reg`
  from the unpacked folder before the copy) and run it. The toast names the
  exit code and the log file, and mshell keeps running on the old build — no
  restart into a half-installed tree.
- Do the same with `AutoRestartShell` set to `0`. mshell installs, then says so
  and does **not** exit — quitting would log the session out. The new build is
  there for the next sign-in.

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
