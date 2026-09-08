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
| 4 | Minimize a tiled window. | The others reflow to fill the space. Press the `window.restore` binding: it comes back. On 0.7.0 the tile stayed empty and there was no way back without a taskbar. |
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

With `mshell.bar.setup{ mode = "floating" }` and `"notifications"` in `modules`:

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
- Bind `bar.toggle`: the panel disappears and comes back. While it is hidden, a
  notification appears as an ordinary toast again.
- Switching to `mode = "top_bar"` and back at reload leaves no stray window and
  no duplicate notifications.
- On a scaled display, the panel and its type are proportionate.
- A window that opens floating is centred on the same spot the panel occupies —
  floats centre on the work area, and floating mode reserves none of it. The
  panel is drawn over it and passes clicks through, so this is a look, not a
  loss of function; `bar.toggle` gets it out of the way.

## Control channel (0.10.0)

From a normal terminal, with mshell running:

- `mshell.exe --query` prints JSON and does not start a second shell.
- `mshell.exe --msg "desktop.focus web"` switches the running shell.
- `mshell.exe --msg "layout.monocle"`, `--msg "window.focus.next"` behave as the
  keybindings do.
- `mshell.exe --msg "nonsense"` prints an error naming the problem.
- With mshell **not** running, `--query` reports that rather than hanging.
- Signed in as a second user, that user's `--query` reaches their own mshell,
  not yours.

## Features added in 0.11.0

- **Sticky**: `window.sticky.toggle` on a window, switch desktops — it comes with you,
  and the bar's window count follows.
- **Scratchpad**: `window.scratchpad.mark` on a terminal, switch desktops, then
  `window.scratchpad.toggle` — it appears here, focused. Again — it hides.

## Desktop bookkeeping

Each of these **fails before this section was written**. They all have the same
shape: `ManagedWindow.desktop_id` and the desktop's `windows[]` disagreeing, or
something off-screen being handed the keyboard.

