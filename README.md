# Amux

Amux is a keyboard-driven terminal window manager for Linux/Wayland. Sessions,
panes, layouts and terminal history live in a headless server. Closing the GUI
detaches; reopening recreates its windows around the existing shells.

## Run

Requires Nix with flakes, an existing Wayland session, and permission to access a
Mesa-supported GPU render node. From this checkout:

```sh
./integration/run
```

This builds and retains the `amux-mesa` package, then opens the GUI. It supplies
Mesa, application libraries and fonts. The execution machine supplies the shell.
See [integration](integration/README.md) and [dependencies](DEPENDENCIES.md).

`nix run .#amux-mesa` launches the same package directly.
The plain `nix run .` expects an externally configured graphics provider.
Builds are defined for `x86_64-linux` and `aarch64-linux`.

## Keyboard

- `Ctrl-a` enters the manager prefix; `?` opens help.
- `Alt-h/j/k/l` changes pane focus; `Alt-s` toggles zoom.
- `Alt-t` opens the workspace buffer. Type a fuzzy filter, then Enter to open a
  session or window. Escape enters navigation/edit mode: `m` holds a window,
  `p/P` places it, `dd` stages deletion, and `i/a/I/A` edits names inline.
  `o/O` opens a line below/above in insert mode.
  Tab/Shift-Tab sets window/session indentation. `=` or Ctrl-s reviews the
  staged changes in the tree; Enter or `y` confirms, Escape or `n` returns to editing.
  An unpasted held window is deleted on confirmation. `/` returns to filtering.
- `Ctrl-a e` opens the connection buffer. Fuzzy search saved endpoints or type
  an SSH destination or socket path. Escape enters editing: `o/O` adds, `i/A`
  edits, Tab selects a field, `dd` forgets, and `=` reviews local bookmark changes.
  Ctrl-k chooses an SSH key from `~/.ssh` or an absolute path, saved with its connection.
- `Alt-u` enters copy mode; `Escape` or `q` exits.
- In copy mode, use vi motions, `/` to search, `v` to select and `y` to copy.
  `Y` copies, clears selection and stays in copy mode. Counts (`5j`, `3w`, `2f:`)
  apply to navigation; `;` / `,` repeat/reverse character searches. `vi` / `va`
  word, quote and bracket objects follow soft wraps and stop at hard newlines.

## Packages and CLI

`#server` contains the headless server and CLI. `#amux` adds the graphical
frontend; `#amux-mesa` adds explicit Mesa integration. They use the same command:

```sh
amux --help
amux server --socket /tmp/amux.sock --shell /bin/sh
amux gui --socket /tmp/amux.sock
amux list --socket /tmp/amux.sock --json
```

See [public interfaces](INTERFACES.md) for all package outputs, CLI commands,
configuration, keyboard controls, remote connections and protocol formats.

Run the foreground server in another terminal. The normal GUI command starts
or reuses the local server at `$XDG_RUNTIME_DIR/amux/protocol-13.sock` automatically.
Each protocol version has a separate default socket. Servers on separate sockets
can run concurrently; use a matching client build and `--socket PATH` to return to
an older server. Stopping a server ends that server's sessions.

## Configuration

Amux reads `client.toml` and `server.toml` under `$XDG_CONFIG_HOME/amux`,
normally `~/.config/amux`. Missing default files use bundled defaults;
invalid files and settings for the other process fail.

In `server.toml`:

```toml
shell = ["bash"]
```

In `client.toml`:

```toml
prefix = "Ctrl+b"
```

`shell` is a command argument array; its executable is resolved on the server's
PATH unless absolute. Amux appends `-i`; the bundled default is `["sh"]`.
`server --shell COMMAND` overrides that setting. `--config PATH` selects client
configuration on `gui` and graphical `connect`, and server configuration on
`server` and `serve-stdio`. `gui --server-config PATH` selects configuration for
local server autostart. Remote servers read their own config. Reopen the GUI to
apply client settings; start a fresh server to apply server settings. The
execution machine supplies the shell.

An amber dot on a window tab marks panes that rang a bell or sent a notification
while unfocused. Focusing a marked pane clears its flag. Configure a notification
command in `client.toml`:

```toml
notify = ["notify-send", "--app-name=Amux"]
```

Amux appends a title and body as separate arguments and limits invocation to one
attention event per second. The execution environment supplies the command;
`notify = []`, the default, disables invocation. Notifications from the focused
pane are suppressed while the desktop window has keyboard focus and the manager
is in normal mode. An unfocused desktop window also requests host attention
through the outer compositor's optional `xdg_activation_v1` interface; the
desktop decides how to display it.

Alacritty renders terminal windows inside Amux's nested compositor. Amux appears
as one desktop window. The server owns layout and logical focus; the frontend
applies those decisions and manages presentation, input and clipboard delivery.

Amux's original code is [MIT licensed](LICENSE).
The compositor includes MIT-licensed code derived from wlroots' tinywl example;
its notice is retained in [src/compositor/LICENSE](src/compositor/LICENSE).
