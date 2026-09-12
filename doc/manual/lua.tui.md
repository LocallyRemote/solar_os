+++
id = "lua.tui"
title = "Lua text user-interface API"
section = "api"
summary = "Build terminal applications from Lua"
aliases = []
keywords = "lua tui terminal text interface curses keyboard keys input box bold inverse"
agent_reference_sections = true
packages_any = ["app_lua"]
+++
# Lua text user-interface API

[API overview](lua.md) · [Python text user interfaces](python.tui.md)

Use the shared layout on displays and cursor-addressable port shells:

```lua
local tui = solaros.tui
local _, _, body = table.unpack(tui.layout())
tui.title("Example")
tui.cell(body[1], 0, body[4], "Shared layout")
tui.help("Enter open  Esc exit")
```

## `solaros.tui`

- `solaros.tui`: curses-like terminal drawing functions

## TUI

`solaros.tui` draws into one buffered foreground frame. `refresh()` atomically
commits the changed cells. It exposes constants
`NORMAL`, `BOLD`, `INVERSE`, plus common key constants such as `KEY_UP`,
`KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_CTRL_LEFT`, `KEY_CTRL_RIGHT`,
`KEY_ESCAPE`, `KEY_PAGE_UP`, and `KEY_PAGE_DOWN`.

Functions:

- `rows()`, `cols()`, `size()`
- `clear()`, `refresh()`
- `move(row, col)`, `write(text[, attr])`, `addstr(row, col, text[, attr])`
- `putch(row, col, ch[, attr])`
- `hline(row, col, width[, attr])`, `vline(row, col, height[, attr])`, `vrule(row, col, height[, width[, attr]])`
- `box(row, col, height, width[, attr])`
- `fill(row, col, height, width[, ch[, attr]])`
- `getch([timeout_ms])`

Example:

```lua
local solaros = require("solaros")
local tui = solaros.tui

tui.clear()
tui.box(0, 0, tui.rows(), tui.cols())
tui.addstr(1, 2, "SolarOS Lua", tui.BOLD)
tui.addstr(3, 2, "Press ESC")
tui.refresh()

while not solaros.should_exit() do
    local key = tui.getch(250)
    if key == tui.KEY_ESCAPE then
        break
    end
end
```

## Quick reference

High-level: layout, cell, title, help, tab, list_move, input_edit, input.
Rectangles are `{row, col, height, width}`. The low-level API remains.

`input(row, col, width, label, text, cursor, view[, attr[, masked]])` draws an
editable input row. Set `masked` to `true` to draw one `*` per UTF-8 character.
The mask is render-only: `text` and the value returned by `input_edit()` remain
unchanged, so a script must still avoid logging secrets and discard them when
they are no longer needed.

Use `tui.getch()` for keyboard characters and navigation keys. Use
`solaros.input.read()` for foreground touch, mouse, and joystick events.
