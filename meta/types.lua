---@meta

---@alias mshell.Direction "left"|"down"|"up"|"right"
---@alias mshell.Step "next"|"prev"
---@alias mshell.FullscreenMode "window"|"content"|"both"
---@alias mshell.LayoutName "tiling"|"monocle"|"grid"|"spiral"|"centered"|"bstack"|"columns"|"bsp"
---@alias mshell.NotifyKind "info"|"warn"|"error"
---@alias mshell.LogLevel "error"|"warn"|"info"|"debug"|"trace"
---@alias mshell.AttachPolicy "end"|"master"|"after"
---@alias mshell.RuleAction "manage"|"float"|"ignore"
---@alias mshell.Event "window_open"|"window_close"|"desktop_switch"|"focus"
---@alias mshell.BarMode "top_bar"|"floating"
---@alias mshell.BarModule "desktops"|"layout"|"title"|"clock"|"notifications"
---@alias mshell.Corners "square"|"round"|"small"|"default"
---@alias mshell.WhichKeyPos "center"|"top"|"bottom"|"left"|"right"|"top_left"|"top_right"|"bottom_left"|"bottom_right"

---An action value, such as `mshell.window.close`. Passing one to a binding
---keeps it a native action, so which-key can label the key and the keypress
---never enters Lua.
---@alias mshell.Action table|function

---What a key may be bound to: an action, a function, a submap name, or a
---table pairing one of those with a `desc` for the which-key panel.
---@alias mshell.Binding mshell.Action|fun()|string|table

---A window under mshell's management. Fields are read off the live window, so
---they are never stale; they are nil once the window is gone.
---@class mshell.Window
---@field title string
---@field class string
---@field process string Executable name, without its path.
---@field path string Full path to the executable.
---@field desktop string Name of the desktop it is on.
---@field monitor integer
---@field floating boolean
---@field fullscreen boolean
---@field managed boolean False for a window mshell only tracks.
---@field valid boolean False once the window has gone.
---@field hwnd integer The Win32 handle, for interop.
local Window = {}

---Give this window focus.
function Window:focus() end

---Move it: a direction, or `{ desktop = }` / `{ monitor = }`.
---@param opts mshell.MoveOpts|mshell.Direction
function Window:move(opts) end

---Resize it. Floating windows only.
---@param opts mshell.ResizeOpts|mshell.Direction
function Window:resize(opts) end

---Read or set whether it floats.
---@param floating? boolean
---@return boolean?
function Window:float(floating) end

---@param mode? mshell.FullscreenMode
function Window:fullscreen(mode) end

---Center it on its monitor. Floating windows only.
function Window:center() end

---Promote a tracked window into the layout.
function Window:promote() end

---Ask it to close.
function Window:close() end

---Terminate its process.
function Window:kill() end

function Window:minimize() end
function Window:restore() end

---@class mshell.Desktop
---@field name string
---@field current boolean
---@field windows integer How many windows are on it.
---@field layout mshell.LayoutName
---@field master_ratio number
---@field nmaster integer
---@field float boolean
---@field monitor integer? The display it is pinned to, nil if unpinned.
---@field app string? Command launched when it is first created.

---Geometry is in physical pixels; mshell is per-monitor DPI aware.
---@class mshell.Monitor
---@field x integer
---@field y integer
---@field width integer
---@field height integer
---@field work_x integer
---@field work_y integer
---@field work_width integer
---@field work_height integer
---@field dpi integer
---@field scale number dpi / 96.
---@field primary boolean
---@field focused boolean
---@field device string Stable identity, unlike the index.
---@field refresh integer?
---@field rotation integer?
---@field hdr boolean? nil when the panel cannot do HDR at all.

---A field left at 0 or omitted means "leave this one alone".
---@class mshell.DisplayMode
---@field width integer?
---@field height integer?
---@field refresh integer?
---@field rotation integer? 0, 90, 180 or 270.

---Exactly one of `dir`, `desktop` or `monitor`.
---@class mshell.MoveOpts
---@field dir mshell.Direction?
---@field desktop string? Created if it does not exist.
---@field monitor (integer|mshell.Step)?