| # | Test | Expected |
|---|------|----------|
| 1 | `window.scratchpad.mark` a terminal on `1`, go to `2`, `window.scratchpad.toggle` to summon it, then close it. Make sure `1` has no other windows. | `1` disappears from the bar. Before: summoning set `desktop_id` without unlinking the window from `1`, so closing it left a dead handle behind — `1` never emptied and never went away. |
| 2 | `window.scratchpad.mark` on `1`, `window.scratchpad.toggle` to stow it, go to `2`, come back to `1`. | Still stowed. Before: the switch-in show loop revealed every window on the desktop, and nothing re-hid a float. |
| 3 | Stow the scratchpad, then `window.scratchpad.mark` a *different* window. | The old scratchpad reappears rather than being stranded invisible — nothing else could ever show it once it lost the role. |
| 4 | Minimize the **only** window on `1`, go to `2`, come back. | Still minimized, and nothing is focused. Strengthens test #8 above, which passes today only because it never uses a single-window desktop. |
| 5 | Tray an app (Discord/Slack) that is the focused window on `1`, go to `2`, come back. | Still in the tray. Before: `window_focus` fell back to `SwitchToThisWindow`, which un-hides. |
| 6 | Pin a desktop with `monitor = 1`, put a **floating** window on it, reload the config. | The float is on display 1. Before: only its recorded monitor changed — the tiler never places floats, so it stayed on display 0. |
| 7 | `window.sticky.toggle` a window, then switch to a desktop pinned to another display. | It arrives on that display, not the one it was already on. |
| 8 | With any of the above, check `%LOCALAPPDATA%\mshell\mshell.log`. | A full desktop or a sticky window that could not follow now logs a line instead of failing silently. |
| 9 | Put Discord (or Slack) on `1`, go to `2`, and **while you are on `2`** close it to the tray from its tray icon. Come back to `1`. | Still in the tray. Before: only `SW_HIDE` clears `WS_VISIBLE`, so only it can raise `EVENT_OBJECT_HIDE` for a window *we* hid — but the handler tested the one mechanism that stopped being the first choice in 0.14.9. Every tray-hide on a background desktop read as mshell's own, and coming back un-trayed the app. Distinct from test #5, which trays the window on the desktop you are looking at. |
| 10 | Open four windows on one desktop, focus the **third**, then close the **first**. | Focus stays on the window you were in. Before: the list shifted down under `focused`, which was only clamped, so it came to name the *fourth* window and `window_unmanage` focused that. Repeat with `window.move.to_desktop` on the first window instead of closing it. |
| 11 | Change a desktop's layout with a keybind, then reload the config (`Win+Shift+R`). | The layout is still the one you chose — a reload re-applies the rules, and no rule names that desktop's layout. |
| 12 | Add `mshell.desktop.rule("web", { layout = "monocle" })`, reload, and go to `web`. Then edit it to `"grid"` and reload again. | Monocle, then grid. Note `mshell.layout.set(...)` is a default rather than a rule and is still outranked by a layout you chose at runtime — `desktop_rule("*", { layout = ... })` is how a config insists. |
| 13 | Change a desktop's layout, then restart mshell. | The layout is whatever the config says, not what you switched to. Nothing is remembered across a restart — no `session.txt` is written, and none appears in `%APPDATA%\mshell`. |
| 14 | Put a window its own app pins **always-on-top** (a media player in that mode) on `1` as a float, go to `2`, then change resolution or plug/unplug a display. | It does not appear over `2`, and the log has no `rescued … from off-screen` line for it. Before: it is hidden by being moved 4000 px clear of every display, which is exactly what the hotplug sweep looks for, so it was hauled back on screen still flagged hidden and nothing put it away. Then unplug the display it was on and return to `1`: it comes back somewhere you can see it. |
| 15 | With windows spread over several desktops, switch back and forth with `mshell.log.level("debug")` on. | Each switch logs one `hide:`/`show:` line per window that actually moved, and no longer re-asserts the z-order of every hidden window on every desktop on every pass. |
- **Zoom**: from the stack it swaps into master; pressed again from master it
  swaps back out to where the old master went.
- **No persistence**: change a desktop's layout and master ratio, quit, restart —
  both are back to what the config says, and you land on the desktop `default`
  names. `%APPDATA%\mshell` gains no `session.txt`.
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
- **A whole desktop across a scale boundary.** The case that broke Chrome. With
  the two displays at different scales — a portrait secondary makes it obvious —
  put two or three windows (at least one Chromium: Chrome, Edge, an Electron
  app) on a desktop and move the desktop with `desktop.to_monitor`. Every window
  fills its tile on the new display, first time. Send it back: same. Then do it
  the other way round, by switching to a desktop already up on the other display
  so the two swap — both desktops land correctly, on both screens.

  What the bug looked like: a window covering half the portrait screen, or one
  blown up past the far monitor's edges and spilling onto the other, painted
  flat grey with nothing inside it. Grey that survives a re-tile is the browser
  having given up presenting, not geometry — see `--tweaks apply apps`.
- **No grey band down a Chromium window's sides.** Tile Chrome, Edge or Discord
  on the scaled display and look at its left and right edges: one pixel of the
  app's own border, then content. A band of ~10px at 150% (wider at higher
  scale) means `WS_THICKFRAME` was stripped from a window that draws its own
  frame, and the app's client inset — which that frame exists to hide — is
  showing. `tools/probe_frame.exe <hwnd>` names it: the window, DWM and client
  rects with their insets, plus the colour runs across the edge. An ordinary
  app (alacritty, Notepad) must still be stripped to a bare frame, with its
  window, DWM and client rects identical.

