import asyncio
import copy
import itertools
import json
import os
from pathlib import Path
import signal
import secrets
import socket
import sys
import time

from . import PROTOCOL_VERSION, __version__
from .model import CommandError, Session, integer, label
from .commands import HELP, parse
from .terminal import Terminal
from .trace import Trace, take_id


class SessionRuntime:
    def __init__(self, session, retire):
        self.session = session
        self.writer = None
        self.events = False
        self.key = None
        self.retire = retire
        self.terminals = {}
        self.history_clients = set()

    def roster(self):
        return {"session": self.session.identity, "name": self.session.name,
                "attached": self.writer is not None,
                "workspace_count": len(self.session.workspaces)}

    def snapshot(self):
        state = self.session.snapshot()
        state["attached"] = self.writer is not None
        for workspace in state["workspaces"]:
            for pane in workspace["panes"]:
                pane["terminal"] = {"pid": self.terminals[pane["id"]].pid}
        return state

    def notify(self):
        if self.events and self.writer is not None and not self.writer.is_closing():
            send(self.writer, {"event": "state", "state": self.snapshot()})

    def present(self, trace_id=None):
        current = self.session.current
        visible = {p["id"] for p in current.snapshot()["panes"] if p["visible"]} if current else set()
        for pane, terminal in self.terminals.items():
            terminal.set_presented(self.writer is not None and pane in visible, trace_id)

    def close(self):
        for terminal in self.terminals.values():
            self.retire(terminal)
        self.terminals.clear()


def send(writer, message):
    writer.write((json.dumps(message, separators=(",", ":")) + "\n").encode())


