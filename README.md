# Amux

Amux is a keyboard-driven terminal workspace manager for Linux/Wayland. Sessions,
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
or reuses the default local server automatically. Server replacement requires
stopping the old server and starting a fresh one. Stopping the server ends its
sessions.

## Configuration

Amux reads `$XDG_CONFIG_HOME/amux/config.toml`, normally
`~/.config/amux/config.toml`. A missing default file uses bundled defaults;
invalid files fail. `--config PATH` on `gui`, `connect`, `server` or `serve-stdio`
selects a different file.

```toml
shell = ["bash"]
prefix = "Ctrl+b"
```

`shell` is a command argument array; its executable is resolved on the server's
PATH unless absolute. Amux appends `-i`; the bundled default is `["sh"]`.
`server --shell COMMAND` overrides that setting. An explicit GUI config reaches
a newly started local server; remote servers read their own config. Running
servers retain their startup settings. The execution machine supplies the shell.

Alacritty renders terminal windows inside Amux's nested compositor. Amux appears
as one desktop window. The server owns layout and logical focus; the frontend
applies those decisions and manages presentation, input and clipboard delivery.

Amux's original code is [MIT licensed](LICENSE).
The compositor includes MIT-licensed code derived from wlroots' tinywl example;
its notice is retained in [src/compositor/LICENSE](src/compositor/LICENSE).