- **The window fills its tile a second later, too.** Right after any of the
  moves above, watch a Chromium window (Chrome, Edge, Discord) for a second: it
  must end up edge to edge in its cell, with no strip of backdrop down its left
  or right side. The browser resizes itself when its own DPI relayout finishes,
  which is after the placement loop has stopped looking, so the correction comes
  from the 250ms janitor tick. `tools/probe_dpiband.exe chrome 20` measures it —
  the colour runs it prints name the owner of any band: the backdrop colour from
  the config means the window is short of its tile, an app's own frame colour
  means the app is insetting its content and the window is placed correctly.
  A window that will not take the rect after four ticks says so in the log, with
  the size it insists on.

- **With animation on** (`mshell.appearance.animation(120)`), repeat the move above.
  Windows crossing the scale boundary jump rather than tween; ones staying on
  their own display still animate.
- **Change the scale factor while running.** Layout and overlays follow.

## Multi-monitor

- `Win+,` / `Win+.` move focus between displays; `Win+Shift+,` / `.` move the
  window. Both monitors re-tile.
- Unplug a monitor while running: windows on it move to a surviving display and
  nothing is stranded off-screen.
- A desktop pinned with `monitor = 1` tiles there, and switching to it moves the
  focus there.
- **The focus ring stays on the monitor you are actually on.** With a different
  desktop up on each display and `mshell.mouse.setup{ follow = true }`, move the
  pointer from a window on one display to a window on the other. The ring
  follows the pointer. Before: `window_focus()` moved `focused_monitor` without
  re-deriving the desktop that hangs off it, so `border_refresh()` asked
  `desktop_get_focused()` and got the window you had just *left* — the ring
  stayed on the far monitor, hugging a window you were no longer in, and the bar
  showed that desktop's layout and title too. Repeat with the pointer parked
  still and the focus moved by closing the last window on one display, by
  summoning the scratchpad, and by jumping to an urgent window: all four reach
  the same place.

### Moving a desktop at runtime (`desktop.to_monitor`)

Needs two displays. Bind
`mshell.keys.bind({mod, ctrl}, ".", function() mshell.desktop.to_monitor(1) end)`
and the same with `0` on `,` for the first few.

- Open two or three windows, press the binding. Every window on the desktop —
  tiled **and** floating — moves to that display and re-tiles there; the focus
  follows. Press the other binding: they all come back.
- `mshell.exe --msg "desktop.to_monitor 1"` does the same to the desktop you are
  on. `--msg "desktop.to_monitor chat 1"` moves `chat` **without** switching to
  it — go there afterwards and its windows are on that display.
- `--msg "desktop.to_monitor 7"` on a two-head machine: a notification says the
  monitor does not exist, and nothing moves.
- **It outranks the rule.** With `mshell.desktop.rule("web", { monitor = 0 })`, move
  `web` to monitor 1 by hand, then save `init.lua` (any edit). After the reload
  `web` is still on monitor 1. `--msg "desktop.to_monitor -1"` clears it and the
  rule takes it back to 0.
- **It survives an unplug.** Move a desktop to the secondary, unplug it — the
  windows fall back to the primary. Plug it back in: they return, without
  touching the binding again.
- **It does not survive a restart.** Move a desktop, quit, start again: it is
  back wherever the rules put it. A pin made by hand lasts as long as mshell
  does; `monitor` on a `desktop_rule` is how it is made to last.

## Fullscreen

The three modes are distinct and each key is its own toggle:

- `window.fullscreen.window` — the window covers the monitor; the app is never told.
- `window.fullscreen.content` — the window keeps its tile, so a fullscreen YouTube
  video fills the tile rather than the screen.
- `window.fullscreen.both` — the app's own fullscreen covers the display.
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

### The action API

`make test` already loads both shipped configs and every README example against
a mock of the API, so a name that does not exist cannot reach a release. What
needs a real machine is what happens when one does anyway, and whether the
labels come out right.

- **A config written for the old API fails usefully.** Put
  `mshell.set_gaps(6, 6)` in `init.lua` and save. The config is rejected, the
  previous one keeps running, and the notification and log say
  `mshell.set_gaps was removed — use mshell.layout.gaps`.
