# Public interfaces

Amux manages sessions, windows and terminal panes. The server owns shells,
PTYs, terminal state, layout and focus. A session permits one active manager
attachment. Detach preserves its shells and state; server shutdown ends every
session. Closing the last pane closes its window. An empty session can create
a new window.

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
| `gui` | Attach the Wayland frontend; `--session ID`, `--width PX`, `--height PX`, `--config PATH`, `--server-config PATH`, `--foreground`, `--pick-connection` |
| `server` | Run a foreground server; `--shell COMMAND [ARG...]`, `--config PATH` |
| `client` | Attach a line-oriented management CLI; `--session ID`, `--width PX`, `--height PX`, `--json` |
| `list` | List sessions; `--json` |
| `create` | Create a detached session; `--name NAME`, `--width PX`, `--height PX`, `--json` |
| `destroy` | End a session and its shells; required `--session ID`, optional `--json` |
| `capture` | Write terminal text; required `--session ID`, optional `--pane ID`, `--history N`, `--json` |
| `clear-history` | Clear primary-screen scrollback; required `--session ID`, optional `--pane ID`, `--json` |
| `prune` | Stop the selected server and remove its socket; ends all its sessions |
| `connect` | Attach through a transport command; `--session ID`, `--cli`, `--json`, `--config PATH`, `--server-config PATH`, `--foreground`, `--trace-dir DIR`, then `-- COMMAND [ARG...]` |
| `serve-stdio` | Serve the binary stdio transport; `--socket PATH`, `--config PATH`, `--trace-dir DIR` |
| `route-stdio` | Host-side socket discovery and selection followed by binary stdio transport |
| `relay` | Terminal component; required `--socket PATH`, `--key KEY`, `--pane ID` |
| `trace-report DIR` | Summarize recorded performance events |

`gui`, `server`, `client`, `list`, `create` and `destroy` accept `--socket PATH`
and `--trace-dir DIR`. `capture`, `clear-history` and `prune` accept `--socket`.
`list` and `capture` operate on detached or attached sessions.
Initial viewport defaults are 1200 × 800 pixels.
`capture` defaults to zero historical rows and permits at most 10,000; its
`--pane` and `clear-history`'s `--pane` default to the focused pane in the active
window. Capture preserves soft wraps and Unicode, removes escape sequences,
and has separate 8 MiB cell and text limits. On the alternate screen it captures
that screen only.

The default endpoint is `$XDG_RUNTIME_DIR/amux/protocol-13.sock`. Each protocol
version uses its own `protocol-N.sock` default, including a separate startup lock
and log. Different server versions can run concurrently on separate sockets,
each owning its sessions and application processes. An explicit `--socket PATH`
selects that exact endpoint and requires a matching client protocol.
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
uses the same grammar. IDs select sessions or panes; window numbers are local
to a session, start at 1 and remain consecutive after a window closes.
Window IDs and pane IDs remain stable across renumbering; custom window names
are preserved, and default names track the new numbers.

```text
state | zoom | split h|v [pane] | close [pane]
focus <pane|left|right|up|down|next|prev> | resize h|v <pixels> [pane]
move left|right|up|down [pane]
move window <number> [h|v] [pane] | join [pane] [h|v]
window new|next|prev|last|select <number>|close [number]
session new [name]|last|select <id> | rename session|window|pane <name>
clear-history [pane] | capture [pane] [history-rows]
viewport <width> <height> | detach | destroy | help
```

Quote names containing spaces, such as `rename pane "build logs"`. Split axis
`h` divides width into left/right panes; `v` divides height into top/bottom panes.
Resize steps are pixels. Splits and new windows inherit the focused or
targeted shell's working directory; initial panes use the server's directory.

## Configuration and environment

User files are `client.toml` and `server.toml` under `$XDG_CONFIG_HOME/amux`,
defaulting to `~/.config/amux`. Missing default files use bundled defaults.
Explicitly selected missing files, invalid values and settings owned by the
other process fail. `--config PATH` selects client configuration on `gui` and
graphical `connect`, and server configuration on `server` and `serve-stdio`.
`--server-config PATH` on `gui` and graphical `connect` selects configuration
for newly started local connections; remote servers read their own files.
`connect --cli` rejects both config options.

