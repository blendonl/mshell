--[[
    mshell configuration.

    Loaded from  %APPDATA%\mshell\init.lua
    (C:\Users\<you>\AppData\Roaming\mshell\init.lua — install.bat puts a copy
    of this file there). If that file is missing, mshell falls back to
    config\init.lua next to mshell.exe, so an unzipped portable copy still
    works. Saving this file reloads it automatically (see set_auto_reload
    below); Win+Shift+R reloads on demand. Either way a syntax error keeps the
    previous config, it won't strand you.

    Two ways to drive mshell:

      1. Win+key chords     — hold Win and press a key (Win+h, Win+Shift+Return…).
                              Instant, exactly as before.
      2. The leader mode    — TAP Win on its own (press and release, nothing held)
                              to enter the sub-map named by mshell.set_leader
                              (below we point it at "normal"). From there bare
                              keys do everything: h/j/k/l focus, and w/r/d/o open
                              the sub-maps. Tap Win again (or Esc) to leave.

    Sub-maps come in two flavours:
      persist = false (default, "one-shot") — the next key runs its action (or is
                     swallowed) and you drop straight back to root.
      persist = true  ("persisting")        — you stay in the map and can fire key
                     after key; leave with Esc, a custom `exit` key, or a Win tap.

    Modifier key:   LWin  (swallowed by mshell; every Win+* combo is ours)
    Vim navigation: hjkl
    Leader (Win tap): normal → w window · r resize · d desktop · o launch
                               g go     · m move

    "go" and "move" are the two desktop maps, and they share one key per
    desktop: `g` takes YOU there, `m` sends the focused WINDOW there. Both are
    generated from the single `desktops` table below, so leader-g-b lands on the
    browser desktop, leader-m-b throws the current window at it, and adding a
    row gives you both keys plus the desktop's rule at once.

    Desktops are DYNAMIC and identified by NAME. There is no count to set and no
    1..9: a desktop is a name — a word ("web") or a number ("1"), no difference
    — and it exists only while something is on it. Switching to a name nothing
    is using creates that desktop; leaving one with no windows destroys it. So
    the `desktops` table below is not a declaration of what exists, it's a list
    of the ones you've given a key and a rule to. You can always switch to a
    name you never listed and get an empty desktop for it.

    What a desktop DOES — which app opens on it, whether its windows float, its
    layout, which monitor it lives on — is a mshell.desktop_rule.
--]]

local mod  = "LWin"
local shft = "Shift"
local ctrl = "Ctrl"
local alt  = "Alt"

-- Build "%VAR%" .. suffix, or return nil if VAR is not set in mshell's
-- environment.
--
-- Use this for EVERY os.getenv path. A bare `os.getenv("X") .. "..."` raises
-- "attempt to concatenate a nil value" the moment X is missing, and because a
-- config error is atomic that throws away the WHOLE file — every keybind and
-- every startup program with it, not just the line that failed. mshell then
-- falls back to its six-binding built-in keymap, which presents as "none of my
-- keybinds work and my terminal didn't start". Returning nil instead lets the
-- one feature that needed the variable drop out on its own.
local function envpath(var, suffix)
    local base = os.getenv(var)
    if not base or base == "" then
        mshell.log(var .. " is not set — skipping the paths that depend on it")
        return nil
    end
    return base .. suffix
end

-- Full path to Flow Launcher. Its Squirrel installer drops it under
-- %LOCALAPPDATA%, which is NOT on PATH, so spawn the full path, not the bare
-- name. The top-level Flow.Launcher.exe is a stub that forwards to the current
-- app-<version>\ build, so this keeps working across updates. Used twice below:
-- started resident at startup, and re-launched from the launch submap (Win+o a)
-- to pop its search box (Flow is single-instance — a second launch signals the
-- running one to show). nil when Flow isn't locatable — both uses are guarded.
local flow = envpath("LOCALAPPDATA", [[\FlowLauncher\Flow.Launcher.exe]])

-- Return the first of these paths that actually exists, or nil if none do.
--
-- Apps that install per-user vs. machine-wide (and installers that rename their
-- Start-menu folder between versions) leave the same program under two or three
-- plausible paths. Probing beats guessing: an app you don't have simply yields
-- nil, and the feature that needed it drops out on its own — exactly like
-- envpath above. nil entries are skipped, so envpath() results can be passed
-- straight in.
local function first_existing(...)
    for _, path in ipairs({...}) do
        if path then
            local f = io.open(path, "r")
            if f then f:close(); return path end
        end
    end
    return nil
end