async def serve(path, shell):
    trace = Trace("server")
    incarnation = secrets.token_hex(16)
    sessions = {}
    attachments = {}
    children = {}
    session_ids = itertools.count(1)
    workspace_ids = itertools.count(1)
    pane_ids = itertools.count(1)
    clients = set()
    closing = set()

    def retire(terminal):
        task = terminal.close()
        closing.add(task)
        task.add_done_callback(closing.discard)

    def release(owner, key):
        if owner.key != key:
            return
        attachments.pop(key, None)
        owner.key = None
        owner.writer = None
        owner.events = False
        owner.present()
        for task in list(owner.history_clients):
            task.cancel()
        for terminal in owner.terminals.values():
            terminal.disconnect()

    def bind(owner, writer, events):
        owner.writer, owner.events = writer, events
        owner.key = secrets.token_urlsafe(24)
        attachments[owner.key] = owner
        owner.present()
        return owner.key

    def create(width, height, trace_id, name=None):
        name = label(name) if name is not None else None
        session = Session(next(session_ids), workspace_ids.__next__, pane_ids.__next__, width, height)
        if name is not None:
            session.name = name
        owner = SessionRuntime(session, retire)
        try:
            materialize(owner, trace_id)
        except OSError:
            owner.close()
            raise
        sessions[session.identity] = owner
        return owner

    def drop_pane(attachment, pane):
        if pane not in attachment.terminals:
            return
        workspace = attachment.session.workspace_for(pane)
        attachment.session.apply_workspace(workspace, {"op": "close", "pane": pane})
        retire(attachment.terminals.pop(pane))
        trace.emit("terminal_removed", workspace=workspace.identity, pane=pane)
        attachment.present()
        attachment.notify()

    def reap():
        while True:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return
            if not pid:
                return
            owned = children.pop(pid, None)
            if owned is not None:
                attachment, pane = owned
                terminal = attachment.terminals.get(pane)
                if terminal is not None:
                    terminal.running = False
                drop_pane(attachment, pane)

    def materialize(attachment, trace_id=None, cwd=None):
        wanted = {node.pane: workspace for workspace in attachment.session.workspaces.values()
                  for node, _ in workspace.leaves()}
        for pane in wanted.keys() - attachment.terminals.keys():
            trace.emit("terminal_create_begin", workspace=wanted[pane].identity,
                       pane=pane, trace_id=trace_id)
            terminal = Terminal(shell, lambda pane=pane: drop_pane(attachment, pane), trace, pane, cwd)
            attachment.terminals[pane] = terminal
            children[terminal.pid] = attachment, pane
            trace.emit("terminal_create_end", workspace=wanted[pane].identity,
                       pane=pane, child_pid=terminal.pid, trace_id=trace_id)
        for pane in attachment.terminals.keys() - wanted.keys():
            retire(attachment.terminals.pop(pane))

    def apply(attachment, message, trace_id):
        session = attachment.session
        before = session.workspaces.copy(), session.active, session.previous, session.width, session.height
        if session.current is not None:
            saved = copy.copy(session.current)
            saved.root = copy.deepcopy(saved.root)
            saved.names = saved.names.copy()
            before[0][session.active] = saved
        old_panes = set(attachment.terminals)
        cwd = None
        try:
            if message.get("op") in ("split", "workspace_new") and session.current is not None:
                source = message.get("pane", session.current.focus) if message["op"] == "split" else session.current.focus
                session.current.target({"pane": source})
                cwd = attachment.terminals[source].directory()
            session.apply(message)
            materialize(attachment, trace_id, cwd)
        except OSError as error:
            session.workspaces, session.active, session.previous, session.width, session.height = before
            for workspace in session.workspaces.values():
                workspace.width, workspace.height = session.width, session.height
            for pane in attachment.terminals.keys() - old_panes:
                retire(attachment.terminals.pop(pane))
            raise CommandError(f"terminal creation failed: {error}") from error
        finally:
            if cwd is not None:
                os.close(cwd)
        attachment.present(trace_id)
        return attachment.snapshot()

    async def handle(reader, writer):
        task = asyncio.current_task()
        clients.add(task)
        attachment = None
        attachment_key = None
        terminal_attachment = None
        terminal_pane = None
        history_owner = None
        previous_session = None
        try:
            while frame := await reader.readline():
                received_ns = time.monotonic_ns() if trace.enabled else None
                if not frame.endswith(b"\n"):
                    break
                detach = False
                trace_id = None
                operation = None
                try:
                    message = json.loads(frame)
                    if not isinstance(message, dict):
                        raise CommandError("request must be an object")
                    try:
                        trace_id = take_id(message)
                    except ValueError as error:
                        raise CommandError(str(error)) from error
                    version = message.pop("version", None)
                    protocol = message.pop("protocol", None)
                    if type(protocol) is not int or protocol != PROTOCOL_VERSION:
                        raise CommandError(f"protocol mismatch: client {protocol!r} ({version}), server {PROTOCOL_VERSION} ({__version__}); exact protocol match required")
                    operation = message.get("op")
                    if operation == "command":
                        if set(message) != {"op", "text"} or not isinstance(message["text"], str):
                            raise CommandError("command requires text")
                        message = parse(message["text"])
                        if message is None:
                            raise CommandError("empty command")
                        operation = message["op"]
                    trace.emit("request_received", at_ns=received_ns, trace_id=trace_id,
                               op=operation if operation in ("hello", "list", "create", "attach", "detach", "destroy", "state",
                               "split", "focus", "move", "close", "zoom", "resize", "viewport", "surface_gone",
                               "terminal", "terminal_size", "history", "workspace_new", "workspace_next", "workspace_prev",
                               "workspace_select", "workspace_close", "workspace_rename", "session_new",
                               "session_select", "session_last", "workspace_last", "join", "pane_move",
                               "session_rename", "pane_rename", "clear_history", "capture", "help") else "invalid")
                    if history_owner is not None and operation != "history":
                        raise CommandError("history connections are read-only")
                    if operation == "hello":
                        if set(message) != {"op"}:
                            raise CommandError("unexpected hello fields")
                        reply = {"ok": True, "protocol": PROTOCOL_VERSION, "version": __version__, "incarnation": incarnation}
                    elif operation == "help":
                        if set(message) != {"op"}:
                            raise CommandError("unexpected help fields")
                        reply = {"ok": True, "help": HELP}
                    elif operation == "list":
                        if set(message) != {"op"}:
                            raise CommandError("unexpected request fields")
                        reply = {"ok": True, "sessions": [a.roster() for a in sessions.values()]}
                    elif operation in ("terminal", "terminal_size", "history"):
                        expected = {"op", "key", "pane"}
                        if operation == "history":
                            expected |= {"read"}
                        if operation == "terminal_size":
                            expected |= {"columns", "rows"}
                        if set(message) != expected:
                            raise CommandError("unexpected terminal request fields")
                        key = message["key"]
                        if not isinstance(key, str) or key not in attachments:
                            raise CommandError("unknown attachment key")
                        owner = attachments[key]
                        pane = integer(message["pane"], "pane")
                        terminal = owner.terminals.get(pane)
                        if terminal is None:
                            raise CommandError("unknown terminal pane")
                        if operation == "history":
                            if history_owner is not None and history_owner is not owner:
                                raise CommandError("history stream belongs to another attachment")
                            history_owner = owner
                            owner.history_clients.add(task)
                            page = await terminal.history(message["read"])
                            if attachments.get(key) is not owner or owner.terminals.get(pane) is not terminal:
                                raise CommandError("attachment expired")
                            reply = {"ok": True, "incarnation": incarnation, **page}
                        elif operation == "terminal_size":
                            columns = integer(message["columns"], "columns")
                            rows = integer(message["rows"], "rows")
                            if not 1 <= columns <= 65535 or not 1 <= rows <= 65535:
                                raise CommandError("terminal dimensions must fit positive winsize fields")
                            await terminal.resize(columns, rows)
                            trace.emit("terminal_resized", trace_id=trace_id, pane=pane,
                                       columns=columns, rows=rows)
                            reply = {"ok": True}
                        else:
                            if terminal.connection is not None or attachment is not None:
                                raise CommandError("terminal requires a separate unused connection")
                            terminal_attachment, terminal_pane = owner, pane
                            terminal.connection = task
                            reply = {"ok": True}
                            if trace_id is not None:
                                reply["_trace"] = trace_id
                            trace.emit("decision_complete", trace_id=trace_id, op=operation, ok=True)
                            trace.emit("reply_queued", trace_id=trace_id, op=operation)
                            try:
                                await terminal.bridge(reader, writer, reply)
                            except OSError:
                                pass
                            break
                    elif operation == "create":
                        if set(message) - {"op", "width", "height", "name"} or not {"width", "height"} <= set(message):
                            raise CommandError("create requires width and height")
                        owner = create(message["width"], message["height"], trace_id, message.get("name"))
                        reply = {"ok": True, "state": owner.snapshot()}
                    elif operation in ("capture", "clear_history"):
                        allowed = {"op", "session", "pane"} | ({"history"} if operation == "capture" else set())
                        if set(message) - allowed:
                            raise CommandError("unexpected pane command fields")
                        owner = sessions.get(integer(message["session"], "session")) if "session" in message else attachment
                        if owner is None:
                            raise CommandError("select an existing session")
                        if "pane" in message:
                            pane = integer(message["pane"], "pane")
                        elif owner.session.current is not None:
                            pane = owner.session.current.focus
                        else:
                            raise CommandError("session has no panes")
                        terminal = owner.terminals.get(pane)
                        if terminal is None:
                            raise CommandError("unknown terminal pane")
                        if operation == "capture":
                            history = integer(message.get("history", 0), "history")
                            if not 0 <= history <= 10000:
                                raise CommandError("history must be between 0 and 10000 rows")
                            reply = {"ok": True, "text": await terminal.capture(history)}
                        else:
                            await terminal.clear_history()
                            reply = {"ok": True, "cleared": pane}
                    elif operation == "attach":
                        if attachment is not None:
                            raise CommandError("connection already attached")
                        if set(message) - {"op", "session", "width", "height", "events"} or not {"width", "height"} <= set(message):
                            raise CommandError("attach requires width and height")
                        events = message.get("events", False)
                        if type(events) is not bool:
                            raise CommandError("events must be boolean")
                        if "session" in message:
                            identity = integer(message["session"], "session")
                            owner = sessions.get(identity)
                            if owner is None:
                                raise CommandError("unknown session")
                            if owner.writer is not None:
                                raise CommandError("session already has an active attachment")
                            owner.session.viewport(message["width"], message["height"])
                        else:
                            owner = create(message["width"], message["height"], trace_id)
                        attachment = owner
                        attachment_key = bind(attachment, writer, events)
                        reply = {"ok": True, "key": attachment_key, "state": attachment.snapshot()}
                    elif operation == "destroy":
                        if set(message) - {"op", "session", "if_empty", "select_next"}:
                            raise CommandError("unexpected destroy fields")
                        if_empty = message.get("if_empty", False)
                        if type(if_empty) is not bool:
                            raise CommandError("if_empty must be boolean")
                        select_next = message.get("select_next", False)
                        if type(select_next) is not bool:
                            raise CommandError("select_next must be boolean")
                        identity = integer(message.get("session", attachment.session.identity if attachment else None), "session")
                        owner = sessions.get(identity)
                        if owner is None:
                            raise CommandError("unknown session")
                        if if_empty and owner.terminals:
                            raise CommandError("session is not empty")
                        if select_next and attachment is not owner:
                            raise CommandError("select_next requires destroying the attached session")
                        candidates = [sessions.get(previous_session), *reversed(sessions.values())]
                        successor = next((a for a in candidates if a is not None
                                          if a is not owner and a.writer is None), None) if select_next else None
                        if successor is not None:
                            successor.session.viewport(owner.session.width, owner.session.height)
                        events = owner.events
                        active_writer = owner.writer
                        release(owner, owner.key)
                        sessions.pop(identity)
                        owner.close()
                        if active_writer is not None and active_writer is not writer:
                            send(active_writer, {"event": "destroyed", "session": identity})
                            active_writer.close()
                        if attachment is owner:
                            attachment = None
                            detach = successor is None
                        if previous_session == identity or (successor is not None and previous_session == successor.session.identity):
                            previous_session = None
                        reply = {"ok": True, "destroyed": identity}
                        if successor is not None:
                            attachment = successor
                            attachment_key = bind(attachment, writer, events)
                            reply.update(key=attachment_key, state=attachment.snapshot())
                    elif attachment is None:
                        raise CommandError("attach before sending workspace commands")
                    elif operation in ("session_select", "session_new", "session_last"):
                        allowed = {"op", "session"} if operation == "session_select" else {"op", "name"} if operation == "session_new" else {"op"}
                        if set(message) - allowed or (operation == "session_select" and "session" not in message):
                            raise CommandError("unexpected session fields")
                        if operation == "session_new":
                            owner = create(attachment.session.width, attachment.session.height, trace_id, message.get("name"))
                        else:
                            identity = previous_session if operation == "session_last" else integer(message["session"], "session")
                            owner = sessions.get(identity)
                            if owner is None:
                                raise CommandError("no previous session" if operation == "session_last" else "unknown session")
                            if owner.writer is not None and owner is not attachment:
                                raise CommandError("session already has an active attachment")
                            owner.session.viewport(attachment.session.width, attachment.session.height)
                        if owner is not attachment:
                            events = attachment.events
                            previous_session = attachment.session.identity
                            release(attachment, attachment_key)
                            attachment = owner
                            attachment_key = bind(owner, writer, events)
                        reply = {"ok": True, "key": attachment_key, "state": attachment.snapshot()}
                    elif operation == "detach":
                        if set(message) != {"op"}:
                            raise CommandError("unexpected request fields")
                        identity = attachment.session.identity
                        release(attachment, attachment_key)
                        attachment = None
                        detach = True
                        reply = {"ok": True, "detached": identity}
                    else:
                        if operation == "surface_gone":
                            if set(message) != {"op", "pane"}:
                                raise CommandError("unexpected surface event fields")
                            pane = integer(message["pane"], "pane")
                            reap()
                            if pane in attachment.terminals:
                                identity = attachment.session.identity
                                release(attachment, attachment_key)
                                attachment = None
                                detach = True
                                reply = {"ok": True, "detached": identity}
                            else:
                                reply = {"ok": True, "state": attachment.snapshot()}
                        else:
                            reply = {"ok": True, "state": apply(attachment, message, trace_id)}
                except (ValueError, OSError, UnicodeDecodeError) as error:
                    reply = {"ok": False, "error": str(error)}
                    if attachment is not None and attachment.session.identity in sessions:
                        reply["state"] = attachment.snapshot()
                if trace_id is not None:
                    reply["_trace"] = trace_id
                trace.emit("decision_complete", trace_id=trace_id, ok=reply["ok"],
                           panes=sum(len(w["panes"]) for w in reply.get("state", {}).get("workspaces", [])))
                send(writer, reply)
                trace.emit("reply_queued", trace_id=trace_id)
                await writer.drain()
                if detach:
                    break
        except (ConnectionError, ValueError):
            pass
        finally:
            if history_owner is not None:
                history_owner.history_clients.discard(task)
            if terminal_attachment is not None:
                terminal = terminal_attachment.terminals.get(terminal_pane)
                if terminal is not None:
                    if terminal.connection is task:
                        terminal.disconnect()
            if attachment is not None:
                release(attachment, attachment_key)
            clients.discard(task)
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as bound:
        bound.bind(path)
        listener = None
        try:
            os.chmod(path, 0o600)
            bound.setblocking(False)
            listener = await asyncio.start_unix_server(handle, sock=bound, cleanup_socket=False)
            stop = asyncio.Event()
            loop = asyncio.get_running_loop()
            for signum in (signal.SIGINT, signal.SIGTERM):
                loop.add_signal_handler(signum, stop.set)
            loop.add_signal_handler(signal.SIGCHLD, reap)
            print(f"amux {__version__} server: {path}", file=sys.stderr, flush=True)
            await stop.wait()
        finally:
            if listener is not None:
                listener.close()
                listener.close_clients()
            for owner in list(sessions.values()):
                release(owner, owner.key)
                owner.close()
            sessions.clear()
            pending = list(clients)
            for task in pending:
                task.cancel()
            await asyncio.gather(*pending, return_exceptions=True)
            if listener is not None:
                await listener.wait_closed()
            await asyncio.gather(*list(closing), return_exceptions=True)
            for pid in list(children):
                try:
                    os.waitpid(pid, 0)
                except ChildProcessError:
                    pass
            Path(path).unlink()


def run(path, shell):
    asyncio.run(serve(path, shell))
