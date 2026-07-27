--[[
    mshell — default configuration.

    Deliberately small, and it assumes nothing is installed but Windows itself.
    Everything here works on a bare machine; change the terminal on the first
    line below and you have your setup.

    A much larger, heavily commented example lives beside this file as
    init.full.lua — leader menus, per-desktop auto-launch, game rules,
    which-key styling, event handlers. Read it when you want more; copy it over
    this file if you want all of it.

    Location:  %APPDATA%\mshell\init.lua
    Reload:    save the file (auto-reload), or press Win+Shift+R.
    Safety:    a syntax error keeps the PREVIOUS config running and writes the
               reason to %LOCALAPPDATA%\mshell\mshell.log — a bad edit cannot
               strand you.
--]]

-- The one thing worth changing first. cmd.exe is used because it is the only
-- terminal guaranteed to exist on every Windows install; swap in
-- "wt.exe", "alacritty.exe", "pwsh.exe", … once you know what you have.
local TERMINAL = "cmd.exe"

local mod, shft, ctrl = "LWin", "Shift", "Ctrl"

----------------------------------------------------------------------
-- Appearance
----------------------------------------------------------------------
mshell.set_gaps(6, 6)            -- gap between windows, gap at the screen edge
mshell.set_border(2, 0xffffff)   -- focus ring: width, 0xRRGGBB
mshell.set_background(0x1e1e2e)  -- desktop backdrop (there is no wallpaper)
mshell.set_layout("tiling")      -- tiling|monocle|grid|spiral|centered|bstack|columns
mshell.set_master_ratio(0.60)

-- Status bar. There is no taskbar, and because desktops are created and
-- destroyed as you use them, this is the only thing telling you which ones
-- exist and which you are on.
--
-- mode = "top_bar" is a strip on every monitor that reserves its space, so
-- tiled windows sit below it while a fullscreen window still covers it.
-- mode = "floating" is instead one panel in the middle of the focused monitor
-- — big clock, date, desktops, and mshell's notifications listed inline — that
-- floats over the windows and reserves nothing. Bind `toggle_bar` to get it
-- out of the way.
mshell.set_bar({
    enabled  = true,
    mode     = "top_bar",    -- or "floating"
    position = "top",        -- or "bottom" (top_bar mode)
    height   = 28,           -- scaled per monitor for DPI; sets the type scale
    bg       = 0x1e1e2e,
    fg       = 0xcdd6f4,
    accent   = 0x7aa2f7,     -- the desktop you're on
    dim      = 0x6c7086,     -- the others
    -- "notifications" is floating-only: with it on, the panel is where
    -- mshell's messages appear instead of as separate toasts.
    modules  = { "desktops", "layout", "title", "clock", "notifications" },
})