-- Discord. Its Squirrel installer puts the real binary in a versioned
-- app-<version>\ folder, and the top-level Update.exe only starts it when
-- passed `--processStart Discord.exe`. Now that spawn and a desktop rule's
-- `app` both take arguments, point at Update.exe and pass them — which also
-- survives updates, since Update.exe picks the current app- folder itself.
--
-- The Start-menu .lnk still works and is the fallback: before arguments
-- existed it was the only option, because a shortcut carries its own.
local discord_update = envpath("LOCALAPPDATA", [[\Discord\Update.exe]])
local discord, discord_args
if discord_update and first_existing(discord_update) then
    discord, discord_args = discord_update, "--processStart Discord.exe"
else
    discord = first_existing(
        envpath("APPDATA",      [[\Microsoft\Windows\Start Menu\Programs\Discord Inc\Discord.lnk]]),
        envpath("ProgramData",  [[\Microsoft\Windows\Start Menu\Programs\Discord Inc\Discord.lnk]]))
end

-- Valorant, for the same reason: it can't be launched as a bare exe (Vanguard +
-- Riot Client), and the game binary moves between patches, so use the shortcut
-- the installer leaves in the Start menu. Adjust if yours lives elsewhere (e.g.
-- the Desktop copy under %USERPROFILE%\Desktop\VALORANT.lnk).
local valorant = first_existing(
    envpath("ProgramData",  [[\Microsoft\Windows\Start Menu\Programs\Riot Games\VALORANT.lnk]]),
    envpath("USERPROFILE",  [[\Desktop\VALORANT.lnk]]))

----------------------------------------------------------------------
-- Appearance
----------------------------------------------------------------------
mshell.set_gaps(6, 6)               -- inner gap (between windows), outer gap (screen edge)
-- mshell.set_smart_gaps(true)      -- drop gaps when a monitor has a single window
mshell.set_border(2, 0xffffff)      -- focused-window ring (width, 0xRRGGBB)
mshell.set_background(0x000000)     -- solid desktop backdrop color
mshell.set_master_ratio(0.60)

-- Status bar — one per monitor, along the top by default. It reserves its
-- strip out of each monitor's work area, so tiled windows sit below it and a
-- fullscreen window still covers it.
--
-- `modules` REPLACES the default set rather than adding to it, so listing only
-- some of them turns the rest off: modules = {"desktops"} gives a bar with
-- nothing but the desktop list.
mshell.set_bar({
    enabled  = true,
    position = "top",        -- or "bottom"
    height   = 28,           -- design pixels at 96 DPI; scaled per monitor
    bg       = 0x1e1e2e,
    fg       = 0xcdd6f4,
    accent   = 0x7aa2f7,     -- the current desktop
    dim      = 0x6c7086,     -- the other desktops
    modules  = { "desktops", "layout", "title", "clock" },
})

-- Submap hint ("which-key"): when you enter a submap (Win+r, Win+x, …) a small
-- panel lists that submap's keys and what they do. On by default; this call
-- just shows the knobs (values below are the defaults).
mshell.set_whichkey({
    enabled = true,
    delay   = 150,          -- ms before it appears; 0 = show instantly
    bg      = 0x1e1e2e,     -- panel background
    fg      = 0xcdd6f4,     -- action-label text
    key_fg  = 0x7aa2f7,     -- key + header accent
    border  = 0x7aa2f7,     -- panel outline
})

-- How much goes to %LOCALAPPDATA%\mshell\mshell.log (and DebugView). The file
-- is appended to and rotates at 5 MB, so raising this is not a commitment.
-- mshell.set_log_level("debug")    -- error | warn | info (default) | debug | trace
-- mshell.set_verbose(true)         -- the older spelling of set_log_level("debug")

----------------------------------------------------------------------
-- Auto-reload
----------------------------------------------------------------------
-- mshell watches this file's folder and reloads a quarter-second after the
-- last write, so saving in your editor applies the config. Rollback still
-- applies: a half-saved or broken file leaves the running config untouched.
-- mshell.set_auto_reload(false)    -- reload only on Win+Shift+R

----------------------------------------------------------------------
-- Tiling policy
----------------------------------------------------------------------
mshell.set_layout("tiling")         -- default layout: tiling|monocle|grid|spiral|centered|bstack|columns
mshell.set_nmaster(1)               -- windows in the master area
mshell.set_attach("end")            -- where new windows land: end|master|after

-- Force EVERY window into the grid — "float" rules are downgraded to "manage"
-- and Win+f becomes a no-op, so nothing is ever stacked on top of a tiled
-- window. Uncomment to guarantee a fully-tiled desktop:
-- mshell.set_float_policy("never")

-- Floating windows stay above the tiled grid, through focus changes too. Turn
-- this off to make a float an ordinary window in the stack, which sinks behind
-- whatever you focus next:
-- mshell.set_float_on_top(false)

