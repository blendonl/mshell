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
  `fullscreen` gives the window the whole monitor, `fullscreen_content` pins it
  so the app's fullscreen fills only its tile, and `fullscreen_both` lets the
  app's fullscreen cover the display. `set_fullscreen_policy` picks which of the
  last two an app that fullscreens itself gets by default.
- **Multi-monitor**: each display is tiled independently; move focus and windows
  across monitors (`Win+,` / `Win+.`).
- **Force-tiled mode** (`set_float_policy("never")`) so *every* window joins the
  grid and nothing is ever stacked on top of another window.
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
  optional **which-key hint** that lists the active submap's keys (configurable
  delay; `set_whichkey`).
- **Lua configuration** that reloads **when you save it** (or on `Win+Shift+R`)
  and is *atomic* — a broken config keeps the previous one instead of
  stranding you.
- **Status bar**, one per monitor: the live desktop set with the current one
  marked, the active layout, the focused window's title, and a clock. It
  reserves its strip from each monitor's work area, so tiled windows sit below
  it and a fullscreen window still covers it. Configurable via `set_bar`, and
  the module list can be trimmed or turned off entirely.
- **Focus ring** around the active window and a **solid desktop backdrop**
  (there is no Explorer to paint one).
- **Window rules** matching class, process or full install path as wildcard
  patterns — float, ignore, strip the frame, or park a window fullscreen over
  its monitor. One rule covers a whole game library; tiled windows can't be
  dragged loose (they snap back) and can't maximize out of the grid. A rule can
  also match **what a window is** rather than what it's called: `dialog = true`
  catches every file picker, message box and permission prompt, whichever app
  raised it, so the default config floats them all in one line.
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

`%TEMP%\mshell.log` (and DebugView) is always written, and is the first place to
look when something doesn't work: it records whether your config loaded, how many
bindings it produced, and any startup program that failed to launch. **If none of
your keybinds work, read it** — a config error is atomic, so a single bad line
rejects the whole file and leaves you on a six-binding fallback keymap. Add
`--verbose` for the full per-keystroke trace on top.

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
| `config/init.full.lua` | **The worked example.** Heavily commented: leader menus, per-desktop auto-launch, game rules, which-key styling, event handlers. Installed alongside as reference; copy it over your `init.lua` if you want the lot. |

A default that launched Alacritty, Firefox, Discord and Valorant would greet
most new users with a log full of launch failures, so it doesn't. Everything
interesting is one file away and documented.

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
| `Win+w` | **window** submap (one-shot; close/kill/float/fullscreen/all 7 layouts) |
| `Win+r` | **resize** submap (persisting; ratio + per-window `cfact`; `Esc` exits) |
| `Win+d` | **desktop** submap (persisting; cycle focus, `Tab` = last desktop) |
| `Win+o` | **launch** submap (one-shot; terminal, browser, files, launcher) |
| `Win+v` / `Win+Shift+v` | Go to / send window to the `game` desktop (Valorant) |
| `Win+Alt+f` | Fullscreen: **both** (the minimal config puts this on `Win+F11`) |
| `Win+Ctrl+i` | A Lua-function binding: logs the current desktop and window |

## Configuration

Your config lives at **`%APPDATA%\mshell\init.lua`**
(`C:\Users\<you>\AppData\Roaming\mshell\init.lua`) — the standard Windows
location for per-user config, so a reinstall never touches it. If that file is
absent, mshell falls back to `config\init.lua` beside `mshell.exe`, which keeps
a portable/unzipped copy working.

See [`config/init.lua`](config/init.lua) for the full commented example.
Highlights:

```lua
mshell.set_gaps(6, 6)              -- inner gap, outer gap
mshell.set_smart_gaps(true)       -- no gaps when a monitor has one window
mshell.set_border(2, 0xffffff)    -- focus ring: width, 0xRRGGBB
mshell.set_background(0x000000)   -- desktop backdrop
mshell.set_start_desktop("1")     -- the desktop you land on (the only one at boot)
mshell.desktop_rule("web", { app = "firefox.exe" })   -- open it when empty
mshell.desktop_rule("game", { float = true, app = "steam.exe" })
mshell.desktop_rule("chat", { layout = "monocle", monitor = 1 })

mshell.set_layout("tiling")       -- tiling|monocle|grid|spiral|centered|bstack|columns
mshell.set_nmaster(1)             -- windows in the master area
mshell.set_float_policy("never")  -- force EVERY window into the grid
mshell.set_attach("master")       -- new windows become master (dwm-style)

mshell.bind({"LWin"}, "h", "focus_left")
mshell.bind({"LWin", "Shift"}, "Return", "spawn", "alacritty.exe")

-- submap values can be a bare action or {"action", arg} for a payload.
-- persist=false (default) is one-shot: the next key drops back to root.
mshell.submap("launch", {
    Return = {"spawn", "alacritty.exe"},
    b      = {"spawn", "firefox.exe"},
})
mshell.bind({"LWin"}, "o", "enter_submap", "launch")

-- persist=true stays active until its exit key (Esc, or a custom `exit`).
mshell.submap("resize", {
    h = "dec_master", l = "inc_master",
}, { persist = true })            -- , exit = "q"  -- replaces Esc

-- set_leader makes a bare Win TAP enter a submap; from there bare keys reach
-- everything (no Win held). Tap Win / Esc to leave. The name is your choice.
mshell.submap("normal", {
    h = "focus_left", j = "focus_down", k = "focus_up", l = "focus_right",
    w = {"enter_submap", "window"},   -- define "window" before this map
    o = {"enter_submap", "launch"},
}, { persist = true })
mshell.set_leader("normal")           -- Win tap now enters the map above

mshell.rule({ process = "Taskmgr.exe" }, "float")

-- Games: never tiled, borderless, covering the monitor, no ring over it.
-- `path` matches the full install path, so one rule covers a whole library
-- (every Steam game, on every drive) instead of one line per executable.
mshell.rule({ path = [[*\steamapps\common\*]] }, "float",
            { ring = false, decorate = false, fullscreen = true })
```

API: `bind`, `submap`, `set_leader`, `rule`, `spawn`, `set_gaps`,
`set_smart_gaps`, `set_border`, `set_background`, `set_whichkey`,
`set_start_desktop`, `desktop_rule`, `set_master_ratio`, `set_nmaster`, `set_layout`,
`set_float_policy`, `set_fullscreen_policy`, `set_attach`, `set_manage_owned`,
`set_float_on_top`,
`set_min_window_size`, `set_auto_reload`, `set_verbose`, `block_system_keys`,
`log`.

**Auto-reload.** mshell watches the folder holding your `init.lua` and reloads
250 ms after the last write, so saving in your editor applies the config —
`Win+Shift+R` is still there for an explicit reload. The debounce means a
save-in-progress isn't read half-written, and the atomic-reload rollback means
a syntax error leaves the running config untouched (the error goes to the log).
Extra modules you keep beside `init.lua` and `require()` count too. Turn it off
with `mshell.set_auto_reload(false)` if you'd rather reload by hand.

