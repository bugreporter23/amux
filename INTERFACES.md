# Public interfaces

Amux manages sessions, workspaces and terminal panes. The server owns shells,
PTYs, terminal state, layout and focus. A session permits one active manager
attachment. Detach preserves its shells and state; server shutdown ends every
session. Closing the last pane closes its workspace. An empty session can create
a new workspace.

## Nix packages and launchers

The flake defines the following packages for `x86_64-linux` and `aarch64-linux`.

| Package | Executables and purpose |
| --- | --- |
| `#server` | `amux`: headless server, management CLI and stdio transport |
| `#amux`, `#default` | `amux`: server and CLI with the Wayland frontend and patched Alacritty |
| `#amux-mesa` | `amux`: the same application with explicit Mesa and font configuration |
| `#compositor` | `amux-client`: nested Wayland frontend component |
| `#alacritty` | Patched `alacritty` and `amux-terminal-state` |
| `#terminal-state` | `amux-terminal-state`: native terminal-state component |

`nix run .` and `nix run .#amux` use the explicit `default` and `amux` app
entries. `nix run .#amux-mesa` and `nix run .#server` use their package's `amux`
executable. All three wrappers expose the same CLI; graphical commands require
the frontend components supplied by `#amux` or `#amux-mesa`.

`#amux` expects an externally configured graphics provider, normally discovered
through `/run/opengl-driver`. `#amux-mesa` supplies Mesa and fonts and selects
them for the compositor and Alacritty only. Servers, shells and transport
commands retain the caller's graphics environment. Both graphical packages
require a Linux Wayland desktop and access to a usable GPU render node.
See [dependencies](DEPENDENCIES.md) for the graphics interface.

`./integration/run [arguments...]` builds `#amux-mesa`, retains it at
`${XDG_STATE_HOME:-$HOME/.local/state}/amux-integration/mesa`, and forwards the
arguments to `amux`. See [integration](integration/README.md).

The component executables are used by the packaged application. They require
its configuration and protocol wiring; launch the GUI with `amux gui`.

## CLI

`amux --help`, `amux COMMAND --help` and `amux --version` describe the installed
release. A bare invocation selects `gui`. GUI launches return after startup;
`--foreground` waits for the frontend to exit.

| Command | Behavior and command-specific options |
| --- | --- |
| `gui` | Attach the Wayland frontend; `--session ID`, `--width PX`, `--height PX`, `--config PATH`, `--foreground`, `--pick-connection` |
| `server` | Run a foreground server; `--shell COMMAND [ARG...]`, `--config PATH` |
| `client` | Attach a line-oriented management CLI; `--session ID`, `--width PX`, `--height PX`, `--json` |
| `list` | List sessions; `--json` |
| `create` | Create a detached session; `--name NAME`, `--width PX`, `--height PX`, `--json` |
| `destroy` | End a session and its shells; required `--session ID`, optional `--json` |
| `capture` | Write terminal text; required `--session ID`, optional `--pane ID`, `--history N`, `--json` |
| `clear-history` | Clear primary-screen scrollback; required `--session ID`, optional `--pane ID`, `--json` |
| `prune` | Stop the selected server and remove its socket; ends all its sessions |
| `connect` | Attach through a transport command; `--session ID`, `--cli`, `--json`, `--config PATH`, `--foreground`, `--trace-dir DIR`, then `-- COMMAND [ARG...]` |
| `serve-stdio` | Serve the binary stdio transport; `--socket PATH`, `--config PATH`, `--trace-dir DIR` |
| `relay` | Terminal component; required `--socket PATH`, `--key KEY`, `--pane ID` |
| `trace-report DIR` | Summarize recorded performance events |

`gui`, `server`, `client`, `list`, `create` and `destroy` accept `--socket PATH`
and `--trace-dir DIR`. `capture`, `clear-history` and `prune` accept `--socket`.
`list` and `capture` operate on detached or attached sessions.
Initial viewport defaults are 1200 × 800 pixels.
`capture` defaults to zero historical rows and permits at most 10,000; its
`--pane` and `clear-history`'s `--pane` default to the focused pane in the active
workspace. Capture preserves soft wraps and Unicode, removes escape sequences,
and has separate 8 MiB cell and text limits. On the alternate screen it captures
that screen only.