| File | Setting | Default and meaning |
| --- | --- | --- |
| `server.toml` | `shell` | `["sh"]`; nonempty executable/argument array; Amux appends `-i` |
| `client.toml` | `prefix` | `"Ctrl+a"`; manager prefix key |
| `client.toml` | `repeat_rate` | `25`; repeats per second, nonnegative integer |
| `client.toml` | `repeat_delay` | `300`; milliseconds before repeating, nonnegative integer |
| `client.toml` | `notify` | `[]`; executable/argument array; Amux appends a notification title and body; empty disables invocation |
| `client.toml` | `connections` | `[]`; named local sockets or transport argument arrays |
| `client.toml` | `ssh` | `["ssh"]`; executable/argument array used by the SSH connection prompt |
| `client.toml` | `bindings.normal`, `bindings.prefix`, `bindings.resize` | Tables mapping key combinations to commands; supplied entries override defaults; an empty string disables a binding |

The execution machine supplies the shell.
`server --shell` overrides the configured command. Reopen the GUI after changing
client settings; start a fresh server after changing server settings.

In `server.toml`:

```toml
shell = ["bash"]
```

In `client.toml`:

```toml
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
`ui windows|sessions|connections|arrange|command|session-name|window-name|pane-name|help`.
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
| `c`, `%`, `"` | New window, horizontal split, vertical split |
| Arrows, `o` | Directional focus, next pane |
| `j`, `x`, `&` | Join a pane from the previous window, close pane, close window |
| `1`–`9` | Select window number |
| `<`, `>`; `+`, `=`; `-`, `_` | Shrink/grow width; shrink height; grow height by 8 pixels |
| Alt-arrows, Ctrl-arrows | Resize by 5 or 1 pixel |
| `w`, `s`, `S` | Window picker, workspace buffer, new session |
| `e`, `C` | Connection buffer |
| `:`, `,`, `$`, `P`, `?` | Command prompt, rename window/session/pane, help |
| `d`, Escape, `Ctrl-g` | Detach, leave prefix/resize mode |
| Prefix again | Send the prefix key to the terminal |

Unprefixed Alt-h/j/k/l changes focus, Alt-H/L switches windows,
Alt-s toggles zoom, Alt-t toggles the workspace buffer, and Alt-y/Y selects the most recently
used other window/session, starting from the neighbouring one after attaching. Alt-J/K sends down/up arrow to the terminal; Alt-v replaces the
focused pane with Neovim and opens its Neovide view. In an empty session,
unprefixed `q` destroys it and selects a detached session if available.
Close operations terminate their shells.

Manager panels use Escape to leave insert/filter mode or cancel a pending
operator. In normal mode, `q` closes the panel.

Alt-t or prefix `s` opens the workspace buffer with a fuzzy filter. Session
headers contain their window rows with stable IDs. Type to match session/window
names or IDs; window matches include the owning session's name. Arrows select a
result. Enter opens a session or a specific window, including across sessions.
Ctrl-n creates a session; Ctrl-o opens connection selection.

Escape enters navigation/edit mode; `j/k` and arrows select a row, `G` selects
the last row, and `/` returns to the fuzzy filter. Ctrl-e in the window picker
opens this same buffer in navigation/edit mode. Ctrl-e clears the
filter while keeping the selected row. `m` removes a window into the single paste
register; `p/P` places it after/before a window, or first under a selected session
header. A second `m`
requires placing the held window first. `dd` stages a deletion and preserves
the register; `u` undoes the last edit. An unpasted held window is also deleted
on confirmation. Names edit inline: `i/a` inserts before/after the name cursor, `I` inserts at the start, and
`A` appends at the end. `h/l`, `0/$` and `w/b/e` move within the name; `cw/dw`,
`x` and `C/D` change or delete text. Editing clears the filter and displays the
full tree. Escape returns to row navigation, preserving the edit; Enter stages
the name. Ctrl-s finishes an active name edit and opens the change review.
`o` opens a line immediately below the selected row;
`O` opens one immediately above. Both enter insert mode in the buffer. A line
above a session header starts as a session; other new lines start as windows.
Tab indents a new line as a window; Shift-Tab outdents it as a session header.
A window belongs to the session header above it; a new session header starts a
group containing the following window rows. The first row is a session header.
Escape leaves insertion and returns to navigation, preserving the new line and
its text, including an empty line. Enter accepts a nonempty name. Ctrl-g cancels
the provisional row while inserting; `u` undoes the last edit.
A new session with an empty window list gets an initial shell window when applied.
Added or moved windows supply the new session's window list.

