# mshell

A tiny, keyboard-driven **tiling window manager that replaces `explorer.exe`**
as the Windows shell. No taskbar, no tray, no desktop icons — just windows,
tiled, driven entirely from the keyboard and configured in Lua.

> ⚠️ mshell takes over your Windows shell. Read [INSTALL.md](INSTALL.md) —
> especially the **Recovery** section — before installing. `Ctrl+Shift+Esc`
> (Task Manager) is never intercepted and is your always-available escape hatch.

## Features

- **Seven tiling layouts:** master-stack, monocle (true single-window), grid,
  spiral (fibonacci), centered-master, bottom-stack, and columns — with
  configurable **`nmaster`** (master count) and per-window **`cfact`** sizing.
- **Fullscreen in three flavours**, because the *window* and the app's own
  *content* fullscreen (YouTube's button, `F11`) are different things:
  `window.fullscreen.window` gives the window the whole monitor,
`window.fullscreen.content` pins it
  so the app's fullscreen fills only its tile, and `window.fullscreen.both` lets the
  app's fullscreen cover the display. `set_fullscreen_policy` picks which of the
  last two an app that fullscreens itself gets by default.
- **Multi-monitor**: each display is tiled independently; move focus and windows
  across monitors (`Win+,` / `Win+.`), and send a whole desktop to a display
  (`desktop.to_monitor`) whether or not a rule pinned it there.
- **Force-tiled mode** (`set_float_policy("never")`) so *every* window joins the
  grid and nothing is ever stacked on top of another window.
- **Floating windows are centred** on their monitor rather than left wherever
  the app opened them — `set_float_placement` and a per-rule `center` decide.
- **Flicker-free placement**: a whole layout pass is applied in one
  `DeferWindowPos` batch, windows already in place are skipped, and geometry is
  computed against DWM's real visible frame so gaps are pixel-accurate.
- **Independent inner/outer gaps** (`set_gaps`) with optional **smart gaps**.
- **Directional focus & movement** (`hjkl`) based on real window geometry, with
  next/prev cycling as a fallback.
- **Dynamic virtual desktops** via show/hide — no dependency on the Win10 API.
  A desktop *is* its name (`"web"`, or `"1"` — a number is just a name): switch
  to a name nothing is using and that desktop is created, leave one empty and it
  is destroyed. Nothing to declare, up to 32 alive at once.
- **Desktop rules** (`desktop_rule`) — per-desktop `app` (auto-launch when you
  enter it empty), `float`, `layout`, `master_ratio`, `nmaster` and `monitor`
  pinning, matched by name or wildcard and layered like window rules. The default
  config pairs each with a key, so a bare `Win`-tap `g b` lands you on a
  *running* browser and `m b` throws the focused window at it.
- **Leader mode + submaps.** Point `set_leader` at any submap and a bare `Win`
  tap enters it, so you reach everything with bare keys (`w` → window, `r` →
  resize, `g` → go to a desktop, `m` → move a window to one, …); the `Win+key`
  chords still work too. Submaps are **persisting**
  (stay until an exit key) or **one-shot** (next key drops back to root),
  expressive enough to spawn programs, switch desktops, and nest — with an
  optional **which-key hint** that lists the active submap's keys (delay,
  placement, maximum size, spacing, font and chrome; `set_whichkey`).
- **Lua configuration** that reloads **when you save it** (or on `Win+Shift+R`)
  and is *atomic* — a broken config keeps the previous one instead of
  stranding you.
- **Status bar** in two modes. `top_bar` is a strip on every monitor — the live
  desktop set with the current one marked, the active layout, the focused
  window's title, and a clock — reserving its strip from each monitor's work
  area, so tiled windows sit below it and a fullscreen window still covers it.
  `floating` is instead one panel in the middle of the focused monitor: a large
  clock, the date, the desktops, and mshell's live notifications listed inline.
  It reserves nothing, passes clicks through, and follows the focus between
  displays. Configurable via `set_bar`, the module list can be trimmed or
  turned off entirely, and `bar.toggle` hides it without a reload.
- **Focus ring** around the active window and a **solid desktop backdrop**
  (there is no Explorer to paint one).
- **Window rules** matching class, process or full install path as wildcard
  patterns — float, ignore, strip the frame, or park a window fullscreen over
  its monitor. One rule covers a whole game library; tiled windows can't be
  dragged loose (they snap back) and can't maximize out of the grid. A rule can
  also match **what a window is** rather than what it's called: `dialog = true`
  catches every file picker, message box and permission prompt, whichever app
  raised it, so the default config floats them all in one line.
- **The display itself, from the config**: a `monitor_rule` can state a
  `resolution`, a `refresh` rate, a `rotation` (landscape, portrait, either
  flipped) and whether `hdr` is on, alongside that display's tiling habits —
  because Settings → System → Display is an Explorer-hosted page and there is no
  Start menu to reach it from. Applied at startup, on reload and to a monitor
  you plug in, but never on top of a change you made yourself in Windows;
  validated first, so a resolution the panel cannot show costs a line in the log
  rather than a black screen. `resolution` always names the panel's unrotated
  size, so a rule stays right whichever way the screen is turned. Mode changes
  are session-only and Windows' own stored configuration is left alone, so
  booting *without* mshell gives you your normal display back.
  `mshell.exe --displays` lists every attached display, its device name, its
  position, its orientation, its HDR support and every mode it will accept;
  `display.hdr.toggle`, `display.refresh.cycle`, `display.portrait.toggle` and
