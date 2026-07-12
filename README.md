# Cronos Terminal Multiplexer

A `tmux`-style terminal multiplexer written in C, built on a Unix-domain-socket
client/server architecture with a `notcurses` terminal UI. Sessions run as detached
background daemons that keep your shells alive after you disconnect, and reattach
later exactly where you left off.

## Features

- **PTY-backed shells** - each pane is a real pseudoterminal running your shell, not a
  simulation.
- **Detach / reattach** - sessions remain running when your terminal disconnects;
  reattach from the CLI and pick up where you left off.
- **Panes** - split any pane horizontally or vertically, navigate between them, resize
  them, close them (with automatic neighbor absorption).
- **Windows (tabs)** - multiple independent pane layouts per session, switchable with a
  footer tab bar, closable when no longer needed.
- **In-app session manager** - list, switch to, create, and rename sessions without
  leaving the multiplexer (`Ctrl+A`).
- **Scrollback** - per-pane history buffer, browsable with Page Up / Page Down.
- **Config file** - customize shell path, environment variables, colors, and
  keybindings via `~/.config/cronos/cronos.conf`.
- **ANSI/VT100 rendering** - SGR colors (including 256-color and truecolor forms),
  cursor movement and save/restore, erase sequences, tabs.

## Requirements

- Linux (uses `posix_openpt`, `epoll`, Unix domain sockets)
- `gcc`
- `notcurses` / `notcurses-core` development libraries

On Debian/Ubuntu:
```bash
sudo apt install build-essential libnotcurses-dev
```

## Build

```bash
make
```

This produces two binaries in the current directory: `cronos` (the client) and
`cronos-server` (the background daemon). The client execs `./cronos-server` by
relative path when starting a new session, so run `cronos new <name>` from inside this
directory unless you've installed both binaries onto your `PATH` (see below).

## Install (optional)

```bash
sudo make install        # installs to /usr/local/bin
sudo make PREFIX=/custom/path install   # or a custom prefix
```

```bash
sudo make uninstall
```

## Usage

```bash
cronos new <name>       # create a new background session and attach to it
cronos attach <name>    # reattach to an existing session
cronos list             # list all active sessions
cronos kill <name>      # terminate a specific session
cronos killall          # terminate all sessions
cronos --help, -h       # show CLI usage
cronos --inst, -i       # show keybinding cheat sheet
```

## Default Keybindings

| Key                     | Action                              |
|--------------------------|--------------------------------------|
| `Ctrl+Q`                | Detach from the session              |
| `Ctrl+V`                | Split active pane vertically         |
| `Ctrl+B`                | Split active pane horizontally       |
| `Ctrl+H` / `Ctrl+L`     | Focus pane left / right              |
| `Ctrl+J` / `Ctrl+K`     | Focus pane down / up                 |
| `Ctrl+W`                | Close active pane                    |
| `Alt+←→↑↓`              | Resize active pane                   |
| `Page Up` / `Page Down` | Enter / scroll / exit scrollback      |
| `Ctrl+N`                | Create a new window (tab)             |
| `Ctrl+R`                | Rename the current window             |
| `Ctrl+X`                | Close the current window              |
| `Ctrl+]` / `Ctrl+[`     | Next / previous window                |
| `Ctrl+A`                | Open the session manager              |

All of these can be supplemented with custom bindings - see **Configuration** below.

## Configuration

Create `~/.config/cronos/cronos.conf`:

```conf
# Server-side
shell = /usr/bin/zsh
env EDITOR = nvim

# Client-side
color_fg = 220,220,220
color_bg = 0,0,0
border_color = 100,100,100

bind ctrl+n = split_vert
bind alt+left = resize_left
```

Recognized action names for `bind`: `detach`, `split_vert`, `split_horz`,
`focus_left`, `focus_right`, `focus_up`, `focus_down`, `close_pane`,
`resize_left`, `resize_right`, `resize_up`, `resize_down`, `scroll_up`,
`scroll_down`, `new_window`, `next_window`, `prev_window`, `rename_window`,
`close_window`, `session_menu`.

Recognized key names for `bind`: any single printable character, `left` /
`right` / `up` / `down`, `pgup` / `pgdown`, `enter`, `backspace`.

## Project Layout

```
client.c          # CLI entry point, event loop, config loading
server.c          # background daemon: PTY management, epoll loop, protocol handling
pane.c            # pane split/close/focus/resize/render logic
window.c          # window (tab) lifecycle (create/switch/rename/close) and footer tab bar
keymap.c          # keybinding table + config-driven custom bindings
parser.c          # ANSI/VT100 byte-level terminal emulator + scrollback capture
guide.c           # CLI help text + scrollback overlay rendering
session_ui.c      # in-app session manager overlay
headers/
  cronos.h        # shared structs, enums, and wire-protocol constants
  guide.h
  keymap.h
  pane.h
  parser.h
  session_ui.h
  window.h
Makefile
```

## Documentation

See `cronos_documentation.md` for the full architecture overview, protocol
specification, terminal-emulation details, and known limitations.

## Known Limitations

- Only pane 0's output (and pane-split layout) is replayed on reattach; other
  panes' pre-detach scrollback is lost, though their geometry is restored.
- No alternate-screen-buffer (`?1049h/l`) or scroll-region (`DECSTBM`) support
  - full-screen programs like `vim` or `htop` may render imperfectly.
- No mouse support (scrollback and pane navigation are keyboard-driven only).
- Fixed internal limits: up to 10 panes per session, 32 custom environment
  variables, 64 keybindings, and 256 bytes per socket packet (larger writes
  are chunked automatically).

See the documentation PDF for the complete list.