- Same for an action named as a string: `mshell.keys.bind({mod}, "h", "focus_left")`
  says `actions are functions now — write mshell.window.focus.left`.
- Binding an action that needs an argument without one —
  `mshell.keys.bind({mod}, "3", mshell.desktop.focus)` — says so and names the
  function form to use instead.
- Binding something that is not an action — `mshell.keys.bind({mod}, "q", mshell.window)`
  — is refused rather than silently doing nothing.
- **Which-key labels.** Enter a submap built from bare actions (`Win` `w`): each
  key is labelled with its dotted path (`window.close`, `layout.tiling`). Enter
  the `go` map (`Win` `g`): every key shows the desktop name, which comes from
  the `desc` its closure carries. No key shows `?`; a function binding with no
  `desc` shows `lua`.
- **Both vocabularies over the control channel.** `mshell.exe --msg close` and
  `--msg window.close` both close the focused window; `--msg "desktop.focus web"`
  and `--msg "switch_desktop web"` both switch.
- **A count still repeats the right things.** `3` then `Win+j` moves the focus
  three windows; `3` then `Win+Shift+q` quits once, not three times.

### Editor types

- After `install.bat`, `%APPDATA%\mshell\meta\mshell.lua`,
  `meta\types.lua` and `.luarc.json` are all present.
- Open `%APPDATA%\mshell` in an editor with lua-language-server: typing
  `mshell.win` completes to `mshell.window`, hovering `mshell.window.close`
  shows its documentation, and `mshell.window.nope` is underlined.
- A reinstall refreshes `meta\`, but does **not** overwrite a `.luarc.json`
  you have edited.
- Press `Win+Space` once and wait a second: the new layout **stays**. Then hold
  it so autorepeat cycles through every layout, and release: the layout you
  released on stays. The log must NOT show `config: file changed on disk` after a
  `Win+Space`; it must show it after actually saving `init.lua`.

## The start desktop (`default`)

- With no rule claiming `default`, a run lands on `"1"`.
- `mshell.desktop.rule("term", { default = true })`, restart: you land on `term`.
  The startup log line reads `starting on desktop 'term'`.
- Now switch to another desktop and restart mshell: you land on `term` again.
  `default` decides every start, not just the first.
- Two rules claiming `default` (`"web"` then `"term"`): the **last** one wins.
- These fail the config load with a message that names the problem, and the
  previous config keeps running:
  - `mshell.desktop.rule("game-*", { default = true })` — a pattern, not a name.
  - `mshell.desktop.rule("term", { default = "always" })` — a string; the
    message says `default` is true or false now and names the removed session
    file. Same for `"remember"` and any other string.
  - `mshell.set_start_desktop("term")` — removed; the error names
    `mshell.desktop.rule` as the replacement.
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
  `mshell.log.level("debug")` and reload, and it appears without a restart.
- `mshell.log.level("nonsense")` is a config error naming the valid levels,
  and — being atomic — leaves the previous config running.
- `mshell.log.verbose(true)` still behaves as it always did.
- **Rotation**: run at `"debug"` until the file passes 5 MB (holding a key with
  a bound repeat gets there), then confirm `mshell.log.1` appears and
  `mshell.log` restarts small. Past two rotations, `mshell.log.2` exists and
  there is no `.3`.
- With the helper installed, `mshelld.log` sits beside it and follows the same
  rules.

## New actions

- **always-on-top**: `window.on_top.toggle` on a floating window keeps it over
  the tiled grid; toggling off demotes it. A window that was already topmost on
  its own account is never demoted.
- **last window**: focus A, focus B, `window.focus.last` -> A, again -> B. After
  closing A it goes to the next most recent instead, not to a dead window.
- **floating move/resize**: `move_*` moves a floating window and still swaps a
  tiled one; `resize_*` changes a floating window's size and is a no-op on a
  tiled one.
- **session**: `system.lock` locks. Test `system.logoff`/`system.reboot`/`system.shutdown`/`system.sleep` only if
  you mean it — they do exactly what they say.
- **media**: `media.volume.up`/`media.volume.down`/`media.volume.mute` move the volume and show
  Windows' own indicator. `media.play` controls a playing track.
- **screenshot**: `screenshot.screen` writes a PNG to `Pictures\Screenshots` and puts
  the image on the clipboard (paste it somewhere to confirm).
  `screenshot.window` captures only the focused window, at the same bounds the
  focus ring hugs. A layered/translucent window is captured, not a hole.
- **counts**: in the leader map, `3j` focuses down three times. `3q` quits ONCE
  (counts do not repeat non-motion actions). In the `go` map, `1` still switches
  to desktop 1 rather than starting a count.
- **spawn cwd**: bind `{"exec", {"cmd.exe", nil, "C:\\Windows"}}` and confirm
  the shell opens there.
- **setenv**: `mshell.exec.setenv("FOO", "bar")`, then spawn `cmd.exe` and `echo
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
  volume steps (counts apply; `media.volume.up`/`down` are on the repeat allowlist).
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
  `d u` = jump to urgent (needs `mshell.appearance.urgency(true)` uncommented, or
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
  mention works): its ring turns the urgent colour and `window.urgent.jump` goes to it,
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