-- Also tile owned/dialog windows (aggressive — modal dialogs tile poorly):
-- mshell.set_manage_owned(true)

-- Ignore windows smaller than this (default 120x80):
-- mshell.set_min_window_size(120, 80)

----------------------------------------------------------------------
-- Fullscreen
----------------------------------------------------------------------
-- Two different things can go fullscreen and mshell keeps them apart: the
-- WINDOW (geometry, which mshell owns) and the app's own CONTENT fullscreen
-- (YouTube's fullscreen button, F11 in a browser — the app switches its own UI
-- and resizes itself to the display). Three keybindable actions, each its own
-- toggle; pressing a different one switches modes directly:
--
--   fullscreen          the window covers its monitor, edge to edge. The app is
--                       never told anything — its ordinary UI is just as big as
--                       the screen. For apps with no fullscreen mode of their own.
--   fullscreen_content  the window stays pinned to its tile, so when the app
--                       fullscreens itself the video fills THE TILE, not the
--                       screen — fullscreen "inside" the window.
--   fullscreen_both     the window covers the monitor AND mshell stops policing
--                       its geometry, so the app's own fullscreen covers the
--                       display, exactly as it would outside a tiling WM.
--
-- This policy decides what an app that fullscreens itself gets when you haven't
-- given its window a mode. "contain" (the default) keeps it in its tile;
-- "monitor" hands it the display and puts it back in the layout the moment it
-- leaves fullscreen — so YouTube's button behaves the way it does everywhere else.
-- mshell.set_fullscreen_policy("monitor")