`=` in navigation mode, or Ctrl-s, reviews staged changes in the workspace tree.
Enter or `y` applies the reviewed draft together
and keeps the buffer open; Escape or `n` returns to editing with the draft intact.
Additions are green; deletions are red; moves and their connecting arrows are
blue; renames are amber and show the old and new names inline. A moved and renamed row combines
the blue route with the amber name change. Source rows remain at their old
locations. Only affected sessions are shown; consecutive runs of two or more
unchanged windows collapse into `…` rows. Single unchanged windows provide
context. Reorders use the same move arrows.
`j/k` or arrows scroll the review; Tab jumps between a move's endpoints.
Ctrl-r reloads the current arrangement.
Enter requires a saved draft before switching. Immediate
session creation with Ctrl-n or changing connections also requires a saved draft.
In navigation/edit mode, Escape cancels a pending operator and keeps the buffer
open. `q` discards the draft and closes the UI. Ctrl-g or Alt-t cancels the draft;
the unattached startup picker returns to selection. Applying requires a name on
every row. A connected client can edit the
server arrangement before attaching; Enter attaches after the draft is saved.
On an empty server, `o` or `O` starts the first session header.

Each retained window must occur exactly once when applying. Confirming a
deletion closes every pane application in that window; an emptied session remains
available. The server validates all rows and destination viewport sizes
before applying the changes; a changed session/window arrangement requires a
reload. Moving a window preserves its pane identities, processes, layout and
terminal history. Other attached frontends receive the updated state. Surviving
active windows retain focus; an emptied session remains available.

Alt-u toggles copy mode. Escape or `q` exits; vi motions, counts, PageUp/PageDown
and Ctrl-u/d/b/f navigate. `v` selects, Ctrl-v selects a rectangle, `y` copies
and exits, `Y` copies and stays, and `L` copies the logical line and exits.
`/` and `?` search; Enter accepts; `n`/`N` repeats. `f/F/t/T` searches a character,
`;`/`,` repeats/reverses, and `i/a` with word, quote or bracket objects selects
across soft wraps. `p/P` selects previous/next prompt positions; `c` follows
prompt/output markers. Ctrl-k opens the hyperlink under the copy cursor using
the configured link-opening command (`xdg-open` by default). The frontend launches
the command in the host desktop environment and supplies a Wayland activation
token when the host exposes `xdg_activation_v1`. The host desktop controls focus.
OSC 8 links and detected URLs are supported; copy mode stays active.
The top-right position indicator counts rows backward
from the bottom of the frozen screen across retained history, including rows
available to load. Resizing or hiding the pane exits copy mode.

Copy/paste uses the outer Wayland clipboard. The frontend permits focused OSC 52
writes and rejects OSC 52 queries. Desktop clipboard access requires an attached
presentation.

## Remote connections

Open the connection buffer with Ctrl-a e (Ctrl-a C also works), or launch `amux --pick-connection`.
Type a fuzzy filter or an endpoint: host, `user@host`, `ssh://user@host:port`,
absolute socket path or `unix:///path/to/socket`. The list includes local sockets,
saved endpoints, recent SSH connections and literal Host aliases from
`~/.ssh/config`, including files matched by literal or glob Include paths.
Exact and contiguous matches rank ahead of subsequence matches. Enter connects
the selected suggestion or “Connect to” row; Ctrl-Enter connects the typed
endpoint literally. Ctrl-n opens the dedicated SSH prompt. Selecting an SSH host
opens its socket picker; “Local sockets” browses this machine. Saved local socket
rows connect directly. GUI SSH runs `amux route-stdio` from the destination PATH.
Custom transport commands use `amux connect -- COMMAND` on the CLI. Selecting a
socket opens that server's workspace buffer. Alt-t searches and edits workspaces
within that server. Ctrl-a e returns to connection selection; Ctrl-o from
connections returns to the current server's workspaces.