- `layout.bsp`, then open three terminals: each splits the one that was focused,
  in the direction `layout.split.h` / `layout.split.v` last named.
- `layout.split.rotate` flips the split holding the focused window.
- `layout.split.grow` / `layout.split.shrink` resize that split, and grow means grow from
  either side of it.
- `layout.container.tabbed` on a split shows one window at a time; `layout.container.next` swaps
  which, and focus follows the tab.
- Pressing `layout.container.tabbed` again on the same split returns it to a plain split.
- Close a window inside a container: its sibling takes the space, no gap left.
- Switch to `tiling` and back to `bsp`: the dynamic layout works normally in
  between and the tree is rebuilt on return.
- Move a window to another desktop while in bsp — it leaves the tree cleanly.
- `Win+Space` cycles the seven dynamic layouts and never lands in bsp; from bsp
  it cycles OUT, to tiling. `layout.bsp` (`b b`) is the only way in.

With **two monitors**, the desktop spanning both:

- `layout.bsp`, windows on both displays: each display holds **its own** splits.
  A window is placed once, on the display it lives on — nothing is placed twice
  per pass, and neither screen's windows appear stacked on the other's.
- Build a different structure per display (say tabbed on one, a three-way split
  on the other); both survive a switch to `tiling` and back.
- `layout.split.rotate`, `layout.split.grow` and `layout.container.tabbed` act on the **focused
  window's** display and leave the other one alone.
- Drag or `window.move.to_monitor.next` a window across: it leaves one tree and splits
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

Handing off to mrun, when it is installed (its own checklist lives in that
repo):

- With `mrun.exe` beside `mshell.exe`, `launcher.open` opens **mrun**, not the box
  below.
- With `mrun.exe` only on `PATH`, it still opens mrun.
- With `mrun.exe` neither beside mshell nor on `PATH`, it falls back to the
  built-in box below and nothing errors.
- mshell does not tile mrun's window, ring it, or count it as a window.

The built-in box:

- `launcher.open` opens it; type "fire" and Firefox is selected.
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

## Display settings (resolution, refresh, rotation, HDR, arrangement)

Needs real hardware — a panel that offers more than one refresh rate for the
first half, an HDR-capable one for the second. Everything here changes the
physical display, so run it on a machine you can still reach a keyboard on.

- `mshell.exe --displays` lists each attached display: device name, current
  mode, position, orientation, HDR state, the monitor's own name, and the modes
  it accepts. Runs with mshell **not** running at all, and while it is your
  shell.
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
- Bind `display.hdr.toggle` and press it: HDR flips on the display you are LOOKING at,
  with a notification saying which way. Press it on a monitor that cannot do
  HDR: a warning toast, no change.