----------------------------------------------------------------------
-- Window rules
----------------------------------------------------------------------
-- Float anything Windows considers a dialog — file pickers, message boxes,
-- permission prompts. They carry no class or process of their own (a file
-- picker IS the host app's process), so this is the only way to name them.
mshell.rule({ dialog = true }, "float")

-- Credential and UAC prompts are their own processes and are not dialogs in
-- the sense above, so they need naming.
mshell.rule({ process = "consent.exe"            }, "float")
mshell.rule({ process = "CredentialUIBroker.exe" }, "float")

----------------------------------------------------------------------
-- Keybindings — hold Win and press a key
----------------------------------------------------------------------
-- focus / move, vim keys
mshell.bind({mod},       "h", "focus_left")
mshell.bind({mod},       "j", "focus_down")
mshell.bind({mod},       "k", "focus_up")
mshell.bind({mod},       "l", "focus_right")
mshell.bind({mod, shft}, "h", "move_left")
mshell.bind({mod, shft}, "j", "move_down")
mshell.bind({mod, shft}, "k", "move_up")
mshell.bind({mod, shft}, "l", "move_right")

-- desktops. These are NAMES, not slots: Win+3 goes to a desktop called "3",
-- which is created when you go there and destroyed when you leave it empty.
-- Any name works, whether or not it appears in this file.
for i = 1, 9 do
    mshell.bind({mod},       tostring(i), "switch_desktop",  tostring(i))
    mshell.bind({mod, shft}, tostring(i), "move_to_desktop", tostring(i))
end
mshell.bind({mod}, "`", "last_desktop")   -- back where you came from
mshell.bind({mod}, "]", "next_desktop")   -- step through what exists now
mshell.bind({mod}, "[", "prev_desktop")

-- monitors
mshell.bind({mod},       ",", "focus_monitor_prev")
mshell.bind({mod},       ".", "focus_monitor_next")
mshell.bind({mod, shft}, ",", "move_to_monitor_prev")
mshell.bind({mod, shft}, ".", "move_to_monitor_next")

-- layout
mshell.bind({mod}, "t",     "layout_tiling")
mshell.bind({mod}, "m",     "layout_monocle")
mshell.bind({mod}, "g",     "layout_grid")
mshell.bind({mod}, "Space", "cycle_layout")
mshell.bind({mod}, "f",     "toggle_float")
mshell.bind({mod}, "Return","promote_master")

-- master area: ratio (Ctrl+h/l), count (Ctrl+j/k)
mshell.bind({mod, ctrl}, "h", "dec_master")
mshell.bind({mod, ctrl}, "l", "inc_master")
mshell.bind({mod, ctrl}, "j", "dec_nmaster")
mshell.bind({mod, ctrl}, "k", "inc_nmaster")

-- fullscreen. Three flavours, because the WINDOW and the app's own CONTENT
-- fullscreen (YouTube's button, F11) are different things:
mshell.bind({mod, shft}, "f", "fullscreen")          -- window covers the monitor
mshell.bind({mod, ctrl}, "f", "fullscreen_content")  -- app's fullscreen stays in the tile
mshell.bind({mod},       "F11", "fullscreen_both")   -- app's fullscreen covers the display

-- windows. Note `restore`: there is no taskbar under mshell, so it is the only
-- way to bring a minimized window back.
mshell.bind({mod, shft}, "c", "close")
mshell.bind({mod, shft}, "x", "kill")
mshell.bind({mod},       "n", "minimize")
mshell.bind({mod, shft}, "n", "restore")
mshell.bind({mod},       "s", "toggle_sticky")   -- follow me to every desktop
mshell.bind({mod},       "z", "zoom")            -- swap with master, and back

-- Scratchpad: one window you can summon anywhere and dismiss again. Mark a
-- window once (Win+Shift+s), then Win+grave toggles it wherever you are.
mshell.bind({mod, shft}, "s", "mark_scratchpad")
mshell.bind({mod, ctrl}, "Space", "toggle_scratchpad")

-- programs
mshell.bind({mod, shft}, "Return", "spawn", TERMINAL)

-- mshell itself
mshell.bind({mod, shft}, "r", "reload")
mshell.bind({mod, shft}, "q", "quit")   -- as the shell, this logs you out

----------------------------------------------------------------------
-- Leader mode — tap Win on its own, then press bare keys
----------------------------------------------------------------------
-- A submap is `persist = true` (stay until Esc) or one-shot (next key drops
-- back). This one persists, so h/j/k/l can be pressed repeatedly.
mshell.submap("normal", {
    h = "focus_left",
    j = "focus_down",
    k = "focus_up",
    l = "focus_right",
    Space  = "cycle_layout",
    Return = "promote_master",
    f      = "toggle_float",
    c      = "close",
    t      = {"spawn", TERMINAL},
}, { persist = true })

mshell.set_leader("normal")   -- a bare Win tap enters it; Esc or Win leaves

----------------------------------------------------------------------
-- Startup
----------------------------------------------------------------------
-- Nothing is launched by default — you may not have any of the usual
-- suspects installed. Add what you want:
--
--   mshell.spawn("alacritty.exe")
--   mshell.spawn("wt.exe", "-p Ubuntu")     -- arguments are a second string