----------------------------------------------------------------------
-- Desktops (dynamic, addressed by name)
----------------------------------------------------------------------
-- There is nothing to declare. A desktop is a NAME, it comes into existence the
-- moment you switch to it, and it disappears when you leave it with no windows
-- on it. "1" and "web" are both just names — a number is not an index into
-- anything, which is why Win+1 goes to a desktop literally called "1".
--
--   mshell.set_start_desktop("1")   -- the one you land on at startup (default)
--
-- So `switch_desktop "scratch"` always works, whether or not "scratch" appears
-- anywhere in this file. What this section configures is the desktops you want
-- a KEY and a RULE for.
-- ---------------------------------------------------------------------
-- The desktop table — the desktops worth naming.
-- ---------------------------------------------------------------------
-- Each row gets you two things at once:
--
--   name   the desktop's name, as switch_desktop/move_to_desktop take it.
--   key    one bare key that addresses this desktop from BOTH desktop submaps:
--          leader g <key> switches to it, leader m <key> sends the focused
--          window to it. Defined once, so the two maps can never drift apart.
--   rule   optional table passed straight to mshell.desktop_rule — see the
--          rules block under the table for what goes in it.
--
-- To add a desktop: add a row (or don't — switch to the name and it exists).
-- A row whose app path could not be resolved (not installed, or the env var is
-- missing) quietly loses just its auto-launch; the desktop and its keys stay.
local desktops = {
    -- No `app` on purpose: the terminal is started by mshell.spawn at the
    -- bottom of this file instead. Setting both would open TWO terminals at
    -- startup, not one — see the note in the Startup section for why they don't
    -- cancel out. To flip it the other way, add `app = "alacritty.exe"` to the
    -- rule here and delete the spawn; you'd then also get a fresh terminal
    -- whenever you close the last one and come back, which the spawn can't do.
    { name = "term",  key = "t" },                                  -- leader g t
    { name = "web",   key = "b", rule = { app = "firefox.exe"  } }, -- leader g b
    -- `app` takes a bare command, or {command, arguments} when the program
    -- needs them — Discord's launcher does. Written as a table only when there
    -- are arguments to pass, so both shapes are visible in one file.
    { name = "chat",  key = "d", rule = { app = discord_args
                                                and { discord, discord_args }
                                                or  discord,
                                          layout = "monocle"   } }, -- leader g d
    -- The game desktop floats: Valorant is borderless-fullscreen and has no
    -- business in a tiling grid, and anything else you open here (a launcher, a
    -- second game) gets the same treatment without needing its own window rule.
    { name = "game",  key = "v", rule = { app = valorant,
                                          float = true         } }, -- leader g v
    { name = "files", key = "e", rule = { app = "explorer.exe" } }, -- leader g e
    { name = "media", key = "u" },
    { name = "notes", key = "n" },
    { name = "misc",  key = "x" },
    { name = "tmp",   key = "z" },
}

-- ---------------------------------------------------------------------
-- Desktop rules — what a desktop DOES
-- ---------------------------------------------------------------------
-- mshell.desktop_rule(pattern, opts). `pattern` is a desktop name, or a
-- wildcard over names ("game-*", "*"). Every field is optional:
--
--   app          = "firefox.exe"  open this whenever you enter the desktop and
--                                 it's empty — a desktop that IS your browser
--                                 rather than one you have to remember to put a
--                                 browser on. Close it, come back, it reopens.
--   float        = true           windows opened here start floating instead of
--                                 tiled. toggle_float still works per window.
--   layout       = "monocle"      this desktop's layout, overriding set_layout.
--   master_ratio = 0.6            master area size for this desktop, 0.2 .. 0.9
--   nmaster      = 1              windows in this desktop's master area
--   monitor      = 1              pin the desktop to a display (0-based): its
--                                 windows tile there and switching to it takes
--                                 the focus there.
--
-- Rules LAYER. Every rule whose pattern matches is applied in the order written,
-- and each overrides only the fields it names — so a "*" rule sets the house
-- style and a specific one adjusts a field or two:
--
--   mshell.desktop_rule("*",       { layout = "tiling"  })
--   mshell.desktop_rule("scratch", { float  = true      })
--
-- They're re-applied on every reload, so editing one takes effect on desktops
-- that already exist.
for _, d in ipairs(desktops) do
    if d.rule then mshell.desktop_rule(d.name, d.rule) end
end

-- The desktop you land on at startup. It's the only one that exists then.
mshell.set_start_desktop("term")

-- Expand the table into the key tables for the two desktop submaps. The submaps
-- themselves are built further down (they must be declared next to the other
-- submaps, before "normal" refers to them) — these tables just carry the keys
-- there.
local go_keys, move_keys = {}, {}

for _, d in ipairs(desktops) do
    -- A key used twice would leave the earlier desktop unreachable from the
    -- submaps, and a silently shadowed keybind is miserable to debug. Say so
    -- and carry on: a warning in the log beats rejecting the whole config.
    if go_keys[d.key] then
        mshell.log("desktops: key '" .. d.key .. "' is bound twice — '" ..
                   d.name .. "' shadows the earlier desktop in the go/move maps")
    end

    go_keys[d.key]   = {"switch_desktop",  d.name}
    move_keys[d.key] = {"move_to_desktop", d.name}
end

-- The numbered desktops work in both maps too (leader g 3 goes to the desktop
-- called "3"), matching the Win+1..9 bindings further down. These are ordinary
-- desktops that happen to have digits for names; none of them exists until you
-- go there. Digits can't collide with the keys above unless you give a desktop
-- a digit for a key.
for i = 1, 9 do
    go_keys[tostring(i)]   = {"switch_desktop",  tostring(i)}
    move_keys[tostring(i)] = {"move_to_desktop", tostring(i)}
end
go_keys.Tab = "last_desktop"    -- leader g Tab — bounce to where you came from

-- [ and ] step through the desktops that exist RIGHT NOW, in name order
-- (numbers first, then words). This is how you get back to a desktop you
-- created on the fly and never gave a key to. Deliberately not n/p — `n` is the
-- notes desktop's key, and a desktop key must win in the map that is for
-- desktop keys.
go_keys["]"] = "next_desktop"
go_keys["["] = "prev_desktop"

----------------------------------------------------------------------
-- Submaps
----------------------------------------------------------------------

-- NOTE: sub-maps must be defined before anything that enters them (the "normal"
-- leader below refers to all four), so declare the leaf maps first.

-- Resize sub-map (persisting — stays active so you can nudge repeatedly; Esc
-- leaves). h/l adjust the master area; j/k grow/shrink the focused window.
-- To leave with a different key instead of Esc, add e.g. `exit = "q"` to the
-- opts table — a custom exit key replaces Esc.
mshell.submap("resize", {
    h     = "dec_master",
    l     = "inc_master",
    j     = "dec_cfact",
    k     = "inc_cfact",
    ["0"] = "reset_cfact",
}, { persist = true })

-- Window management sub-map (one-shot — one action, then back to root)
mshell.submap("window", {
    c     = "close",
    k     = "kill",
    f     = "toggle_float",
    Space = "promote_master",
    -- fullscreen (see the Fullscreen section above): w = the WINDOW fills the
    -- monitor, i = the app's own fullscreen stays INSIDE the window, a = both
    w     = "fullscreen",
    i     = "fullscreen_content",
    a     = "fullscreen_both",
    -- layouts
    t     = "layout_tiling",
    m     = "layout_monocle",
    g     = "layout_grid",
    s     = "layout_spiral",
    e     = "layout_centered",
    b     = "layout_bstack",
    n     = "layout_columns",
})

-- Desktop sub-map (persisting — cycle focus within the current desktop and stay
-- put between presses; Esc leaves). Tab jumps back to the desktop you came from
-- and, since it swaps the pair every time, pressing it again returns you here.
mshell.submap("desktop", {
    h   = "focus_prev",
    l   = "focus_next",
    Tab = "last_desktop",
}, { persist = true })

-- Launch sub-map (one-shot). Shows the richer {"action", arg} binding form, so
-- sub-maps can spawn programs (and switch desktops, enter other sub-maps, …).
-- Built as a table so the Flow Launcher key can be left out entirely when Flow
-- isn't locatable: {"spawn", nil} would fail the config load ("spawn requires a
-- command string") and take every other binding down with it.
-- A spawn payload is either a bare command or {command, arguments}:
--     Return = {"spawn", "alacritty.exe"}
--     t      = {"spawn", {"wt.exe", "-p Ubuntu"}}
-- Arguments are a separate string because that is how Windows takes them;
-- packing them into the command would be ambiguous for any path with a space.
local launch_keys = {
    Return = {"spawn", "alacritty.exe"},
    b      = {"spawn", "firefox.exe"},
    e      = {"spawn", "explorer.exe"},   -- file manager on demand
    t      = {"spawn", {"wt.exe", "-p Ubuntu"}},
}
if flow then
    launch_keys.a = {"spawn", flow}       -- Flow Launcher (pops its search box)
end
mshell.submap("launch", launch_keys)

-- The two desktop maps, both built from the `desktops` table in the Desktops
-- section above — same keys in each, so `g` and `m` differ only in what moves:
--
--   go    (leader g)  YOU go to that desktop        — g b browser, g t terminal,
--                                                     g v Valorant, g d Discord
--   move  (leader m)  the focused WINDOW goes there, and you stay put
--
-- One-shot, deliberately: picking a destination is a single decision, so the
-- next key runs and drops you straight back to root — leader g b is three keys
-- and you're on the browser desktop, no Esc needed. (Make either persisting
-- with { persist = true } if you'd rather stay and fire several in a row.)
-- Both maps also take 1..9 for the numbered desktops, and `go` takes [ and ] to
-- step through whatever exists at that moment.
mshell.submap("go",   go_keys)
mshell.submap("move", move_keys)

-- The leader map. It's an ordinary sub-map — the name is our choice; what makes
-- it the leader is the mshell.set_leader call below. It's persisting, so you
-- stay in it and can fire several keys in a row. Every sub-map above is reachable
-- from here without touching Win: w → window, r → resize, d → desktop, o → launch,
-- g → go (jump to a desktop), m → move (send the focused window to one).
mshell.submap("normal", {
    -- focus navigation (stay in normal — press h/j/k/l as many times as you like)
    h = "focus_left",
    j = "focus_down",
    k = "focus_up",
    l = "focus_right",
    -- open a sub-map
    w = {"enter_submap", "window"},
    r = {"enter_submap", "resize"},
    d = {"enter_submap", "desktop"},
    o = {"enter_submap", "launch"},
    g = {"enter_submap", "go"},       -- go to a desktop      (g b, g t, g v, …)
    m = {"enter_submap", "move"},     -- send a window to one  (m b, m t, m v, …)
    -- quick one-key actions
    Return = "promote_master",
    Space  = "cycle_layout",
    f      = "toggle_float",
}, { persist = true })

-- Make a bare Win tap (press + release, nothing held) enter the map above; tap
-- Win again — or press Esc — to leave. Point this at any sub-map you like, or
-- drop the line entirely to leave a Win tap doing nothing.
mshell.set_leader("normal")

----------------------------------------------------------------------
-- Root keybindings  (LWin + key)
----------------------------------------------------------------------

-- --- focus navigation (directional, vim keys) ---
mshell.bind({mod}, "h", "focus_left")
mshell.bind({mod}, "j", "focus_down")
mshell.bind({mod}, "k", "focus_up")
mshell.bind({mod}, "l", "focus_right")

-- --- window swap ---
mshell.bind({mod, shft}, "h", "move_left")
mshell.bind({mod, shft}, "j", "move_down")
mshell.bind({mod, shft}, "k", "move_up")
mshell.bind({mod, shft}, "l", "move_right")

-- --- desktop switching (Win+1 through Win+9) ---
-- Win+3 goes to the desktop NAMED "3". It doesn't have to exist — pressing the
-- key is what brings it into being, and it's gone again once you leave it
-- empty, so these nine cost nothing until you use them.
for i = 1, 9 do
    mshell.bind({mod}, tostring(i), "switch_desktop", tostring(i))
    mshell.bind({mod, shft}, tostring(i), "move_to_desktop", tostring(i))
end

-- --- step through whatever exists: Win+] / Win+[ ---
mshell.bind({mod}, "]", "next_desktop")
mshell.bind({mod}, "[", "prev_desktop")

-- --- back to the last desktop: Win+` returns to the desktop you switched away
-- from, and because every switch records where it came from, pressing it twice
-- lands you back where you started. Deliberately NOT on Win+Tab: Windows picks
-- that combo up below our keyboard hook, so Task View would open on top of the
-- switch. The same action sits on Tab inside the "desktop" submap (Win+d then
-- Tab), where bare keys never reach the OS. ---
mshell.bind({mod}, "`", "last_desktop")

-- --- game desktop shortcut: Win+v jumps to the "game" desktop (Valorant
-- auto-launches there when empty and everything on it floats; see its rule in
-- the desktops table above), Win+Shift+v sends the focused window there. The
-- leader reaches it as `g v` / `m v` — this is just the Win-chord alias. ---
mshell.bind({mod}, "v", "switch_desktop", "game")
mshell.bind({mod, shft}, "v", "move_to_desktop", "game")

-- --- monitors (Win+,/. focus; Win+Shift+,/. send) ---
mshell.bind({mod}, ",", "focus_monitor_prev")
mshell.bind({mod}, ".", "focus_monitor_next")
mshell.bind({mod, shft}, ",", "move_to_monitor_prev")
mshell.bind({mod, shft}, ".", "move_to_monitor_next")

-- --- layout ---
mshell.bind({mod}, "t", "layout_tiling")
mshell.bind({mod}, "m", "layout_monocle")
mshell.bind({mod}, "g", "layout_grid")
mshell.bind({mod}, "Space", "cycle_layout")   -- cycle through all layouts
mshell.bind({mod}, "f", "toggle_float")

-- --- fullscreen (all three flavours; each key is its own toggle) ---
mshell.bind({mod, shft}, "f", "fullscreen")           -- window fills the monitor
mshell.bind({mod, ctrl}, "f", "fullscreen_content")   -- app fullscreen stays in the tile
mshell.bind({mod, alt},  "f", "fullscreen_both")      -- app fullscreen fills the monitor

-- --- master area: ratio (Win+Ctrl+h/l) and count (Win+Ctrl+j/k) ---
mshell.bind({mod, ctrl}, "h", "dec_master")
mshell.bind({mod, ctrl}, "l", "inc_master")
mshell.bind({mod, ctrl}, "k", "inc_nmaster")
mshell.bind({mod, ctrl}, "j", "dec_nmaster")

-- --- promote to master ---
mshell.bind({mod}, "Return", "promote_master")

-- --- spawn programs ---
-- dwm convention: Win+Return zooms (promote), Win+Shift+Return opens a terminal.
mshell.bind({mod, shft}, "Return", "spawn", "alacritty.exe")
mshell.bind({mod}, "p", "spawn", "wt.exe")

-- --- sub-map entry points (the direct Win+chord equivalents of tapping Win
--     then w/r/d/o in "normal"). Win+w enters the window map, and so on; from
--     there the map's own flavour decides whether you stay (persisting) or drop
--     back after one key (one-shot). ---
mshell.bind({mod}, "w", "enter_submap", "window")
mshell.bind({mod}, "r", "enter_submap", "resize")
mshell.bind({mod}, "d", "enter_submap", "desktop")
mshell.bind({mod}, "o", "enter_submap", "launch")

-- "go" and "move" are deliberately leader-only (tap Win, then g / m): the
-- matching Win+g and Win+m chords are layout_grid and layout_monocle below, and
-- the first binding for a chord wins, so adding them here would silently shadow
-- one or the other. If you'd rather have the chords, move those two layouts to
-- the window submap (they're already on Win+w g / Win+w m) and uncomment:
-- mshell.bind({mod}, "g", "enter_submap", "go")
-- mshell.bind({mod}, "m", "enter_submap", "move")

-- --- close / kill ---
-- kill is on Win+Shift+x, NOT Win+Shift+k: Win+Shift+h/j/k/l is the move-window
-- block above, and the first binding for a chord wins, so a second Win+Shift+k
-- here was silently shadowed by move_up and kill could never fire. It is also
-- on `k` inside the window submap (Win+w then k).
mshell.bind({mod, shft}, "c", "close")
mshell.bind({mod, shft}, "x", "kill")

-- --- a key can also run a Lua function ---
-- Anything the config can do, a binding can do. The function runs on mshell's
-- main thread when you press the key, so keep it quick — and note that the
-- config-BUILDING calls (bind, submap, rule, spawn, …) are refused from in
-- here: rebuilding the keymaps while the keyboard hook is reading them is what
-- a reload takes a lock for, and a binding holds no such lock. The query calls
-- and mshell.log are fine.
mshell.bind({mod, ctrl}, "i", function()
    local d = mshell.get_current_desktop()
    local w = mshell.get_focused_window()
    mshell.log(("desktop '%s' (%s, %d windows) — focused: %s")
        :format(d and d.name or "?", d and d.layout or "?",
                d and d.windows or 0, w and w.process or "nothing"))
end)

----------------------------------------------------------------------
-- Event handlers
----------------------------------------------------------------------
-- mshell.on(event, fn) runs fn when something happens:
--
--   "window_open"     a window came under management
--   "window_close"    one is about to leave it
--   "desktop_switch"  the visible desktop changed
--   "focus"           the focused window changed
--
-- The handler gets one table: the window for the window/focus events, the
-- desktop (plus `from`) for a switch. Handlers run on mshell's main thread,
-- between your keypress and the screen updating, so keep them fast — and the
-- config-BUILDING calls are refused in here, same as in a function binding.
--
-- Commented out because it writes a line to %TEMP%\mshell.log for every window
-- you open; uncomment when you want to see what mshell is seeing.
--
-- mshell.on("window_open", function(w)
--     mshell.log(("opened %s [%s] on '%s'"):format(w.process, w.class, w.desktop))
-- end)
--
-- mshell.on("desktop_switch", function(d)
--     mshell.log(("%s -> %s (%d windows, %s)")
--         :format(d.from or "?", d.name, d.windows, d.layout))
-- end)

-- --- reload config ---
mshell.bind({mod, shft}, "r", "reload")

-- --- quit mshell (logs out the session if we're the shell!) ---
mshell.bind({mod, shft}, "q", "quit")

----------------------------------------------------------------------
-- Window rules
----------------------------------------------------------------------

-- Browsers tile fine — mshell already leaves their custom chrome alone (it only
-- strips WS_CAPTION frames), so there's no need to float them. Add a rule here
-- only for apps you specifically want floating.
--
-- Match on `class`, `process` (the .exe name) and/or `path` (its full image
-- path). Every one is a case-insensitive wildcard pattern — `*` matches any run
-- of characters, `?` a single one, and `/` and `\` are interchangeable — so a
-- pattern with no wildcard in it is simply an exact match. All the keys you give
-- must match. Rules are tried top to bottom and the FIRST match wins, so put
-- specific rules above broad ones.

-- Keep transient/utility windows floating:
mshell.rule({ process = "Taskmgr.exe" }, "float")

-- Flow Launcher: its search box is a transient popup, not a window to tile.
-- Float it and drop the focus ring so mshell leaves the overlay alone.
mshell.rule({ process = "Flow.Launcher.exe" }, "float", { ring = false })

-- --- System dialogs: file pickers, message boxes, permission prompts ---
--
-- `dialog = true` is the one match that isn't a name. It matches what WINDOWS
-- treats as a dialog, whatever app it belongs to: a window with a TITLE BAR
-- that is also one of — the class Win32 gives every common dialog (Open / Save
-- As / Select Folder, message boxes, print and properties sheets), a modal
-- dialog frame with no maximize box (Qt, WinUI, .NET), or a window owned by
-- another one (GTK, and most app-modal prompts). The title bar is what keeps
-- menus, dropdowns and tooltips out; they are owned popups too.
-- There is nothing else to match those on — a file picker IS the host app's
-- process, so `process = "firefox.exe"` cannot tell the Open box apart from the
-- browser window that opened it.
--
-- Floating them means a picker never joins the grid and never resizes the
-- window you opened it from — it opens at the size the app asked for. It also
-- makes them windows mshell *manages* rather than ones it ignores: they hide
-- with their desktop instead of hovering over the next one you switch to, take
-- the focus ring, and answer the close binding.
--
-- This sits ABOVE the game rules on purpose: an error box from a Steam game
-- should float as the dialog it is, not be stripped bare and stretched over the
-- whole monitor by the library rule further down. To tile one app's dialogs
-- anyway, add a rule naming it above this line:
--   mshell.rule({ process = "code.exe", dialog = true }, "manage")
-- Note this is an ordinary "float" rule, so set_float_policy("never") tiles
-- these along with everything else — that setting means what it says.
mshell.rule({ dialog = true }, "float")

-- Admin authentication. Two windows, neither of them a dialog in the sense
-- above, so they need naming:
--
--   consent.exe is UAC — "Do you want to allow this app to make changes?".
--   It normally draws on the *secure desktop*, a separate Windows desktop no
--   shell can see or touch (the screen dims, you answer, it's gone), so this
--   rule matters only where that is switched off (PromptOnSecureDesktop = 0)
--   and consent.exe becomes an ordinary window on your desktop.
--
--   CredentialUIBroker.exe is the "Windows Security" box that asks for a PIN,
--   a password, a smartcard or Hello — network shares, credential prompts. It
--   is its own process and is NOT owned by whatever asked for it.
mshell.rule({ process = "consent.exe"            }, "float")
mshell.rule({ process = "CredentialUIBroker.exe" }, "float")

-- Explorer's file-operation windows: copy/move progress, "file in use",
-- "confirm delete". Transient, fixed-size, never worth a tile.
mshell.rule({ class = "OperationStatusWindow" }, "float")

-- Not listed here because they need no rule: the XAML consent popups
-- ("Let this app access your camera/location", the Open-with picker) are
-- Windows.UI.Core.CoreWindow windows, which mshell never manages at all — they
-- already float over whatever is on screen.

-- Games. The options that make a window behave like a game:
--   float             never tiled, and the tiler never moves it
--   ring = false      no focus ring painted over the game's edges
--   decorate = false  strip the title bar and add no border — floating windows
--                     normally keep their own chrome, this makes them bare
--   fullscreen = true park it over the monitor's full bounds, ignoring gaps
-- Long-bracket strings ([[...]]) keep Windows paths readable — no \\ escaping.
-- Matching on `path` rather than on each .exe means one rule covers everything
-- installed in a folder, including games you haven't bought yet. Naming a
-- window in a rule also rescues it from the "caption-less popup = menu/tooltip"
-- filter, which is the exact style borderless-fullscreen games use, so the game
-- stays a real managed window: it hides on desktop switch and Win+Shift+c can
-- close it.

-- --- Steam ---
-- Not everything under steamapps\common is a game window. Launchers and crash
-- reporters ship in the same folders, and stretching a 400x300 launcher over
-- the whole monitor is not what you want — so except them FIRST (rules match in
-- order, first one wins) and let them float with their own frame. These two are
-- name heuristics: trim or extend them to match what you actually own.
mshell.rule({ path = [[*\steamapps\common\*]], process = "*launcher*" }, "float")
mshell.rule({ path = [[*\steamapps\common\*]], process = "*crash*"    }, "float")

-- Every Steam game, on every drive and in every library folder. `*\steamapps\`
-- rather than a fixed root so a second library on another disk is covered too.
-- (Non-game apps you install through Steam — Wallpaper Engine, Aseprite — land
-- here as well; give them a "manage" or plain "float" rule above this one.)
mshell.rule({ path = [[*\steamapps\common\*]] }, "float",
            { ring = false, decorate = false, fullscreen = true })

-- --- Riot / Valorant ---
-- Valorant is not a Steam game; it installs under C:\Riot Games\. Match the
-- install path, not the executable: the game binary sits several folders down
-- (VALORANT\live\ShooterGame\Binaries\Win64\VALORANT-Win64-Shipping.exe) and
-- Riot moves and renames it between patches, while the path stays put.
-- Valorant is auto-launched on its own desktop — see its desktop_rule above.
mshell.rule({ path = [[*\Riot Games\VALORANT\*]] }, "float",
            { ring = false, decorate = false, fullscreen = true })

-- The Riot Client (the launcher/patcher that opens first, and comes back when
-- you quit the game) is a normal window you click through — float it so it is
-- never tiled, but leave its frame and its own size alone. Matching the folder
-- covers whichever of its processes owns the window (RiotClientServices.exe /
-- RiotClientUx.exe — that has changed across releases).
mshell.rule({ path = [[*\Riot Games\Riot Client\*]] }, "float", { ring = false })

-- Games outside any of those folders — match the executable directly:
-- mshell.rule({ process = "eldenring.exe" }, "float",
--             { ring = false, decorate = false, fullscreen = true })

----------------------------------------------------------------------
-- Startup
----------------------------------------------------------------------
mshell.spawn("alacritty.exe")

-- Give an app a startup spawn OR a desktop `app` in the table above, never
-- both. They do NOT cancel out: mshell only auto-launches a desktop's app when
-- that desktop is empty, and at startup the spawned terminal's window does not
-- exist yet when that check runs — so the desktop still reads as empty and BOTH
-- launches go through, leaving you with two terminals on every boot. This is
-- why the "term" row above carries no `app`.
--
-- Rule of thumb: mshell.spawn for things that aren't tied to a desktop (the
-- launcher below, a sync client, a hotkey daemon) and want to exist from boot;
-- a desktop rule's `app` for anything that belongs to one desktop and should come
-- back when you return to it.

-- Flow Launcher: a resident app launcher (there is no Start menu under mshell).
-- Start it once here so it's ready instantly; summon its search box with the
-- launch submap: Win+o then a (see the "launch" submap above). Path defined at
-- the top of this file as `flow`; skipped when it couldn't be resolved, so a
-- missing Flow install never costs you the terminal above.
if flow then mshell.spawn(flow) end