Socket suggestions are direct entries in user-owned `$XDG_RUNTIME_DIR/amux` and
`/tmp/amux-UID` directories. Discovery reads directory entries; filename protocol
numbers are hints, and attachment verifies the actual protocol. “Default server”
starts the current protocol's server if absent. Type an absolute path, `~/...` or
`unix:///...` for another socket; Ctrl-Enter uses the typed path literally. Remote
paths are expanded on the remote host. Custom socket paths require an existing,
compatible listening server. Create one with
`amux server --socket /path/to/name.sock` on the selected machine. Saving a socket
bookmark records its address; “Default server” is the GUI's server creation entry
point. Enter opens, Ctrl-r refreshes and Ctrl-o
returns to machine selection. Escape enters navigation mode; `q` closes the
picker. The host's SSH key is shown beside the filter. Authentication, discovery,
refresh and socket attachment share one SSH process. Discovery and failed
selections preserve the current attachment.

Escape enters navigation/edit mode; `/` returns to filtering. `o/O` adds a row
below/above in insert mode; Tab chooses SSH or socket while adding. Escape
preserves the new row and text; Enter stages it; Ctrl-g cancels the active edit.
In navigation mode, Tab/Shift-Tab chooses endpoint, label or SSH key.
`i/a/I/A` opens the key picker for the key field and edits other fields inline;
text motions and operators use the workspace
editor. Editing a suggested SSH destination or configured socket stages a saved
copy. Configured transport commands remain selectable.
`dd` forgets a saved bookmark; `u` undoes the last edit. `=` or Ctrl-s reviews
changes: additions are green, removals red and edits amber. Enter or `y` saves
the reviewed list and stays in the buffer; Escape or `n` returns to editing.
Connecting requires a saved draft. `q` discards it and closes the buffer;
Ctrl-r reloads. Every saved row requires a label and endpoint.

Bookmarks are stored in `$XDG_STATE_HOME/amux/connections.json`, normally
`~/.local/state/amux/connections.json`. Updates are atomic; a changed saved
snapshot requires reloading before applying. Forgetting a bookmark keeps an
active connection and its server running. Each server owns its own sessions,
windows and panes; selecting a connection changes the server the client presents.

Ctrl-k on an SSH row opens its fuzzy key picker; `K` also works in navigation
mode. Suggestions include standard `id_*` files and identities with `.pub`
siblings in `~/.ssh`. Public-key-only files can select agent-held identities.
Type an absolute path to select another identity; Ctrl-Enter uses it literally.
Enter stages the association;
`=` and Enter save it with the local connection bookmark. “Use SSH defaults”
clears the association. Ctrl-g cancels; Escape enters navigation mode and `q`
returns to connections. Suggestions inspect filenames; OpenSSH reads the keys.
An explicit identity adds `-i`, `IdentitiesOnly=yes` and
`PreferredAuthentications=publickey` to the SSH command.

Successful connections save the last 100 distinct destination and key
combinations in `$XDG_STATE_HOME/amux/ssh-history.json`, normally
`~/.local/state/amux/ssh-history.json`, most recent first. Suggestions refresh
each time the picker opens. Suggestion source errors appear as warnings, and
typed destinations remain usable. A history write error leaves the connection
active and displays a warning. Opened endpoints also remain in the connection
list for the lifetime of this GUI.

The prompt invokes the `client.toml` SSH command with `-T`, the destination and
the remote command `exec amux route-stdio`. The command
runs in Amux's launch environment. SSH owns authentication configuration, agent
selection and host-key prompts. GUI SSH processes use external askpass for input
with `SSH_ASKPASS_REQUIRE=force`; an inherited `never` policy remains in effect.
Set `SSH_ASKPASS` in the desktop environment to the external helper executable.
The remote shell must accept the quoted command.
Connection failures appear in the prompt and preserve the current attachment.