The default endpoint is `$XDG_RUNTIME_DIR/amux/default.sock`.
The normal GUI starts or reuses that server. A GUI `--socket` selecting another
endpoint connects to an existing server; other management CLI commands require
a running server.
The server refuses to overwrite an existing socket. To replace a server version,
stop it and start a fresh server from the selected package. Stopping the server
ends its sessions. `prune` also handles a stale socket after an unclean exit.

For an explicit local endpoint, run this in one terminal:

```sh
amux server --socket "$XDG_RUNTIME_DIR/amux-example.sock" --shell sh
```

In another terminal, inspect or attach to it:

```sh
amux list --socket "$XDG_RUNTIME_DIR/amux-example.sock" --json
amux gui --socket "$XDG_RUNTIME_DIR/amux-example.sock" --foreground
```

`client` reads one manager command per input line. The GUI's prefix `:` prompt
uses the same grammar. IDs select sessions or panes; workspace numbers are local
to a session and increase monotonically.

```text
state | zoom | split h|v [pane] | close [pane]
focus <pane|left|right|up|down|next|prev> | resize h|v <pixels> [pane]
move left|right|up|down [pane]
move workspace <number> [h|v] [pane] | join [pane] [h|v]
workspace new|next|prev|last|select <number>|close [number]
session new [name]|last|select <id> | rename session|workspace|pane <name>
clear-history [pane] | capture [pane] [history-rows]
viewport <width> <height> | detach | destroy | help
```

Quote names containing spaces, such as `rename pane "build logs"`. Split axis
`h` divides width into left/right panes; `v` divides height into top/bottom panes.
Resize steps are pixels. Splits and new workspaces inherit the focused or
targeted shell's working directory; initial panes use the server's directory.

## Configuration and environment

The user file is `$XDG_CONFIG_HOME/amux/config.toml`, defaulting to
`~/.config/amux/config.toml`. A missing default file uses bundled defaults.
An explicitly selected missing file, invalid values or unknown settings fail.
`--config PATH` is available on `gui`, `server`, `serve-stdio` and graphical
`connect`; `connect --cli` rejects `--config`.

| Setting | Default and meaning |
| --- | --- |
| `shell` | `["sh"]`; nonempty executable/argument array; Amux appends `-i` |
| `prefix` | `"Ctrl+a"`; manager prefix key |
| `repeat_rate` | `25`; repeats per second, nonnegative integer |
| `repeat_delay` | `300`; milliseconds before repeating, nonnegative integer |
| `connections` | `[]`; named local sockets or transport argument arrays |
| `bindings.normal`, `bindings.prefix`, `bindings.resize` | Tables mapping key combinations to commands; supplied entries override defaults; an empty string disables a binding |

The execution machine supplies the shell.
`server --shell` overrides the configured command. A GUI's explicit config
reaches a newly started local server; an existing server keeps its startup
settings. Remote servers read configuration on their own machine.

```toml
shell = ["bash"]
prefix = "Ctrl+b"

[bindings.normal]
"Alt+s" = "zoom"

[[connections]]
name = "Other local server"
socket = "$XDG_RUNTIME_DIR/amux-other.sock"

[[connections]]
name = "Remote server"
transport = ["ssh", "-T", "example-host", "amux", "serve-stdio"]
```

Connection names must be unique, printable and nonempty. Each profile specifies
exactly one of `socket` or `transport`. Socket paths expand environment variables
and `~`; transport commands receive their configured argument arrays directly.
The connection picker also includes the default local endpoint and the launch
connection. Configure the example's SSH host before using its remote profile.

Bindings accept manager commands, `mode normal`, `mode resize`, and
`ui workspaces|sessions|connections|command|session-name|workspace-name|pane-name|help`.
The GUI uses the bundled Alacritty preset.
Interactive shells still read their normal startup files.

`XDG_RUNTIME_DIR` must name an absolute writable runtime directory; the GUI also
needs `WAYLAND_DISPLAY`. Background-launch logs go under
`${XDG_STATE_HOME:-$HOME/.local/state}/amux`. `AMUX_TRACE_DIR` or `--trace-dir`
enables tracing; `AMUX_GRAPHICS_DEBUG=1 amux gui --foreground` reports graphics
selection and renderer logs. The Mesa wrapper sets `AMUX_GUI_MESA` and
`AMUX_GUI_FONTCONFIG` for GUI provider selection. Component paths and other
`AMUX_*` variables are supplied by the wrappers and frontend.

## Keyboard, copy mode and clipboard

The bundled prefix is `Ctrl-a`, followed by a command key:

| Keys | Action |
| --- | --- |
| `c`, `%`, `"` | New workspace, horizontal split, vertical split |
| Arrows, `o` | Directional focus, next pane |
| `j`, `x`, `&` | Join a pane from the previous workspace, close pane, close workspace |
| `1`–`9` | Select workspace number |
| `<`, `>`; `+`, `=`; `-`, `_` | Shrink/grow width; shrink height; grow height by 8 pixels |
| Alt-arrows, Ctrl-arrows | Resize by 5 or 1 pixel |
| `w`, `s`, `S`, `C` | Workspace picker, session picker, new session, connection picker |
| `:`, `,`, `$`, `P`, `?` | Command prompt, rename workspace/session/pane, help |
| `d`, Escape, `Ctrl-g` | Detach, leave prefix/resize mode |
| Prefix again | Send the prefix key to the terminal |

Unprefixed Alt-h/j/k/l changes focus, Alt-H/L switches workspaces,
Alt-s toggles zoom, Alt-t opens sessions, and Alt-y/Y selects the previous
workspace/session. In an empty session, unprefixed `q` destroys it and selects
a detached session if available. Close operations terminate their shells.

Alt-u toggles copy mode. Escape or `q` exits; vi motions, counts, PageUp/PageDown
and Ctrl-u/d/b/f navigate. `v` selects, Ctrl-v selects a rectangle, `y` copies
and exits, `Y` copies and stays, and `L` copies the logical line and exits.
`/` and `?` search; Enter accepts; `n`/`N` repeats. `f/F/t/T` searches a character,
`;`/`,` repeats/reverses, and `i/a` with word, quote or bracket objects selects
across soft wraps. `p/P` selects previous/next prompt positions; `c` follows
prompt/output markers. Resizing or hiding the pane exits copy mode.

Copy/paste uses the outer Wayland clipboard. The frontend permits focused OSC 52
writes and rejects OSC 52 queries. Desktop clipboard access requires an attached
presentation.

## Remote connections

With SSH configured and a compatible `amux` available on the destination PATH:

```sh
amux connect -- ssh -T example-host amux serve-stdio
amux connect --cli --json -- ssh -T example-host amux serve-stdio
```

The transport must provide clean binary stdin/stdout; SSH uses existing
authentication. `connect --json` requires `--cli`. The destination's
`serve-stdio` starts or reuses `/tmp/amux-UID/default.sock`, or uses its explicit
`--socket`. The remote server survives transport exit. Switching connections
detaches the previous session. Transport loss closes presentation; reconnection
is explicit.

## Socket protocol

The current wire protocol is **3**. Requests carry integer `protocol` and an
informational release `version`. The protocol must match exactly; compatible
releases can differ. One UTF-8 JSON object per newline is exchanged over a Unix
socket, with one outstanding manager request per connection. Socket mode is
0600. The identity handshake is:

```json
{"protocol":3,"version":"0.2.0","op":"hello"}
```

The reply contains `ok`, `protocol`, `version` and server `incarnation`.
Replies use `ok`; failures include `error`. State-changing replies generally
include `state`. Unknown fields are rejected. Optional `_trace` is a
`"pid:sequence"` string echoed by replies for tracing.

| Operations | Fields beyond the common envelope |
| --- | --- |
| `hello`, `help`, `list` | None; return identity, command grammar or `sessions` |
| `create` | Required `width`, `height`; optional `name`; creates detached session |
| `attach` | Required `width`, `height`; optional `session`, `events`; returns attachment `key` and `state`; omitted session creates one |
| `detach`, `state`, `zoom` | None; require attachment |
| `destroy` | Optional `session`, `if_empty`, `select_next`; absent session uses attachment; selecting a successor requires destroying the attached session |
| `command` | Required `text`; parses the shared manager grammar |
| `split` | Required `axis`; optional `pane` |
| `focus` | One of `pane` or `direction` (`left`, `right`, `up`, `down`, `next`, `prev`) |
| `move` | Required `direction`; optional `pane` |
| `resize` | Required `axis`, `pixels`; optional `pane` |
| `close`, `pane_rename` | Optional `pane`; rename requires `name` |
| `viewport` | Required `width`, `height` |
| `workspace_new`, `workspace_next`, `workspace_prev`, `workspace_last` | None |
| `workspace_select`, `workspace_close`, `workspace_rename` | `number` required for select, optional otherwise; rename requires `name` |
| `session_new`, `session_select`, `session_last`, `session_rename` | New accepts `name`; select requires `session`; rename requires `name`; last uses the common envelope |
| `join`, `pane_move` | Optional `pane`, `axis`, destination `target`; move requires destination workspace `number` |
| `capture`, `clear_history` | Optional `session`, `pane`; capture also accepts `history`; omitted session uses the attachment |
| `surface_gone` | Required `pane`; frontend reports a lost surface |
| `terminal`, `terminal_size`, `history` | Required attachment `key`, `pane`; size requires `columns`, `rows`; history requires `read` |