`display.rotation.cycle` are
  bindable, for HDR only while a game is up, 60Hz on battery, or standing a
  secondary on its end to read.
- **The arrangement too**: `primary = true` and `position = {x, y}` state which
  display Windows treats as primary and where each one sits, so a config can
  restate a whole multi-monitor desk after a hotplug shuffles it. Positions are
  relative — mshell slides the arrangement until the primary lands on the
  origin, which is the only shape Windows accepts — and a change that is refused
  part-way is rolled back rather than half-applied. Unlike a mode, this one
  persists: batching displays into a single change is only offered alongside a
  registry write, so it is stored the way `hdr` is.
- **Control it from a script**: `mshell.exe --msg "desktop.focus web"` runs any
  action in the running shell, and `mshell.exe --query` prints its state as JSON
  (desktops, monitors, focused window). The pipe is per-session and its DACL
  admits only the owning user.
- **Sticky windows**, a **scratchpad**, dwm-style **zoom**, and mouse
  drag-to-swap between tiles.
- **The things replacing Explorer takes away**: lock, log off, reboot, shut
  down, sleep and hibernate, because there is no Start menu to pick them from;
  volume and media keys, because every `Win+*` combo belongs to mshell and a
  keyboard without dedicated media keys would otherwise have no route to volume
  at all; and screenshots to `Pictures\Screenshots` and the clipboard. The
  worked config reaches all of them through submaps, so none needs a chord.
  **Pointer speed, acceleration and the left/right button swap** come from the
  same place — `set_mouse{speed=, accel=, swap_buttons=}` is the Settings page
  you no longer have. They are *borrowed*, not set: mshell notes what the
  machine had, never writes the change into your user profile, and hands the
  originals back when it exits. A field you don't mention is left alone.
- **Nothing is remembered across a restart**: every start is the one your
  `init.lua` describes. A layout or master ratio you change at runtime lasts as
  long as mshell does, and a config reload keeps it — unless a `desktop_rule`
  names that field, in which case the config states it and the reload puts it
  back. (`set_layout` is a default, not a rule; `desktop_rule("*", {layout =
  ...})` is how a config insists.)
- **An optional privileged helper** (`mshelld.exe`) so an *unelevated* mshell can
  still tile, hide and close windows owned by elevated processes — without your
  `init.lua` ever becoming administrator-level code. No config, no Lua, no
  scripting: it moves, cloaks and closes windows, and nothing else. Installed by
  `install.bat`; started by `install.bat /helper` from an administrator prompt.