- Bind `display.refresh.cycle` with `1` and `-1`: steps through that display's rates at
  the current resolution and wraps, with the new rate in a toast. The
  RESOLUTION must not change. On a 60Hz-only panel: a warning toast instead.
- `rotation = "portrait"` on a secondary: the desktop turns a quarter clockwise,
  the monitor's bounds become tall, tiling reflows into the new shape and the
  bar re-measures. `--displays` says `portrait`. Remove the rule and reload —
  it goes back.
- **Rotation and resolution together.** With `resolution = "2560x1440"` AND
  `rotation = "portrait"` on the same display you get a 1440x2560 desktop, and
  the log line says `2560x1440 ... portrait`. The resolution is the panel's
  unrotated size and stays written that way; asking for `"1440x2560"` here is
  what should be REFUSED, since the panel has no such mode.
- Bind `display.portrait.toggle`: the focused display stands on its end, press again and
  it lies back down. Do it from 270° too — one press returns to landscape, not
  three.
- Bind `display.rotation.cycle` with `1` and `-1`: four quarter turns, clockwise and
  back, wrapping through 0/90/180/270 with each one named in a toast. Rotate a
  monitor you are NOT focused on: nothing happens to it.
- With a display left in portrait, `--displays` still lists its modes as the
  panel's unrotated `WIDTHxHEIGHT` (2560x1440, not 1440x2560), and
  `display.refresh.cycle` still works there — the rates are found against the same
  unrotated mode.
- **Arrangement.** Needs two displays. `position = {0, 0}` on the right-hand one
  and `{-3840, 0}` on the left swaps which side they are on: the mouse crosses
  the other way, tiling follows, and `--displays` reports the new `+x+y`. Reload
  again with the same rule — nothing happens the second time, no flicker, since
  the arrangement already matches.
- `primary = true` on the secondary makes it primary: `--displays` moves the
  `(primary)` marker, that display's position becomes `+0+0` and the other one
  goes negative WITHOUT any position rule naming it. This is the case worth
  reading twice — one rule moves both displays, because the primary must be at
  the origin.
- Write positions from a corner other than the origin — `{1000, 1000}` and
  `{4840, 1000}` — and the pair lands relative to each other with the primary
  back at `+0+0`. The absolute numbers are not honoured, only the offsets.
- **It persists**, unlike a mode. With an arrangement rule in force, quit mshell
  and reboot to Explorer: the displays are still arranged the way the rule said,
  and Windows' display settings agree. This is the documented exception —
  compare with the resolution test above, which comes back the way Windows had
  it.
- **Rollback.** Ask for something Windows refuses (a `position` far off in space
  on a machine where that fails). The log says the arrangement was not changed
  and the desktop is exactly as it was — in particular, log out and back in and
  it is STILL as it was, which is what the rollback is for.
- `mshell.exe --query` and `mshell.monitor.list()` both report `device`,
  `refresh`, `rotation` and `hdr` per monitor; `hdr` is `null`/`nil` (not
  `false`) on a display that cannot do it. Change the rate or the orientation
  outside mshell and query again — the new value is reported, not a cached one.

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

- `mshell.mouse.setup{ speed = 4 }` and save. The pointer slows down immediately,
  and the Settings slider shows 4 if you open it.
- Delete that line and save again. The pointer goes back to the speed you
  started with — *not* to Windows' middle notch, and not to 4.
- `mshell.mouse.setup{ speed = 4, accel = false }`, save, then delete only the
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
- `window.on_top.toggle` on one of two floats keeps it over the other one.
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
- `mshell.window.float_on_top(false)` and reload: the old behaviour is back — the
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
- Send the last window off the current desktop with `window.move.to_desktop`, then
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
- `mshell.window.policy.hide("hide")`, save, then switch desktops. Desktops still
  work. Windows may flicker and a GPU-heavy app may briefly blank — that is the
  mechanism, and it is why `"cloak"` is the default.
