import argparse
import asyncio
import json
import os
import sys

from . import __version__

def main():
    parser = argparse.ArgumentParser(prog="amux", description="Amux server and clients; no command opens the GUI")
    parser.add_argument("--version", action="version", version=f"amux {__version__}")
    commands = parser.add_subparsers(dest="command", required=True)
    pruning = commands.add_parser("prune", help="stop the default server and remove its socket; ends its sessions")
    pruning.add_argument("--socket", help="explicit socket to prune (default: $XDG_RUNTIME_DIR/amux/default.sock)")
    for name in ("server", "client", "list", "gui", "create", "destroy"):
        command = commands.add_parser(name)
        command.add_argument("--socket", help="server socket (default: $XDG_RUNTIME_DIR/amux/default.sock)")
        command.add_argument("--trace-dir", help="write opt-in performance events to this directory")
        if name == "server":
            command.add_argument("--shell", nargs="+", help="override the configured shell command")
            command.add_argument("--editor", nargs="+", help="override the configured Neovim command")
            command.add_argument("--config", help="server config path (default: $XDG_CONFIG_HOME/amux/server.toml)")
        if name == "create":
            command.add_argument("--name", help="initial session name")
        if name in ("client", "list", "create", "destroy"):
            command.add_argument("--json", action="store_true")
        if name in ("client", "gui", "create"):
            command.add_argument("--width", type=int, default=1200)
            command.add_argument("--height", type=int, default=800)
        if name in ("client", "gui", "destroy"):
            command.add_argument("--session", type=int, required=name == "destroy")
        if name == "gui":
            command.add_argument("--config", help="client config path (default: $XDG_CONFIG_HOME/amux/client.toml)")
            command.add_argument("--server-config", help="server config path for a newly started local server")
            command.add_argument("--foreground", action="store_true", help="wait for the graphical frontend to exit")
            command.add_argument("--pick-connection", action="store_true", help="open the connection picker without attaching")
    for name in ("capture", "clear-history"):
        command = commands.add_parser(name)
        command.add_argument("--socket", help="server socket (default: $XDG_RUNTIME_DIR/amux/default.sock)")
        command.add_argument("--session", type=int, required=True)
        command.add_argument("--pane", type=int, help="pane ID (default: active window's focused pane)")
        command.add_argument("--json", action="store_true")
        if name == "capture":
            command.add_argument("--history", type=int, default=0, help="retained rows before the screen, at most 10000")
    terminal = commands.add_parser("relay")
    terminal.add_argument("--socket", required=True)
    terminal.add_argument("--key", required=True)
    terminal.add_argument("--pane", type=int, required=True)
    viewing = commands.add_parser("editor-view")
    viewing.add_argument("--socket", required=True)
    viewing.add_argument("--key", required=True)
    viewing.add_argument("--pane", type=int, required=True)
    reporting = commands.add_parser("trace-report", help="summarize a local trace directory")
    reporting.add_argument("directory")
    stdio = commands.add_parser("serve-stdio", help="guest mux transport over binary stdin/stdout")
    stdio.add_argument("--socket", help="explicit guest mux socket; otherwise private /tmp runtime")
    stdio.add_argument("--config", help="server config path on the execution machine")
    stdio.add_argument("--trace-dir", help="trace this connection and a newly started guest server")
    remote = commands.add_parser("connect", help="attach through a stdio transport command")
    remote.add_argument("--session", type=int)
    remote.add_argument("--cli", action="store_true", help="headless CLI attachment")
    remote.add_argument("--json", action="store_true", help="JSON replies for --cli")
    remote.add_argument("--config", help="local client config path")
    remote.add_argument("--server-config", help="server config path for a newly started local connection")
    remote.add_argument("--foreground", action="store_true", help="wait for the graphical frontend to exit")
    remote.add_argument("--trace-dir", help="write local performance events to this directory")
    remote.add_argument("transport", nargs=argparse.REMAINDER)
    argv = sys.argv[1:]
    if not argv or (argv[0].startswith("-") and argv[0] not in ("-h", "--help", "--version")):
        argv = ["gui", *argv]
    args = parser.parse_args(argv)
    if getattr(args, "trace_dir", None):
        os.environ["AMUX_TRACE_DIR"] = os.path.abspath(args.trace_dir)
    try:
        if args.command in ("serve-stdio", "connect"):
            from . import transport
            if args.command == "serve-stdio":
                asyncio.run(transport.serve_stdio(args.socket, args.config))
                return 0
            def connect(ready=None):
                return asyncio.run(transport.connect(args.transport, args.session, args.cli,
                                                     args.config, args.json, ready=ready, server_config=args.server_config))
            if args.cli or args.foreground:
                return connect()
            from .launcher import launch
            return launch(connect)
        if args.command == "trace-report":
            from .trace import report
            report(args.directory)
            return 0
        default_endpoint = not args.socket
        if default_endpoint:
            from .launcher import default_socket
            args.socket = default_socket()
        if args.command == "prune":
            from .launcher import prune
            prune(args.socket)
            print(f"amux: pruned {args.socket}")
            return 0
        if args.command == "server":
            from . import server
            from .config import load_server
            config = load_server(args.config)
            server.run(args.socket, args.shell or config["shell"], args.editor or config["editor"])
            return 0
        if args.command == "editor-view":
            from . import editor_view
            return editor_view.run(args.socket, args.key, args.pane)
        if args.command == "relay":
            from . import relay
            relay.run(args.socket, args.key, args.pane)
            return 0
        if args.command == "gui":
            from . import gui
            def frontend(ready=None):
                return gui.run(args.socket, args.width, args.height, args.session, args.config,
                               start_server=default_endpoint, ready=ready, pick_connection=args.pick_connection,
                               server_config=args.server_config)
            if args.foreground:
                return frontend()
            from .launcher import launch
            return launch(frontend)
        from . import client
        if args.command in ("capture", "clear-history"):
            message = {"op": args.command.replace("-", "_"), "session": args.session}
            if args.pane is not None:
                message["pane"] = args.pane
            if args.command == "capture":
                message["history"] = args.history
            return client.once(args.socket, message, args.json)
        return client.run(args.socket, getattr(args, "width", None), getattr(args, "height", None),
                          args.json, listing=args.command == "list", session=getattr(args, "session", None),
                          creating=args.command == "create", destroying=args.command == "destroy",
                          name=getattr(args, "name", None))
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
