import json
import socket
import sys

from . import envelope
from .trace import Trace

from .commands import HELP, parse


def request(stream, message):
    stream.write((json.dumps(envelope(message), separators=(",", ":")) + "\n").encode())
    stream.flush()
    frame = stream.readline()
    if not frame:
        raise ConnectionError("server disconnected")
    return json.loads(frame)


def display(reply, as_json):
    if as_json:
        print(json.dumps(reply, separators=(",", ":")), flush=True)
        return
    if not reply["ok"]:
        print(f"error: {reply['error']}", file=sys.stderr)
        return
    if "text" in reply:
        print(reply["text"], end="", flush=True)
    elif "cleared" in reply:
        print(f"cleared history for pane {reply['cleared']}")
    elif "state" in reply:
        state = reply["state"]
        viewport = state["viewport"]
        print(f"session {state['session']} ({state['name']})  {viewport['width']}x{viewport['height']}"
              f"  {'attached' if state['attached'] else 'detached'}")
        for window in state["windows"]:
            active = window["window"] == state["active_window"]
            print(f"{'*' if active else ' '} window {window['number']}"
                  f" ({window['name']}, id {window['window']})  focus {window['focus']}"
                  f"{'  zoomed' if window['zoomed'] else ''}")
            for pane in window["panes"]:
                rect = pane["rect"]
                mark = "*" if pane["id"] == window["focus"] else " "
                print(f"  {mark} pane {pane['id']} ({pane['name']}): {rect['width']}x{rect['height']}"
                      f" at {rect['x']},{rect['y']}{'  hidden' if not pane['visible'] else ''}")
        if not state["windows"]:
            print("no windows; use window new")
    elif "sessions" in reply:
        for state in reply["sessions"]:
            print(f"session {state['session']} ({state['name']}, {'attached' if state['attached'] else 'detached'}): {state['window_count']} windows")
        if not reply["sessions"]:
            print("no sessions")
    elif "destroyed" in reply:
        print(f"destroyed session {reply['destroyed']}")
    else:
        print(f"detached session {reply['detached']}")


def once(path, message, as_json=False):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.connect(path)
        with connection.makefile("rwb") as stream:
            reply = request(stream, message)
            display(reply, as_json)
            return 0 if reply["ok"] else 1


def run(path, width, height, as_json=False, listing=False, session=None, creating=False, destroying=False, name=None):
    trace = Trace("cli")
    def call(stream, message):
        identity = trace.request_id()
        if identity:
            message["_trace"] = identity
        trace.emit("request_send", trace_id=identity, op=message["op"])
        reply = request(stream, message)
        if identity and reply.get("_trace") != identity:
            raise ConnectionError("trace reply identifier mismatch")
        trace.emit("reply_received", trace_id=identity, ok=reply["ok"])
        return reply
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.connect(path)
        with connection.makefile("rwb") as stream:
            if listing or creating or destroying:
                message = {"op": "list"} if listing else ({"op": "create", "width": width, "height": height}
                            if creating else {"op": "destroy", "session": session})
                if creating and name is not None:
                    message["name"] = name
                reply = call(stream, message)
                display(reply, as_json)
                return 0 if reply["ok"] else 1
            reply = call(stream, {"op": "attach", "width": width, "height": height,
                                  **({"session": session} if session is not None else {})})
            display(reply, as_json)
            if not reply["ok"]:
                return 1
            failed = False
            interactive = sys.stdin.isatty()
            if interactive and not as_json:
                print(HELP)
            while True:
                try:
                    line = input("amux> " if interactive and not as_json else "")
                except EOFError:
                    break
                try:
                    message = parse(line)
                except ValueError as error:
                    print(f"error: {error}", file=sys.stderr)
                    failed = True
                    continue
                if message is None:
                    continue
                if message["op"] == "help":
                    print(HELP, file=sys.stderr if as_json else sys.stdout)
                    continue
                reply = call(stream, message)
                display(reply, as_json)
                failed |= not reply["ok"]
                if message["op"] in ("detach", "destroy") and reply["ok"]:
                    break
            return int(failed)