---Any subset; anything omitted keeps its current value.
---@class mshell.ResizeOpts
---@field x integer?
---@field y integer?
---@field width integer?
---@field height integer?

---Every field is optional; a window must match all the ones given.
---@class mshell.WindowFilter
---@field desktop string?
---@field monitor integer?
---@field floating boolean?
---@field class string? Wildcard pattern.
---@field process string? Wildcard pattern.

---Patterns accept `*` and `?`, and treat `/` and `\` as equal.
---@class mshell.RuleMatch
---@field class string?
---@field process string?
---@field path string?
---@field title string?
---@field dialog boolean? Anything Windows considers a dialog.

---@class mshell.RuleOpts
---@field ring boolean? Draw the focus ring on it.
---@field decorate boolean? Leave the window's own title bar alone.
---@field fullscreen boolean? Let the app's own fullscreen cover the monitor.
---@field desktop string? Send it here when it opens.
---@field monitor integer?
---@field geometry integer[]? `{x, y, w, h}`, floating windows only.
---@field center boolean?
---@field start_fullscreen boolean?

---@class mshell.DesktopRule
---@field default boolean? Start on this desktop.
---@field app string|string[]? Launch this when the desktop is created.
---@field gaps integer|integer[]?
---@field float boolean? Float everything on it.
---@field layout mshell.LayoutName?
---@field master_ratio number?
---@field nmaster integer?
---@field monitor integer? Pin it to a display.

---@class mshell.MonitorRule
---@field gaps integer|integer[]?
---@field nmaster integer?
---@field master_ratio number?
---@field layout mshell.LayoutName?
---@field resolution integer[]? `{width, height}`.
---@field refresh integer?
---@field rotation integer? 0, 90, 180 or 270.
---@field hdr boolean?
---@field primary boolean? Make this the primary display.
---@field position integer[]? `{x, y}` in the virtual desktop.

---@class mshell.BarOpts
---@field enabled boolean?
---@field mode mshell.BarMode?
---@field position "top"|"bottom"|nil Only meaningful in top_bar mode.
---@field height integer? Scaled per monitor for DPI; sets the type scale.
---@field bg integer? 0xRRGGBB.
---@field fg integer?
---@field accent integer? The desktop you are on.
---@field dim integer? The others.
---@field modules mshell.BarModule[]?

---@class mshell.BorderOpts
---@field width integer?
---@field focused integer? 0xRRGGBB.
---@field floating integer?
---@field urgent integer?
---@field corners mshell.Corners?

---@class mshell.DimOpts
---@field enabled boolean?
---@field color integer?
---@field opacity integer? 0 to 255.

---@class mshell.NotifyOpts
---@field enabled boolean?
---@field desktop_switch boolean? Announce every desktop change.

---@class mshell.MouseOpts
---@field drag_swap boolean?
---@field follow boolean? Focus follows the pointer.
---@field warp boolean? Move the pointer to the focused window.
---@field mod_drag boolean?
---@field speed integer? Windows pointer speed, 1 to 20.
---@field accel boolean?
---@field swap_buttons boolean?

---@class mshell.WhichKeyOpts
---@field enabled boolean?
---@field delay integer? Milliseconds before the panel appears.
---@field position mshell.WhichKeyPos?
---@field margin integer?
---@field max_width integer?
---@field max_height integer?
---@field max_rows integer? Wrap into a new column past this many.
---@field padding integer?
---@field row_spacing integer?
---@field column_spacing integer?
---@field key_spacing integer?
---@field header_spacing integer?
---@field font string?
---@field font_size integer?
---@field border_width integer? 0 for no outline.
---@field opacity integer? 0 to 255.
---@field rounded boolean?
---@field bg integer?
---@field fg integer?
---@field key_fg integer?
---@field border integer?

---@class mshell.BindOpts
---@field desc string? The which-key label. A function has no name of its own,
---so this is the only way to label one.
---@field terminal boolean? Return to the root map after firing.

---@class mshell.SubmapOpts
---@field persist boolean? Stay in the map until the exit key.
---@field exit string? The key that leaves a persisting map; replaces Escape.
---@field sticky boolean? Alias for persist.