Remote sessions trust their host and server with all input delivered to their
panes. SSH encrypts network traffic and verifies host identity; the remote host
and session endpoint receive plaintext. Authentication through the local SSH
connection picker and a host-desktop askpass dialog stays on the local machine
until OpenSSH performs the requested authentication. Passwords typed inside a
remote pane are delivered to that pane's host, including passwords for another
machine. See [session trust](https://man.openbsd.org/ssh#AUTHENTICATION).

With SSH configured and a compatible `amux` available on the destination PATH:

```sh
amux connect -- ssh -T example-host amux serve-stdio
amux connect --cli --json -- ssh -T example-host amux serve-stdio
```

For an explicit remote executable path, supply the transport command on the CLI:

```sh
amux connect -- ssh -T example-host /opt/amux/bin/amux serve-stdio
```

The transport must provide clean binary stdin/stdout; SSH uses existing
authentication. `connect --json` requires `--cli`. The destination's
`serve-stdio` starts or reuses `/tmp/amux-UID/protocol-13.sock`, or uses its explicit
`--socket`. The remote server survives transport exit. Switching connections
detaches the previous session. Transport loss closes presentation; reconnection
is explicit.

## Socket protocol

The current wire protocol is **13**. Requests carry integer `protocol` and an
informational release `version`. The protocol must match exactly; compatible
releases can differ. One UTF-8 JSON object per newline is exchanged over a Unix
socket, with one outstanding manager request per connection. Socket mode is
0600. The identity handshake is:

```json
{"protocol":13,"version":"0.3.0","op":"hello"}
```

The reply contains `ok`, `protocol`, `version` and server `incarnation`.
Replies use `ok`; failures include `error`. State-changing replies generally
include `state`. Unknown fields are rejected. Optional `_trace` is a
`"pid:sequence"` string echoed by replies for tracing.

Every wire contract change increments the protocol version, including added
operations or fields and changes to terminal/history data, native checkpoints or
stdio framing. Each server speaks one protocol; clients require an exact match
before attaching. Keep a matching client build available to reattach to an older
server. Stopping a server ends its sessions; other server instances keep running.

| Operations | Fields beyond the common envelope |
| --- | --- |
| `hello`, `help`, `list` | None; return identity, command grammar or `sessions` |
| `arrangement` | None; returns the current session/window `arrangement` |
| `arrangement_apply` | Required `base`, `arrangement`; optional paired `width`, `height`, required for new sessions; applies a complete staged arrangement |
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
| `window_new`, `window_next`, `window_prev`, `window_last` | None |
| `window_select`, `window_close`, `window_rename` | `number` required for select, optional otherwise; rename requires `name` |
| `session_new`, `session_select`, `session_last`, `session_rename` | New accepts `name`; select requires `session`; rename requires `name`; last uses the common envelope |
| `join`, `pane_move` | Optional `pane`, `axis`, destination `target`; move requires destination window `number` |
| `capture`, `clear_history` | Optional `session`, `pane`; capture also accepts `history`; omitted session uses the attachment |
| `surface_gone` | Required `pane`; frontend reports a lost surface |
| `terminal`, `terminal_size`, `history` | Required attachment `key`, `pane`; size requires `columns`, `rows`; history requires `read` |

Layout commands require attachment. Session switching returns its new key and
state, expires the old key and preserves the previous session detached.
`destroy` with `if_empty:true` rejects sessions with panes; `select_next:true`
selects the previous detached session if available, otherwise the newest one,
or closes the connection when successor selection is exhausted. `detach` returns `detached`;
`destroy` returns `destroyed`.

An arrangement is an array of `{session, name, windows}` objects. Each `windows`
array contains `{window, name}` objects using stable window IDs. `base` is the
unchanged array returned by `arrangement`; `arrangement` is its edited form.
Every current session must occur exactly once. Retained windows occur once;
omitted windows are deleted and their pane applications closed after validation
and preparation succeed. Negative session or window IDs request creation; each temporary ID occurs once within its type.
New rows require printable, nonempty names. The server assigns positive IDs on
success. Session array order determines picker order; window order within a
session determines consecutive local numbers. New sessions use the request's
positive integer `width` and `height`; supply both dimensions when creating a
session. Existing sessions retain their viewports. A new session gets an initial
shell window when its window list is empty.
Applying validates the entire draft and creates new terminals before changing
the live arrangement. Failed terminal creation leaves existing names, windows
and processes intact and retires the staged terminals. The reply returns the new
`arrangement`, plus `state` for an attached caller. Applying preserves attachment
ownership and the caller's session selection; an unattached caller stays
unattached. Other attached clients subscribed to events receive updated state.

State contains `session`, `name`, `attached`, `viewport`, `active_window` and
`windows`. Windows include stable `window` IDs, local `number`, name,
viewport, focus, `zoomed`, split `layout` and `panes`. Pane records contain
`id`, `name`, `rect` (`x`, `y`, `width`, `height`), `visible` and `terminal.pid`.
Split ratios are
`[numerator, denominator]`. Visibility is relative to the window; the
frontend also hides inactive windows. With `events:true`, unsolicited
`{"event":"state","state":...}` reports shell exits and pane attention changes.
Pane snapshots include an `attention` boolean, cleared by focusing that pane.
BEL, OSC 9 (excluding OSC 9;4 progress), OSC 777 `notify;title;body` and OSC 99
notification text produce
`{"event":"attention","pane":ID,"bell":true,"notifications":[["title","body"]]}`;
`bell` indicates BEL and `notifications` contains the parsed text pairs.
The GUI invokes its configured `notify` command for unviewed panes, at most once
per second; intervening events are dropped. Destruction can send
`{"event":"destroyed","session":ID}` before disconnecting.

The frontend's connection-control socket additionally accepts these operations:

- `connections` returns `connections`, `connection_bookmarks` and `ssh_keys` arrays. Roster
  rows contain `connection`, `name`, `kind`, `address` and `source`.
  Sources include `local`, `config`, `opened`, `saved`, `recent` and `alias`.
  `active` identifies the selected stored or configured endpoint.
  SSH rows may carry an absolute `identity` path. Key suggestions contain `name`
  and `identity`; an empty identity uses OpenSSH's defaults.
  The `machine` row `local-sockets` opens local socket discovery.
- `connection_select` requires a `connection` value from that roster. Configured
  and opened connections use integer indices; saved bookmarks and suggestions
  use strings local to the client connection catalog.
- `connections_apply` requires `base` and `bookmarks` arrays. Every bookmark has
  `connection`, `name`, `kind` and `address`, with an optional absolute
  `identity` path for SSH. Existing bookmark
  IDs are stable strings; negative integer IDs create bookmarks and receive
  assigned string IDs on success; `connection_ids` maps their temporary ID
  strings to the assigned IDs. Omitted bookmarks are forgotten. Kinds are
  `ssh` or `socket`; socket bookmarks use absolute paths.
  The saved snapshot must match `base`. This updates the local list and returns
  both arrays; the active transport and server state remain available.
- `connection_socket` requires a `socket` path and connects to that endpoint.
- `connection_ssh` requires a `destination` string and accepts an
  optional absolute `identity` path. It connects directly to the remote default server.
- `connection_browse` accepts a roster `connection`, or a `destination` and
  optional `identity`. Omit both to browse local sockets. It returns `sockets`,
  `route_name` and `route_identity` while preserving the active attachment.
  Socket rows have `socket`, `path`, `name` and `source`, plus an optional
  `protocol_hint`. The default row's `socket` is empty.
- `route_sockets` refreshes the pending machine's socket list.
- `route_select` requires a `socket` string; an empty string selects the default
  server. Success returns the selected endpoint's sessions.
- `route_cancel` closes pending discovery and returns `route_cancelled: true`;
  the active attachment remains available.
- `ssh_targets` returns recent destinations and SSH aliases in a `ssh_targets`
  array with `destination`, `name` and `source` (`recent` or `alias`),
  plus `identity` when a recent connection used an explicit key.

An optional `warning` reports suggestion or storage errors. These operations
use the same envelope and return the connection catalog or the selected
endpoint's sessions. `connection_lost` returns the frontend to connection
selection. This interface belongs to the frontend's local connection controller.

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