Layout commands require attachment. Session switching returns its new key and
state, expires the old key and preserves the previous session detached.
`destroy` with `if_empty:true` rejects sessions with panes; `select_next:true`
selects the previous detached session if available, otherwise the newest one,
or closes the connection when successor selection is exhausted. `detach` returns `detached`;
`destroy` returns `destroyed`.

State contains `session`, `name`, `attached`, `viewport`, `active_workspace` and
`workspaces`. Workspaces include stable `workspace` IDs, local `number`, name,
viewport, focus, `zoomed`, split `layout` and `panes`. Pane records contain
`id`, `name`, `rect` (`x`, `y`, `width`, `height`), `visible` and `terminal.pid`.
Split ratios are
`[numerator, denominator]`. Visibility is relative to the workspace; the
frontend also hides inactive workspaces. With `events:true`, unsolicited
`{"event":"state","state":...}` reports shell exits. Destruction can send
`{"event":"destroyed","session":ID}` before disconnecting.

The frontend's connection-control socket additionally accepts `connections`
and `connection_select` with a required `connection` index. It uses the same
envelope and returns connection profiles or the selected endpoint's sessions.
`connection_lost` events return the frontend to connection selection. This is
a frontend-local interface, separate from the server socket.

### Terminal and history streams

A separate unused connection sends `terminal` with the attachment key and pane.
After its successful JSON reply, server output uses a one-byte kind followed
by a four-byte unsigned big-endian length and payload:

| Kind | Payload |
| --- | --- |
| `C` | Native terminal-state JSON checkpoint |
| `U` | Four-byte descriptor length, JSON history descriptor, then PTY bytes |
| `P` | Empty; revokes presentation and copy view |

Client input is unframed terminal bytes. One relay connects per pane. Hidden
panes pause output; changed panes receive a fresh checkpoint before resuming.
`terminal_size` updates PTY cell dimensions; `viewport` controls logical geometry.
Keys expire on detach. Checkpoints require matching pinned Alacritty/VTE sources.
Use `capture` to obtain plain terminal text.

History requests use a separate read-only connection. `read.kind` is `rows` or
`marker`. Rows require `epoch`, `start`, `end` and `projection` (`text` or
`cells`), with optional `whole:[start,end]` to validate a larger interval.
Marker reads require `epoch`, `bound`, `start`, `end`, `origin:[row,column]`,
`direction` (-1 or 1) and numeric `marker` (65–68 for OSC 133 A–D).
Descriptors contain `epoch`, `sequence`,
`oldest`, `next`, `columns`, `lines` and `alternate`. Intervals are end-exclusive.
Replies include the server incarnation and descriptor, plus rows or a marker
position. Epoch changes and evicted ranges reject stale reads. A page permits
at most 128 rows, 32,768 cells and 512 KiB encoded output.

### Binary stdio and native-state components

The stdio transport frames each stream with a one-byte kind, four-byte
big-endian stream ID, four-byte big-endian payload length and payload.
`O` opens with class 0 (manager), 1 (terminal) or 2 (history/capture); `A`
acknowledges; `D` carries up to 32 KiB; `E` half-closes; `C` aborts; `W`
returns one frame credit. Each direction permits eight outstanding data frames
per stream. Before binary frames, `serve-stdio` emits one newline-terminated
JSON greeting with `protocol`, `version` and `incarnation`. Subsequent output
uses binary framing exclusively.

`amux-terminal-state` uses a one-byte command and four-byte big-endian payload
length on stdin: `D` feeds PTY bytes, `R` resizes with JSON `[columns,rows]`,
`C` captures full state, `L` captures live state, `H` reads history, `T` captures
plain text with a JSON history-row count, `X` clears history, and `I` restores
a checkpoint. Replies contain a length-prefixed terminal response, a
length-prefixed state/result, a four-byte remaining synchronized-output timeout
in milliseconds, and a length-prefixed JSON history descriptor. Empty sections
have zero length. Component and transport formats share the Amux protocol
version and require matching packaged implementations.