- **An optional launcher.** The built-in one (`launcher`) types a name and runs
  a program. If you want modules, Lua configuration and a clipboard/emoji story,
  [**mrun**](https://github.com/notpc/mrun) is a separate app that does that,
  and the `launcher` action prefers it automatically when it is installed. See
  [The launcher](#the-launcher).
- Single global instance, low-level keyboard hook, out-of-context WinEvent
  hooks — no DLL injection.

## Build

Cross-compiled from Linux with **mingw-w64**. You need Lua 5.4 source in
`vendor/lua/`:

```sh
mkdir -p vendor/lua && cd vendor/lua
curl -LO https://www.lua.org/ftp/lua-5.4.7.tar.gz
tar xzf lua-5.4.7.tar.gz --strip-components=1
cd ../..
make            # produces mshell.exe
make test       # runs the host-side unit tests (no Windows needed)
make dist       # produces dist/mshell-<version>-win64.zip
```

The version lives in one place — `VERSION` in the `Makefile` — and is baked into
the binary's startup log, its VERSIONINFO resource, and the release zip name.

`make test` builds the parts with no Windows in them — rule pattern matching and
the tiling arithmetic — with the host compiler and runs them directly, so it
works on the same Linux box you cross-compile from. Everything that needs a real
machine is listed in [MANUAL-TESTS.md](MANUAL-TESTS.md).

## Try it (without committing)

Copy `mshell.exe` and `config/` to the Windows machine and run alongside
Explorer — quitting just exits, it does **not** log you out:

```
mshell.exe --test
```

`%LOCALAPPDATA%\mshell\mshell.log` (and DebugView) is always written, and is the
first place to look when something doesn't work: it records whether your config
loaded, how many bindings it produced, and any startup program that failed to
launch. **If none of your keybinds work, read it** — a config error is atomic, so
a single bad line rejects the whole file and leaves you on a six-binding fallback
keymap. Add `--verbose` for the full per-keystroke trace on top.

Every line is timestamped and carries a level, the file is **appended to** rather
than truncated (so a crash leaves its evidence behind), and it rotates at 5 MB
keeping two older generations alongside it. `mshell.log.level("error" |
"warn" | "info" | "debug" | "trace")` sets the level from your config — `"debug"`
is what `--verbose` gives you, and `"info"` is the default. The privileged
helper writes `mshelld.log` beside it.

## Install as the shell

See [INSTALL.md](INSTALL.md). In short: `install.bat` copies the program to
`C:\mshell`, your config to `%APPDATA%\mshell\init.lua`, and sets the
**per-user** (HKCU) Winlogon `Shell` key. It only affects your account and is
reverted by `uninstall.bat`.

Run it again to upgrade: it replaces the installed exe even while that exe is
your running shell, then restarts mshell so the new build takes over without a
sign-out. Your `init.lua` is never overwritten.

## Two config files

The release ships two, and they are for different moments:

| File | What it is |
|------|-----------|
| `config/init.lua` | **The default.** ~130 lines, assumes nothing is installed but Windows, and opens `cmd.exe` because that is the one terminal every machine has. This is what `install.bat` puts at `%APPDATA%\mshell\init.lua`. |
| `config/init.full.lua` | **The worked example.** Heavily commented: leader menus, per-desktop auto-launch, game rules, which-key layout and styling, event handlers. Installed alongside as reference; copy it over your `init.lua` if you want the lot. |

A default that launched Alacritty, Firefox, Discord and Valorant would greet
most new users with a log full of launch failures, so it doesn't. Everything
interesting is one file away and documented.

## The launcher

The `launcher` action opens a type-a-name-run-a-program box. It indexes both
Start menus, matches subsequences (`fox` finds Firefox), and runs whatever you
typed when nothing matches — so it is a Run box as well as a menu.

It is deliberately small and has no configuration. When you want more,
[**mrun**](https://github.com/notpc/mrun) is a **separate application** built
for exactly that: a modular launcher with its own Lua config, where app
launching is one module and clipboard history, emoji or anything you write
yourself are the same shape.

mshell ships nothing of it and depends on nothing in it, and using it needs no
configuration: the `launcher` action looks for `mrun.exe` beside `mshell.exe`,
then on `PATH`, and falls back to the built-in box when neither has it. Install
`mrun.exe` and the binding you already have starts using it. To bind it
explicitly instead:

```lua
mshell.keys.bind({"LWin"}, "Space",
    function() mshell.exec("mrun.exe") end, { desc = "run" })
```

mshell knows not to tile it — `mrun_Window` is in the ignore list — so it floats
over whatever it covers.

## Default keybindings

Modifier is **`Win`** (swallowed entirely; every `Win+*` combo is mshell's).
Two ways to drive it: hold **`Win`+key** for a chord, or **tap `Win`** on its own
to enter the leader map and use bare keys — `h/j/k/l` focus, `Space` cycles the
layout, `t` opens a terminal. Tap `Win` again or press `Esc` to leave.

Desktops are created by going to them and destroyed when you leave them empty,
so none of the numbered desktops has to exist in advance.

| Keys | Action |
|------|--------|
| `Win+h/j/k/l` | Focus left/down/up/right |
| `Win+Shift+h/j/k/l` | Move the window that way |
| `Win+1..9` / `Win+Shift+1..9` | Go to desktop `1`..`9` / send the window there |
| ``Win+` `` · `Win+[` · `Win+]` | Last desktop · previous · next |
| `Win+,` `.` (+`Shift`) | Focus / move to the previous or next monitor |
| `Win+t` `m` `g` · `Win+Space` | Layout tiling / monocle / grid · cycle |
| `Win+f` · `Win+Return` | Toggle floating · promote to master |
| `Win+Ctrl+h/l` · `Win+Ctrl+j/k` | Master ratio · master count |
| `Win+Shift+f` · `Win+Ctrl+f` · `Win+F11` | Fullscreen: window · inside the tile · both |
| `Win+n` · `Win+Shift+n` | Minimize · restore (there is no taskbar to click) |
| `Win+Shift+c` · `Win+Shift+x` | Close · kill |
| `Win+Shift+Return` | Terminal |
| `Win+Shift+r` · `Win+Shift+q` | Reload config · quit |
| `Win+Shift+u` | Install the latest GitHub release (mshell restarts) |

### What `init.full.lua` adds

Everything below comes from the worked example, not the default. Its `desktops`
table gives each desktop a key *and* a rule — the rule carries the app to open
when you arrive and it's empty — so the leader gets you there in three
keystrokes with the app already running:

| Keys | Action |
|------|--------|
| **Tap `Win`** then `g b` | Go to the **browser** desktop (opens it if empty) |
| **Tap `Win`** then `g t` | Go to the **terminal** desktop |
| **Tap `Win`** then `g d` | Go to the **Discord** desktop |
| **Tap `Win`** then `g v` | Go to the **Valorant** desktop |
| **Tap `Win`** then `g 1..9` / `g Tab` | Go to the desktop named `1`..`9` / to the last one |
| **Tap `Win`** then `g [` / `g ]` | Step through the desktops that exist right now |
| **Tap `Win`** then `m <key>` | Same keys, but send the focused **window** there |

On top of the core bindings above, it adds four submaps and a game desktop:

| Keys | Action |
|------|--------|
| `Win+w` | **window** submap (one-shot; close/kill/float/fullscreen/all 7 layouts, `Tab` = last window, `o` = always on top) |
| `Win+r` | **resize** submap (persisting; ratio + per-window `cfact`; `Esc` exits) |
| `Win+d` | **desktop** submap (persisting; cycle focus, `Tab` = last desktop, `u` = jump to urgent) |
| `Win+o` | **launch** submap (one-shot; terminal, browser, files, `p` = the built-in launcher) |
| `Win+v` / `Win+Shift+v` | Go to / send window to the `game` desktop (Valorant) |
| `Win+Alt+f` | Fullscreen: **both** (the minimal config puts this on `Win+F11`) |
| `Win+Ctrl+i` | A Lua-function binding: logs the current desktop and window |

Everything a shell is expected to have but a tiling WM has no chord left for —
power, volume, screenshots, manual tiling — lives in five more submaps that are
deliberately **leader-only**. A tap and two bare keys reaches any of them, so
nothing needs three keys held at once:

| Keys | Action |
|------|--------|
| **Tap `Win`** then `u` | **media** submap (persisting; `k`/`j` volume, `m` mute, `Space` play, `h`/`l` track, `s` stop) |
| **Tap `Win`** then `x` | **system** submap (one-shot; `r` reload, `q` quit, `u` update, `x` panic, `i` notify current state) |
| **Tap `Win`** then `x p` | **power** submap (one-shot; `l` lock, `s` sleep, `h` hibernate, `o` log off, `r` reboot, `d` shut down) |
| **Tap `Win`** then `c` | **capture** submap (one-shot; `s` whole screen, `w` focused window) |
| **Tap `Win`** then `b` | **bsp** submap (persisting; `b` manual layout, `h`/`v` splits, `t`/`s` tabbed/stacked, `n`/`p` cycle, `=`/`-` resize) |

Counts work in the persisting maps, so `u` then `10k` is ten volume steps. The
destructive power actions are nested a layer deeper on purpose: mshell has no
confirmation dialog, so `Win` `x` `p` `d` being four deliberate taps — `Esc`
bailing out at every one — is what stands between you and an accidental
shutdown.

## Configuration

Your config lives at **`%APPDATA%\mshell\init.lua`**
(`C:\Users\<you>\AppData\Roaming\mshell\init.lua`) — the standard Windows
location for per-user config, so a reinstall never touches it. If that file is
absent, mshell falls back to `config\init.lua` beside `mshell.exe`, which keeps
a portable/unzipped copy working.

### How a binding is written

An action is a **value**, not a string. `mshell.window.close` is the action
itself, so your editor completes it and a typo is a mistake you see as you
write rather than one the config load reports:

```lua
mshell.keys.bind({"LWin"}, "h", mshell.window.focus.left)
```

Anything that has to be *told* something — which desktop, which command — goes
inside a function, because that is the only place an argument can live:

```lua
mshell.keys.bind({"LWin"}, "3",
    function() mshell.desktop.focus("3") end, { desc = "3" })
```

A function has no name, so the which-key panel has nothing to label the key
with unless you give it `desc`. That is the one thing to remember about the
form: **parameterised bindings want a `desc`**.

A **string** in the action position names a submap to enter:

```lua
mshell.keys.bind({"LWin"}, "x", "extra")   -- enters the "extra" submap
```

The same names are callable at runtime, from a binding or an event handler,
and every window verb takes an optional window to act on — the focused one when
you leave it out:

```lua
mshell.window.close()                          -- the focused window
mshell.window.move({ desktop = "web" })
for _, w in ipairs(mshell.window.list({ process = "firefox.exe" })) do
    w:move({ desktop = "web" })                -- windows are objects
end
```

### Editor support

mshell ships the type definitions for its whole API, generated from the same
table the binary dispatches through, so they describe exactly the release you
have installed. `install.bat` puts them in `%APPDATA%\mshell\meta` alongside a
`.luarc.json` that points [lua-language-server][luals] at them.

Open `%APPDATA%\mshell` in any editor with that LSP and you get completion over
`mshell.*`, signatures and documentation on hover, and a diagnostic on anything
mshell does not have — including a removed name, which says what replaced it.

[luals]: https://github.com/LuaLS/lua-language-server

### The rest

See [`config/init.lua`](config/init.lua) for the full commented example.
Highlights:

```lua
mshell.layout.gaps(6, 6)                  -- inner gap, outer gap
mshell.layout.smart_gaps(true)            -- no gaps when a monitor has one window
mshell.appearance.border(2, 0xffffff)     -- focus ring: width, 0xRRGGBB
mshell.appearance.smart_borders(true)     -- no ring when a monitor shows one window
mshell.appearance.background(0x000000)    -- desktop backdrop

mshell.desktop.rule("1", { default = true })          -- the desktop you land on
mshell.desktop.rule("web", { app = "firefox.exe" })   -- open it when empty
mshell.desktop.rule("game", { float = true, app = "steam.exe" })
mshell.desktop.rule("chat", { layout = "monocle", monitor = 1 })

mshell.layout.set("tiling")     -- tiling|monocle|grid|spiral|centered|bstack|columns
mshell.layout.master.count(1)   -- windows in the master area
mshell.desktop.attach("master") -- new windows become master (dwm-style)

mshell.window.policy.float("never")       -- force EVERY window into the grid
mshell.window.policy.placement("center")  -- floats land mid-monitor ("none" = leave them)

-- persist = false (the default) is one-shot: the next key drops back to root.
mshell.keys.submap("launch", {
    Return = { function() mshell.exec("alacritty.exe") end, desc = "alacritty" },
    b      = { function() mshell.exec("firefox.exe") end,   desc = "firefox" },
    p      = mshell.launcher.open,
})
```

**API.** Everything is grouped by what it acts on, and every name is a value
rather than a string: `mshell.window.*`, `mshell.desktop.*`, `mshell.monitor.*`,
`mshell.display.*`, `mshell.layout.*`, `mshell.bar.*`, `mshell.appearance.*`,
`mshell.keys.*`, `mshell.system.*`, `mshell.media.*`, `mshell.exec`,
`mshell.notify`, `mshell.log`, `mshell.config.*`, `mshell.on`.

The full list is not reproduced here, because a list in a README goes stale.
It is generated from the same table the binary dispatches through and shipped
as `meta/mshell.lua`, so your editor can show it to you — see
[Editor support](#editor-support) below. `make meta` regenerates it and CI
fails if the committed copy has drifted.

**Manual tiling.** Alongside the seven dynamic layouts there is `bsp`: windows
split wherever you were, `layout.split.h`/`layout.split.v` decide the direction the next one
takes, and any split can become a **tabbed** or **stacked** container showing one
window at a time. The tree does not replace the window list — it is an index
over it — so a desktop moves between `bsp` and the dynamic layouts freely. Each
display gets its own tree, and `Win+Space` deliberately does not cycle into
`bsp`: a layout you build by hand is one you ask for, with `mshell.layout.bsp`.

**A launcher.** `mshell.launcher.open` opens a filter over your Start-menu
shortcuts; anything that matches nothing is run as typed, so it is a Run box too.
It types without ever taking focus, because the keyboard hook hands it keys
directly. `init.full.lua` puts it on `Win` `o` `p` — in the one-shot `launch`
submap rather than on the persisting leader, because a map you are still *in*
would be swallowing keys the moment the launcher closed.

**Registry tweaks you can undo.** `mshell.exe --tweaks list` shows what is
applied and why; `apply` records the previous value before writing, so `revert`
restores exactly what you had rather than Microsoft's default.

**Notifications.** There is no Explorer, so there is no toast host and no tray —
mshell paints its own. A config that fails to reload says so on screen with the
Lua error, which matters because the atomic rollback that keeps your previous
config running is otherwise completely silent. `mshell.notify("text", "warn")`
raises one yourself, and `mshell.exe --msg 'notify hello'` from a script does the
same. It is deliberately mshell's own messages only: real Windows toasts are
WinRT/WNS and require being a registered Explorer-class shell.

**A panic key.** The `panic` action — `Win` `x` `x` in `init.full.lua` — starts
Explorer alongside mshell and stops the hook binding anything, so a shell that is
misbehaving does not need Task Manager to escape. It deliberately does not quit —
exiting as the shell ends the session, which is the thing you were avoiding. Any
reload undoes it (`mshell.exe --msg reload`, or saving `init.lua`); no keybinding
can, because not binding keys is the point.

**Updating from a keybinding.** The `update` action — `Win+Shift+u`, and `Win`
`x` `u` in `init.full.lua` — fetches the latest GitHub release, checks the
download against the SHA-256 the release published, unpacks it and runs the
`install.bat` inside it. That script is the upgrade path either way: it renames
the running image rather than overwriting it and restarts mshell itself, so the
screen blinks and you come back on the new build. Progress, and every way it can
fail, arrives as a notification.

This is the deliberate opposite of `set_update_check`, which only ever *tells*
you a release exists. An updater that swapped out the shell unattended would
turn a bad release into a black screen at sign-in with no desktop left to fix it
from; a key you pressed, while sitting in front of the machine, is a different
proposition. Two things it will not do: run twice at once, and install when the
running mshell is not the registered shell — from a portable copy or `--test`,
`install.bat` would not be upgrading anything, it would be taking over your
shell for the first time. In that case it unpacks and tells you where, and you
run `install.bat` yourself. `mshell.exe --msg update` works too.

**Safe mode.** Three starts inside a minute means the previous two did not
survive one, so the next run skips `init.lua` entirely and comes up on the
built-in keymap with the reason in the log. As the shell, a config that crashes
at startup otherwise loops with no way in.

**Auto-reload.** mshell watches the folder holding your `init.lua` and reloads
250 ms after the last write, so saving in your editor applies the config —
`Win+Shift+R` is still there for an explicit reload. The debounce means a
save-in-progress isn't read half-written, and the atomic-reload rollback means
a syntax error leaves the running config untouched (the error goes to the log).
Extra modules you keep beside `init.lua` and `require()` count too. Turn it off
with `mshell.config.auto_reload(false)` if you'd rather reload by hand.

**Window rules & games.** `mshell.window.rule(match, action, opts)` matches on `class`,
`process` (the .exe name) and/or `path` (its full image path). Each criterion is
a case-insensitive wildcard pattern — `*` any run, `?` one character, `/` and
`\` interchangeable — so a pattern without wildcards is an exact match, and one
`path` pattern covers an entire game library. All the keys you give must match;
rules are tried in order and the first match wins, so a specific rule placed
above a broad one carves an exception out of it. `opts` takes `ring = false`
(no focus ring painted over the window), `decorate = false` (strip the title bar
and add no border — floating windows otherwise keep their own chrome) and
`fullscreen = true` (park it over the monitor's full bounds, ignoring gaps).
The three together are the game preset:

```lua
mshell.window.rule({ path = [[*\steamapps\common\*]] }, "float",
            { ring = false, decorate = false, fullscreen = true })
```

**System dialogs.** Some windows can't be named: a file picker is the *host
app's* process wearing a class the OS handed it, so no `process` or `class`
pattern separates Firefox's Open box from Firefox. `dialog = true` matches them
by what they are: a window with a title bar that is *also* one of the
common-dialog class (`#32770`: Open, Save As, Select Folder, message boxes,
print and properties sheets), a modal dialog frame without a maximize box (Qt,
WinUI, .NET), or an owned window (GTK, and most app-modal prompts). The title
bar is what keeps menus, dropdowns and tooltips out — those are owned popups
too.

```lua
mshell.window.rule({ dialog = true }, "float")                     -- every picker/prompt
mshell.window.rule({ process = "code.exe", dialog = true }, "manage")  -- except this app's
```

Owned windows are the one kind a `dialog` rule *fully* manages that mshell
otherwise only tracks. Every window is adopted in one of two tiers: *full*
(tiled, decorated, ringed) or *tracked* — desktop membership and nothing else.
An unmatched owned window lands in the tracked tier: it hides and shows with
the desktop it opened on, is focusable, closable and movable between desktops,
but keeps its own chrome and is never tiled. Matched by a `dialog` rule it
becomes a real floating window, and `Win+f` on any tracked window promotes it
to full management the same way. The shipped config uses the one-liner above
plus named rules for `consent.exe` (UAC, only visible if the secure desktop is
disabled) and `CredentialUIBroker.exe` (the "Windows Security" PIN/password
box), which are separate processes rather than dialogs of the app that
triggered them. `dialog = false` is the inverse, for a rule that should skip
dialogs. An `ignore` rule is the way to leave a window on every desktop at
once — it stays completely untouched.

Naming a window in a rule also rescues it from the "caption-less popup =
menu/tooltip" filter — the exact style a borderless-fullscreen game uses — so
the game stays a real managed window: it hides on desktop switch and
`Win+Shift+c` can close it. Because games rebuild their window when the
graphics device comes up (and again whenever you change resolution or flip
windowed/borderless in their options), the frame and the fullscreen geometry are
re-asserted whenever the window moves, not just once when it opens.

**Force every window to tile.** Set `mshell.window.policy.float("never")`: any
`"float"` rule is downgraded to `"manage"` and `Win+f` becomes a no-op on
managed windows, so nothing is ever stacked on top of a tiled window. (Tracked
windows — owned/modal dialogs and the like — are still only desktop-bound,
not tiled, unless you also set `set_manage_owned(true)`, which is aggressive:
many dialogs are fixed-size and tile poorly.)

**A float is an overlay.** Floating windows stay above the tiled grid — that is
the default, and it holds across focus changes: focusing a tiled window, with a
keybind or with the mouse, does not bury the float you were looking at, because
activation raising the tiled window is undone on the spot. Floats keep their own
order among themselves, with the focused one on top, and a float that has been
promoted to always-on-top or fullscreen sits above the rest of them. Pass
`mshell.window.float_on_top(false)` for the old behaviour, where a float is an
ordinary window in the stack and sinks behind whatever you focus next.

**Floating windows land in the middle.** A float is the window you deliberately
kept out of the grid, so mshell centres it on its monitor's work area rather
than leaving it wherever the app opened it — both a window that opens floating
and one `Win+f` just untiled. Only the position is decided: the size stays the
app's own, clamped to fit. `mshell.window.policy.placement("none")` turns it off,
and `center = false` (or `true`) in a rule's opts answers for one app — useful
for an overlay that already positions itself well. A rule with an explicit
`geometry = {x, y, w, h}` or `fullscreen = true` places the window itself and is
unaffected either way.

**Desktops are dynamic, and a desktop is its name.** There is no desktop count
to configure and no `1..9`: a desktop is a *name* — a word (`"web"`) or a number
(`"1"`), with no difference between them — and it exists only while something is
on it. Switching to a name nothing is using **creates** that desktop; leaving one
with no windows on it **destroys** it. At startup exactly one desktop exists, the
one you land on:

```lua
mshell.desktop.rule("term", { default = true })   -- start here; "1" if nothing claims it

-- "web" is created on demand; "3" is a name, not an index
mshell.keys.bind({mod}, "w",
    function() mshell.desktop.focus("web") end, { desc = "web" })
mshell.keys.bind({mod, shft}, "w",
    function() mshell.window.move.to_desktop("web") end, { desc = "→ web" })
mshell.keys.bind({mod}, "3",
    function() mshell.desktop.focus("3") end, { desc = "3" })
```

`default` is a desktop rule like any other, so where you begin sits beside that
desktop's app, layout and monitor instead of in a setter somewhere else. It needs
a literal name rather than a pattern — mshell has to create exactly one desktop
at startup — and if two rules claim it, the **last** one wins.

It decides **every** start, not just the first: mshell keeps no memory of where
you were, so a restart puts you back on the desktop your config names. Nothing to
clear, nothing to drift.

Names are case-insensitive (the first spelling to create the desktop is the one
displayed), can't contain whitespace, and are capped at 63 characters. Because a
name never has to be declared, `desktop.focus "scratch"` always works whether or
not `scratch` appears anywhere in your config — you can invent a desktop at any
time and it costs nothing once you close its last window.

Two consequences worth knowing:

- The desktop you are *standing on* is never destroyed, however empty it is —
  closing everything in front of you leaves you somewhere, not nowhere.
- `desktop.focus.last` remembers a **name**, so it goes back to a desktop that was
  destroyed behind you by re-creating it (empty, with its rules applied).

Since the set changes under you, `desktop.focus.next` / `.prev` step through
whatever exists at that moment, in name order — numbers first and numerically
(`1, 2, 10`), then words alphabetically. That's how you get back to a desktop you
made on the fly and never bound a key to.

**How a desktop is taken off the screen (`set_hide_policy`).** A desktop you are
not on is a set of windows that have been removed from view, and *how* they are
removed decides whether they come back looking right. mshell **cloaks** them
through DWM — the window keeps rendering and DWM simply stops compositing it,
which is exactly the mechanism Windows' own virtual desktops use.

The obvious alternative, `ShowWindow(SW_HIDE)`, is what mshell did before 0.13.0
and it has a visible failure mode: clearing a window's visible bit makes DWM
throw away its redirection surface, and every app that renders off the UI thread
— anything Chromium or Electron based (Chrome, VS Code, Discord, Spotify), WPF,
Qt on D3D — *also* reads it as being occluded and shuts its renderer down. On the
way back there is a fresh, empty surface and nothing has asked the app to draw
into it, so the window comes back **black**. Because the tiler skips windows that
are already in the right place, no resize arrived to shake them out of it either,
and a whole desktop's worth of apps could come back blank at once.

```lua
mshell.window.policy.hide("cloak")   -- default
mshell.window.policy.hide("hide")    -- pre-0.13.0 ShowWindow(SW_HIDE)
```

Cloaking alone is **not** what fixes the blackness, though — an app stops
presenting whether it learns it is invisible from the hide or from its own
occlusion tracking noticing the cloak. What fixes it is that a window coming
back is always *asked to draw*: `RedrawWindow` over it and its children, plus a
forced re-placement carrying `SWP_FRAMECHANGED` and `SWP_NOCOPYBITS` so the
tiler cannot skip it for already being in the right place. That applies to both
policies, which is why `"hide"` is a usable escape hatch rather than a way back
to the bug.

**Desktop rules — what a desktop does.** `mshell.desktop.rule(pattern, opts)` is
the desktop counterpart of `mshell.window.rule`. `pattern` is a desktop name or a
case-insensitive wildcard over names, matched with the same `*`/`?` syntax window
rules use:

```lua
mshell.desktop.rule("web",    { app = "firefox.exe" })
mshell.desktop.rule("chat",   { app = discord, layout = "monocle" })
mshell.desktop.rule("game-*", { float = true, monitor = 1 })
```

| field | effect |
|---|---|
| `app` | open this whenever you enter the desktop and it has no windows |
| `float` | windows opened here start floating instead of tiled |
| `layout` | this desktop's layout, overriding `set_layout` |
| `master_ratio` | master area size for this desktop, `0.2 .. 0.9` |
| `nmaster` | windows in this desktop's master area |
| `monitor` | pin the desktop to a display (0-based) |

A pin is also a runtime operation, which is what the rule alone could not be:
`desktop.to_monitor` as an action (bindable, and reachable as
`mshell.exe --msg "desktop.to_monitor chat 1"`), or `mshell.desktop.to_monitor`
from a binding or an event handler. What you set by hand outranks the rule and
survives a reload, an unplug/replug and a restart, until `-1` clears it.

Rules **layer** rather than compete: every rule whose pattern matches is applied
in declaration order, and each overrides only the fields it names. So a `"*"`
rule sets the house style and a specific one adjusts a field or two:

```lua
mshell.desktop.rule("*",       { layout = "tiling" })
mshell.desktop.rule("scratch", { float  = true     })   -- still layout = tiling
```

Rules are resolved when a desktop is created and re-applied on every config
reload, so editing one takes effect on desktops that already exist — including
a layout you changed at runtime, which goes back to what the rule says.

`float = true` sets what new windows on that desktop *start* as; `window.float.toggle`
still works per window, so you can tile one thing on a floating desktop. It is
deliberately checked *after* `set_float_policy("never")` vetoes a window rule's
float, because a config that tiles aggressively and then carves out one floating
desktop means it. `monitor` is not advisory — windows already on the desktop are
moved to that display too, and switching to the desktop takes the focus there. A
pin naming a display that isn't there (unplugged, or `monitor = 2` on a one-head
machine) lapses to "wherever it opens" rather than tiling into nothing.

**Auto-launch (`app`).** The launch goes through the normal manage path, so any
`rule` you set for that app still applies. Switching away and back before the
window appears won't spawn a second copy; closing the app and returning re-opens
it. A failed launch (bad command) is retried on your next visit rather than
latched.

Note that `app` and `mshell.exec.startup` do **not** cancel out: at startup the
auto-launch check runs while a spawned app's window still doesn't exist, so the
desktop looks empty and *both* launches go through. Put an app in one or the
other — `spawn` for resident, desktop-less things (a launcher, a sync client),
a desktop rule's `app` for anything that belongs to a desktop.

**Drive desktops from one table (`go` / `move`).** Two submaps sharing one key
per desktop — `g` takes *you* there, `m` sends the focused *window* there — are
worth generating from a single declaration rather than writing three times:

```lua
local desktops = {
    { name = "term", key = "t" },                           -- no app: see the note below
    { name = "web",  key = "b", rule = { app = "firefox.exe" } },
    { name = "chat", key = "d", rule = { app = discord       } },  -- nil if not installed
    { name = "game", key = "v", rule = { app = valorant,
                                         float = true        } },
}

local go_keys, move_keys = {}, {}
for _, d in ipairs(desktops) do
    local name = d.name
    -- A closure is how an action is told WHICH desktop, and desc is how the
    -- which-key panel gets a label for it — a function has no name of its own.
    go_keys[d.key]   = { function() mshell.desktop.focus(name) end,
                         desc = name }
    move_keys[d.key] = { function() mshell.window.move.to_desktop(name) end,
                         desc = "→ " .. name }
    if d.rule then mshell.desktop.rule(d.name, d.rule) end
end
go_keys.Tab  = mshell.desktop.focus.last
go_keys["]"] = mshell.desktop.focus.next   -- step through whatever exists now
go_keys["["] = mshell.desktop.focus.prev

mshell.keys.submap("go",   go_keys)                   -- one-shot: pick and you're there
mshell.keys.submap("move", move_keys)
-- then, in the leader map:  g = "go", m = "move"
```

Adding a desktop is then one row: it gets both leader keys and its rule at once,
and the two maps can't drift apart. `app` may be `nil`, so a program you don't
have loses only its auto-launch — the desktop and its keys stay. The shipped
`config/init.lua` does exactly this, which is what makes `Win`-tap `g b` land on
a running browser. Note the table declares nothing: these are just the desktops
worth a key and a rule, and you can still switch to any other name at any time.

Apps whose launcher needs **arguments** can't be used directly: `spawn` and a
desktop rule's `app` both go through `ShellExecute` with no parameters. Discord
(`Update.exe --processStart Discord.exe`) and Valorant (Riot Client + Vanguard)
are the common cases — point at the Start-menu `.lnk` instead, which carries the
arguments itself.

**Fullscreen, in three flavours.** Two different things can go fullscreen and
mshell keeps them apart: the **window** (geometry, which mshell owns) and the
app's own **content** fullscreen — YouTube's fullscreen button, `F11` in a
browser, where the app switches its own UI and resizes itself to the display.
Each action is its own toggle, and pressing a different one switches modes
directly:

```lua
-- window fills the monitor / app fullscreen stays in the tile / both
mshell.keys.bind({mod, shft}, "f", mshell.window.fullscreen.window)
mshell.keys.bind({mod, ctrl}, "f", mshell.window.fullscreen.content)
mshell.keys.bind({mod, alt},  "f", mshell.window.fullscreen.both)
```

| Action | Window geometry | The app's own fullscreen |
|--------|-----------------|--------------------------|
| `window.fullscreen.window` | covers the monitor, edge to edge | untouched — the app is never told |
| `window.fullscreen.content` | pinned to its tile | renders *inside* the window |
| `window.fullscreen.both` | covers the monitor | left alone — it covers the display |

A fullscreen window leaves the layout: the others tile underneath it as if it
weren't there, so leaving fullscreen reveals the layout already in place. Only
one window per monitor can cover the screen — claiming it releases the previous
one — and its focus ring is suppressed (a colored line hugging the screen edges
is not what content asking for the whole display wants).

`window.fullscreen.content` has nothing to do until the app itself goes fullscreen:
mshell can't press YouTube's button for you. What it does is *pin* the window, so
when you do press it the fullscreen content fills the tile instead of escaping to
the display.

**What happens when an app fullscreens itself.** For windows you haven't given a
mode, `mshell.window.policy.fullscreen("contain" | "monitor")` decides.
`"contain"` (the default, and what mshell has always done) keeps the window in
its tile, so a fullscreen video fills the tile. `"monitor"` hands it the display
— detected from the window covering its monitor's full bounds — and puts it back
in the layout the moment it leaves fullscreen, so the fullscreen button behaves
the way it does outside a tiling WM. The per-window actions override the policy
either way.

**Jump back to the last desktop.** The `desktop.focus.last` action returns to the
desktop you switched away from. Every switch records where it came from, so the
two form a toggle — press it twice and you are back where you started:

```lua
mshell.keys.bind({mod}, "`", mshell.desktop.focus.last)  -- bounces between two
```

The default config also puts it on `Tab` inside the **desktop** submap (`Win+d`
then `Tab`). It is deliberately *not* on `Win+Tab`: Windows picks that combo up
below mshell's keyboard hook, so Task View would open on top of the switch —
bare keys inside a submap never reach the OS. Until you switch desktops at least
once the action does nothing (there is nowhere to go back to). It remembers a
*name*, not a desktop, so it still works when the desktop you came from was
destroyed behind you for being empty — it is simply re-created.

## Architecture

| File | Responsibility |
|------|----------------|
| `main.c` | bootstrap, elevation check, message loop, session window |
| `keyboard.c` | low-level keyboard hook, submap state machine, action dispatch |
| `window.c` | adoption tiers (tracked/full), manage/unmanage, decoration stripping, rules |
| `tiling.c` | 7 layout algorithms, per-monitor tiling, batched/frame-accurate placement |
| `desktop.c` | virtual desktops (show/hide), reconfigure, reapply |
| `events.c` | WinEvent hooks for window lifecycle tracking |
| `config.c` | atomic Lua config load/reload + built-in fallback keymap |
| `lua_api.c` | C functions exposed to the Lua config |
| `border.c` | focused-window ring overlay |
| `background.c` | solid-color desktop backdrop |

The keyboard hook only mutates state and **defers** heavy work to the message
pump (`PostMessage`), keeping it well under `LowLevelHooksTimeout`.

## Design decisions

### Desktops span every monitor

A desktop in mshell is a set of windows, and each monitor tiles that desktop's
windows independently. Switching desktops changes what is on **all** displays at
once.

The alternative — dwm's model, where every monitor owns its own tag set and
switching affects only the focused one — was considered and deliberately not
taken. Two reasons:

1. **A desktop here is a name you invent, not a slot you own.** Desktops are
   created by going to them and destroyed when you leave them empty, so "the
   tags on monitor 2" would be a second, differently-shaped namespace layered
   over a set that is already dynamic. `desktop.focus "web"` would have to mean
   something different depending on which monitor had focus.
2. **It matches how the desktops are actually used.** A desktop that *is* your
   browser, or your chat app, is a context you move between — and a context
   spanning your whole desk is usually what you want when you switch into it.

If you want a display to hold one thing permanently, pin a desktop to it with
`desktop_rule("name", { monitor = 1 })`; its windows tile there and switching to
it moves the focus there.

## Known limitations

- **Layout is per-desktop, not per-monitor.** All monitors on a desktop share
  the same layout / `nmaster` / master-ratio; there is no independent per-monitor
  layout state yet.
- **Owned/modal dialogs are tracked, not tiled**, unless `set_manage_owned(true)`.
  This is deliberate: forcing fixed-size dialogs into a tile can make them
  unusable. They still hide and show with their desktop like any other window.
- Directional focus/move uses window-rect centers; unusual custom layouts may
  not always match intuition (it falls back to cycling).

## License

MIT — see [LICENSE](LICENSE). Vendors Lua (also MIT).