- Still on `"hide"`, with two windows on one desktop: `Win+Space` into monocle,
  `Win+Space` again to leave it. The second window comes back. Ten times over,
  switching desktops away and back in between. Then read the log: an `app hid
  its own window` line naming a window *mshell* hid is the bug — our own hide,
  delivered late and mistaken for a minimise-to-tray — and the window it names
  never returns, on any layout or desktop.

## The backdrop and settings broadcasts

The bug this guards against is an intermittent **half-second black screen**
while a borderless-fullscreen app is running. The backdrop is black by default,
so the first thing to do is make it not black: `mshell.appearance.background(0xFF00FF)`
in `init.lua`. A magenta flash is mshell's backdrop; a black one is the display
itself and nothing here will fix it.

Run with `mshell.log.level("debug")` — the lines below are all at debug
level.

- Start a borderless-fullscreen game, leave it in the foreground for ten
  minutes with a second app that raises itself (Remote Desktop, a chat client
  with notifications). No flash, magenta or black. Before the fix this happened
  every minute or two.
- Change a Windows setting that has nothing to do with the layout — the colour
  theme, a power plan, an environment variable. The log says
  `settings: ignoring WM_SETTINGCHANGE`, and no window moves.
- Save `init.lua` with `mshell.mouse.setup{ speed = 4 }` in it. Pointer speed
  changes; the log shows the ignore line marked `(our own broadcast)`, and the
  layout does not churn. Setting the pointer back gives the same.
- Press a focus binding at something that will refuse it — an elevated window
  with no `mshelld.exe` running. The log shows `focus -> ... FAILED` and, right
  after it, an ignored broadcast of our own. Nothing re-tiles.
- Plug in or unplug a second display. Everything still re-measures and re-tiles:
  the work-area change is one of the two settings still acted on, and the
  display change itself arrives separately.
- Change Windows' text size / non-client metrics. Tiled geometry updates — the
  other setting still acted on.
- Switch desktops with a game running, then come back. The sunk windows are
  still off the screen and the backdrop is still at the bottom; nothing of the
  desktop you left is visible.
- With windows on two or three other desktops, force the virtual screen to
  change while you watch the one you are on: plug a display in or out, apply a
  `monitor_rule` with a different `resolution` or `rotation`, or change a
  display's scale. Nothing from the other desktops appears, not even for a
  frame. Before the fix every one of them flashed on and off again, because
  re-placing the backdrop bottomed it and lifted them all with it.

## The `config.update` action

`make test` covers the version comparison and the release-JSON reading
(`test_update_parse`). What it cannot cover is the network, the unpack, and the
hand-off to `install.bat` — after which mshell restarts itself, so the
shell-mode rows below need a real install rather than `--test`.

Bind it if your config has not: `mshell.keys.bind({mod, shft}, "u", "config.update")`.

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

## Restarting over a live session

The case a crash or a `taskkill` leaves behind, and the one a rebuild during
development hits several times an hour. Needs a real install.

| # | Test | Expected |
|---|------|----------|
| 1 | Tile windows across two or three desktops, then kill mshell from Task Manager (never `Win+Shift+Q` — that runs the shutdown path this is about). Let Winlogon restart it. | Every window is managed again: switch desktops and each one hides and shows with the desktop it is on. Before, a stripped window failed the adoption test and stayed on screen over every desktop — visible on any desktop whose own windows did not cover it. |
| 2 | Same again, then read `%LOCALAPPDATA%\mshell\mshell.log`. | `startup: handed back the frame of N window(s) a previous mshell stripped and did not live to restore`, with N the number that had been tiled. Their title bars are briefly back before the rules strip them again. |
| 3 | Switch to a desktop that has nothing on it. | The backdrop, and nothing else. |