**Window rules & games.** `mshell.rule(match, action, opts)` matches on `class`,
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
mshell.rule({ path = [[*\steamapps\common\*]] }, "float",
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
mshell.rule({ dialog = true }, "float")                     -- every picker/prompt
mshell.rule({ process = "code.exe", dialog = true }, "manage")  -- except this app's
```

It is also the only match that pulls in *owned* windows, which mshell otherwise
leaves untouched entirely. Untouched sounds like floating and mostly behaves
like it, but such a window is invisible to the WM: it stays on screen when you
switch desktops and no binding reaches it. Matched by a `dialog` rule it becomes
a real floating window — hidden with its desktop, focusable, closable. The
shipped config uses that one-liner plus named rules for `consent.exe` (UAC, only
visible if the secure desktop is disabled) and `CredentialUIBroker.exe` (the
"Windows Security" PIN/password box), which are separate processes rather than
dialogs of the app that triggered them. `dialog = false` is the inverse, for a
rule that should skip dialogs.

Naming a window in a rule also rescues it from the "caption-less popup =
menu/tooltip" filter — the exact style a borderless-fullscreen game uses — so
the game stays a real managed window: it hides on desktop switch and
`Win+Shift+c` can close it. Because games rebuild their window when the
graphics device comes up (and again whenever you change resolution or flip
windowed/borderless in their options), the frame and the fullscreen geometry are
re-asserted whenever the window moves, not just once when it opens.

**Force every window to tile.** Set `mshell.set_float_policy("never")`: any
`"float"` rule is downgraded to `"manage"` and `Win+f` becomes a no-op, so
nothing is ever stacked on top of a tiled window. (Owned/modal dialogs are
still left alone unless you also set `set_manage_owned(true)`, which is
aggressive — many dialogs are fixed-size and tile poorly.) When windows *do*
float, `set_float_on_top(true)` keeps them above the tiled grid.

**Desktops are dynamic, and a desktop is its name.** There is no desktop count
to configure and no `1..9`: a desktop is a *name* — a word (`"web"`) or a number
(`"1"`), with no difference between them — and it exists only while something is
on it. Switching to a name nothing is using **creates** that desktop; leaving one
with no windows on it **destroys** it. At startup exactly one desktop exists, the
one you land on:

```lua
mshell.set_start_desktop("term")   -- default "1"

mshell.bind({mod}, "w", "switch_desktop",  "web")   -- creates "web" on demand
mshell.bind({mod, shft}, "w", "move_to_desktop", "web")
mshell.bind({mod}, "3", "switch_desktop",  "3")     -- "3" is a name, not an index
```

Names are case-insensitive (the first spelling to create the desktop is the one
displayed), can't contain whitespace, and are capped at 63 characters. Because a
name never has to be declared, `switch_desktop "scratch"` always works whether or
not `scratch` appears anywhere in your config — you can invent a desktop at any
time and it costs nothing once you close its last window.

Two consequences worth knowing:

- The desktop you are *standing on* is never destroyed, however empty it is —
  closing everything in front of you leaves you somewhere, not nowhere.
- `last_desktop` remembers a **name**, so it goes back to a desktop that was
  destroyed behind you by re-creating it (empty, with its rules applied).

Since the set changes under you, `next_desktop` / `prev_desktop` step through
whatever exists at that moment, in name order — numbers first and numerically
(`1, 2, 10`), then words alphabetically. That's how you get back to a desktop you
made on the fly and never bound a key to.

**Desktop rules — what a desktop does.** `mshell.desktop_rule(pattern, opts)` is
the desktop counterpart of `mshell.rule`. `pattern` is a desktop name or a
case-insensitive wildcard over names, matched with the same `*`/`?` syntax window
rules use:

```lua
mshell.desktop_rule("web",    { app = "firefox.exe" })
mshell.desktop_rule("chat",   { app = discord, layout = "monocle" })
mshell.desktop_rule("game-*", { float = true, monitor = 1 })
```

| field | effect |
|---|---|
| `app` | open this whenever you enter the desktop and it has no windows |
| `float` | windows opened here start floating instead of tiled |
| `layout` | this desktop's layout, overriding `set_layout` |
| `master_ratio` | master area size for this desktop, `0.2 .. 0.9` |
| `nmaster` | windows in this desktop's master area |
| `monitor` | pin the desktop to a display (0-based) |

Rules **layer** rather than compete: every rule whose pattern matches is applied
in declaration order, and each overrides only the fields it names. So a `"*"`
rule sets the house style and a specific one adjusts a field or two:

```lua
mshell.desktop_rule("*",       { layout = "tiling" })
mshell.desktop_rule("scratch", { float  = true     })   -- still layout = tiling
```

Rules are resolved when a desktop is created and re-applied on every config
reload, so editing one takes effect on desktops that already exist — including
a layout you changed at runtime, which goes back to what the rule says.

`float = true` sets what new windows on that desktop *start* as; `toggle_float`
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

Note that `app` and `mshell.spawn` do **not** cancel out: at startup the
auto-launch check runs while a spawned app's window still doesn't exist, so the
desktop looks empty and *both* launches go through. Put an app in one or the
other — `spawn` for resident, desktop-less things (a launcher, a sync client),
a desktop rule's `app` for anything that belongs to a desktop.

**Drive desktops from one table (`go` / `move`).** Two submaps sharing one key
per desktop — `g` takes *you* there, `m` sends the focused *window* there — are
worth generating from a single declaration rather than writing three times:

```lua
local desktops = {
    { name = "term", key = "t" },                                  -- no app: see spawn note
    { name = "web",  key = "b", rule = { app = "firefox.exe" } },
    { name = "chat", key = "d", rule = { app = discord       } },  -- nil if not installed
    { name = "game", key = "v", rule = { app = valorant,
                                         float = true        } },
}

local go_keys, move_keys = {}, {}
for _, d in ipairs(desktops) do
    go_keys[d.key]   = {"switch_desktop",  d.name}
    move_keys[d.key] = {"move_to_desktop", d.name}
    if d.rule then mshell.desktop_rule(d.name, d.rule) end
end
go_keys.Tab  = "last_desktop"
go_keys["]"] = "next_desktop"     -- step through whatever exists right now
go_keys["["] = "prev_desktop"

mshell.submap("go",   go_keys)                   -- one-shot: pick and you're there
mshell.submap("move", move_keys)
-- then, in the leader map:  g = {"enter_submap", "go"}, m = {"enter_submap", "move"}
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
mshell.bind({mod, shft}, "f", "fullscreen")           -- window fills the monitor
mshell.bind({mod, ctrl}, "f", "fullscreen_content")   -- app fullscreen stays in the tile
mshell.bind({mod, alt},  "f", "fullscreen_both")      -- app fullscreen fills the monitor
```

| Action | Window geometry | The app's own fullscreen |
|--------|-----------------|--------------------------|
| `fullscreen` | covers the monitor, edge to edge | untouched — the app is never told |
| `fullscreen_content` | pinned to its tile | renders *inside* the window |
| `fullscreen_both` | covers the monitor | left alone — it covers the display |

A fullscreen window leaves the layout: the others tile underneath it as if it
weren't there, so leaving fullscreen reveals the layout already in place. Only
one window per monitor can cover the screen — claiming it releases the previous
one — and its focus ring is suppressed (a colored line hugging the screen edges
is not what content asking for the whole display wants).

`fullscreen_content` has nothing to do until the app itself goes fullscreen:
mshell can't press YouTube's button for you. What it does is *pin* the window, so
when you do press it the fullscreen content fills the tile instead of escaping to
the display.

**What happens when an app fullscreens itself.** For windows you haven't given a
mode, `mshell.set_fullscreen_policy("contain" | "monitor")` decides.
`"contain"` (the default, and what mshell has always done) keeps the window in
its tile, so a fullscreen video fills the tile. `"monitor"` hands it the display
— detected from the window covering its monitor's full bounds — and puts it back
in the layout the moment it leaves fullscreen, so the fullscreen button behaves
the way it does outside a tiling WM. The per-window actions override the policy
either way.

**Jump back to the last desktop.** The `last_desktop` action returns to the
desktop you switched away from. Every switch records where it came from, so the
two form a toggle — press it twice and you are back where you started:

```lua
mshell.bind({mod}, "`", "last_desktop")   -- Win+` bounces between two desktops
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
| `window.c` | manageability filter, manage/unmanage, decoration stripping, rules |
| `tiling.c` | 7 layout algorithms, per-monitor tiling, batched/frame-accurate placement |
| `desktop.c` | virtual desktops (show/hide), reconfigure, reapply |
| `events.c` | WinEvent hooks for window lifecycle tracking |
| `config.c` | atomic Lua config load/reload + built-in fallback keymap |
| `lua_api.c` | C functions exposed to the Lua config |
| `border.c` | focused-window ring overlay |
| `background.c` | solid-color desktop backdrop |

The keyboard hook only mutates state and **defers** heavy work to the message
pump (`PostMessage`), keeping it well under `LowLevelHooksTimeout`.

## Known limitations

- **Layout is per-desktop, not per-monitor.** All monitors on a desktop share
  the same layout / `nmaster` / master-ratio; there is no independent per-monitor
  layout state yet.
- **Owned/modal dialogs stay floating** unless `set_manage_owned(true)`. This is
  deliberate: forcing fixed-size dialogs into a tile can make them unusable.
- Directional focus/move uses window-rect centers; unusual custom layouts may
  not always match intuition (it falls back to cycling).

## License

MIT — see [LICENSE](LICENSE). Vendors Lua (also MIT).
