"""Local connection selection and forwarding for one graphical frontend."""

import asyncio
from contextlib import AsyncExitStack
import json
import os
import shlex

from . import PROTOCOL_VERSION, envelope
from .commands import parse
from .editor_view import view_command
from .launcher import default_socket, ensure_server, verify_server
from .ssh import Targets, identities
from .bookmarks import Bookmarks, socket_path


class Connections:
    def __init__(self, profiles, initial=None, start_server=False, server_config=None, ssh_command=None, ssh_targets=None, bookmarks=None):
        self.server_config = server_config
        self.ssh_command = ["ssh"] if ssh_command is None else list(ssh_command)
        self.ssh_targets = Targets() if ssh_targets is None else ssh_targets
        self.bookmarks = Bookmarks() if bookmarks is None else bookmarks
        self.bookmark_rows, self.suggestions = [], []
        self.profiles = [dict(profile) for profile in profiles]
        local = {"name": "Local", "socket": default_socket()}
        if not any(p.get("socket") == local["socket"] for p in self.profiles):
            self.profiles.insert(0, local)
        self.initial = None
        if initial is not None:
            target = "transport" if "transport" in initial else "socket"
            self.initial = next((i for i, p in enumerate(self.profiles) if p.get(target) == initial[target]), None)
            if self.initial is None:
                self.initial = len(self.profiles)
                self.profiles.append(dict(initial))
            self.profiles[self.initial]["start_server"] = start_server
        self.selected = None
        self.path = None
        self.reader = self.writer = self.client = self.forwarding = None
        self.stack = None
        self.attached = False
        self.handler = None
        self.route_stack = self.pending_route = self.route_profile = None

    def roster(self):
        rows = [{"connection": "local-sockets", "name": "Local sockets", "kind": "machine",
                 "address": "This machine", "source": "local", "remote": False, "active": False}]
        for i, profile in enumerate(self.profiles):
            kind = "ssh" if profile.get("ssh") else "command" if "transport" in profile else "socket"
            rows.append({"active": self.selected == i, "connection": i, "name": profile["name"], "remote": kind != "socket",
                         "source": "opened" if profile.get("ssh") else "local" if profile.get("socket") == default_socket() else "config",
                         "kind": kind, "address": profile.get("destination") if kind == "ssh" else
                         shlex.join(profile["transport"]) if kind == "command" else profile["socket"],
                         **({"identity": profile["identity"]} if profile.get("identity") else {})})
        rows.extend({**row, "remote": row["kind"] == "ssh", "source": "saved", "active": self.selected == row["connection"]} for row in self.bookmark_rows)
        endpoints = {(row["address"], row.get("identity", "")) for row in rows if row["kind"] == "ssh"}
        rows.extend({"connection": f"suggestion:{i}", "name": row["name"], "kind": "ssh",
                     "address": row["destination"], "source": row["source"], "remote": True,
                     **({"identity": row["identity"]} if row.get("identity") else {})}
                    for i, row in enumerate(self.suggestions) if (row["destination"], row.get("identity", "")) not in endpoints)
        return rows

    async def listing(self):
        await self.cancel_route()
        warning = ""
        try:
            self.bookmark_rows = await asyncio.to_thread(self.bookmarks.read)
        except (OSError, ValueError) as error:
            warning = f"Connection bookmarks: {error}"
        self.suggestions, ssh_warning = await asyncio.to_thread(self.ssh_targets.suggestions)
        reply = {"ok": True, "connections": self.roster(), "connection_bookmarks": self.bookmark_rows}
        try:
            reply["ssh_keys"] = await asyncio.to_thread(identities)
        except OSError as error:
            reply["ssh_keys"] = []
            warning = "; ".join(filter(None, (warning, f"SSH key suggestions: {error}")))
        if warning or ssh_warning:
            reply["warning"] = "; ".join(filter(None, (warning, ssh_warning)))
        return reply

    async def cancel_route(self):
        stack, self.route_stack = self.route_stack, None
        self.pending_route = self.route_profile = None
        if stack is not None:
            await stack.aclose()

    async def browse(self, connection=None, destination=None, identity=""):
        await self.cancel_route()
        if connection is not None and connection != "local-sockets":
            row = next((row for row in self.roster() if row["connection"] == connection), None)
            if row is None or row["kind"] != "ssh":
                raise ValueError("select a machine to browse its sockets")
            destination, identity = row["address"], row.get("identity", "")
        if destination is None:
            from .endpoints import catalog
            rows, warning = await asyncio.to_thread(catalog)
            self.route_profile = {"name": "Local", "local": True}
            return {"ok": True, "sockets": rows, "route_name": "Local", "route_identity": "", "warning": warning}
        from .ssh import command
        from .transport import route
        stack = AsyncExitStack()
        try:
            pending, greeting = await stack.enter_async_context(route(command(destination, self.ssh_command, identity, browse=True)))
        except BaseException:
            await stack.aclose()
            raise
        self.route_stack, self.pending_route = stack, pending
        self.route_profile = {"name": destination, "destination": destination, "identity": identity, "ssh": True}
        from .endpoints import validate_catalog
        try:
            rows = validate_catalog(greeting["sockets"])
        except ValueError:
            await self.cancel_route()
            raise
        return {"ok": True, "sockets": rows, "route_name": destination,
                "route_identity": identity, "warning": greeting.get("warning", "")}

    async def route_sockets(self):
        if self.route_profile is None:
            raise ValueError("choose a machine first")
        if self.pending_route is not None:
            greeting = await self.pending_route.request({"op": "sockets"})
            if greeting.get("discovery") != 1 or not isinstance(greeting.get("sockets"), list):
                raise ValueError("invalid remote socket catalog")
            from .endpoints import validate_catalog
            rows, warning = validate_catalog(greeting["sockets"]), greeting.get("warning", "")
        else:
            from .endpoints import catalog
            rows, warning = await asyncio.to_thread(catalog)
        return {"ok": True, "sockets": rows, "route_name": self.route_profile["name"],
                "route_identity": self.route_profile.get("identity", ""), "warning": warning}

    async def select_route_socket(self, value):
        if self.route_profile is None:
            raise ValueError("choose a machine first")
        if not isinstance(value, str) or len(value) > 511 or (value and not value.isprintable()):
            raise ValueError("socket endpoint must be a printable path")
        if self.pending_route is None:
            path = default_socket() if value == "" else socket_path(value)
            profile = {"name": path, "socket": path}
            index = next((i for i, p in enumerate(self.profiles) if p.get("socket") == path), len(self.profiles))
            reply = await self.select_profile(profile, index)
        else:
            greeting = await self.pending_route.request({"op": "select", "socket": value})
            profile = {**self.route_profile, "name": f"{self.route_profile['name']} · {value or 'default'}",
                       "remote_socket": value, "process": self.pending_route.process, "greeting": greeting}
            index = next((i for i, p in enumerate(self.profiles)
                          if p.get("ssh") and p.get("destination") == profile["destination"]
                          and p.get("identity", "") == profile.get("identity", "")), len(self.profiles))
            stack, self.route_stack = self.route_stack, None
            self.pending_route = self.route_profile = None
            reply = await self.select_profile(profile, index, stack)
            profile.pop("process"); profile.pop("greeting")
        if index == len(self.profiles):
            self.profiles.append(profile)
        else:
            self.profiles[index] = profile
        return reply

    async def drop(self):
        task, self.forwarding = self.forwarding, None
        if task is not None and task is not asyncio.current_task():
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        reader, writer = self.reader, self.writer
        self.reader = self.writer = None
        if writer is not None:
            try:
                if self.attached and not writer.is_closing():
                    writer.write(json.dumps(envelope({"op": "detach"})).encode() + b"\n")
                    await writer.drain()
                    async def detached():
                        while data := await reader.readline():
                            if "ok" in json.loads(data):
                                break
                    await asyncio.wait_for(detached(), 1)
            except (OSError, ValueError, TimeoutError):
                pass
            finally:
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass
        self.attached = False
        self.path = self.selected = None
        stack, self.stack = self.stack, None
        if stack is not None:
            await stack.aclose()

    async def select(self, index):
        if isinstance(index, str):
            row = next((row for row in self.roster() if row["connection"] == index), None)
            if row is None:
                raise ValueError("unknown connection")
            if row["kind"] == "machine":
                return await self.browse(index)
            if row["kind"] == "ssh":
                from .ssh import command
                profile = {"name": row["name"], "transport": command(row["address"], self.ssh_command, row.get("identity", "")),
                           "ssh": True, "destination": row["address"], "identity": row.get("identity", "")}
            else:
                profile = {"name": row["name"], "socket": row["address"]}
            return await self.select_profile(profile, index)
        if type(index) is not int or not 0 <= index < len(self.profiles):
            raise ValueError("unknown connection")
        if self.profiles[index].get("ssh") and "transport" not in self.profiles[index]:
            return await self.browse(index)
        return await self.select_profile(self.profiles[index], index)

    async def select_ssh(self, destination, identity=""):
        from .ssh import command
        transport = command(destination, self.ssh_command, identity)
        index = next((i for i, profile in enumerate(self.profiles)
                      if profile.get("transport") == transport), len(self.profiles))
        profile = (dict(self.profiles[index]) if index < len(self.profiles) else
                   {"name": f"SSH {destination}", "transport": transport})
        profile.update(ssh=True, destination=destination, identity=identity)
        reply = await self.select_profile(profile, index)
        if index == len(self.profiles):
            self.profiles.append(profile)
        else:
            self.profiles[index] = profile
        return reply

    async def select_profile(self, profile, index, stack=None):
        stack = AsyncExitStack() if stack is None else stack
        writer = None
        try:
            if "process" in profile:
                from .transport import bridge
                path, _ = await stack.enter_async_context(bridge(process=profile["process"], greeting=profile["greeting"]))
            elif "transport" in profile:
                from .transport import bridge
                path, _ = await stack.enter_async_context(bridge(profile["transport"],
                                                               capture_stderr=profile.get("ssh", False),
                                                               greeting_timeout=120 if profile.get("ssh") else 15,
                                                               environment={"SSH_ASKPASS_REQUIRE": "never" if os.environ.get("SSH_ASKPASS_REQUIRE") == "never" else "force"}
                                                               if profile.get("ssh") else None))
            else:
                path = profile["socket"]
                if profile.get("start_server") or path == default_socket():
                    await asyncio.to_thread(ensure_server, path, self.server_config)
            await asyncio.to_thread(verify_server, path)
            reader, writer = await asyncio.open_unix_connection(path, limit=16 * 1024 * 1024)
            writer.write(json.dumps(envelope({"op": "list"})).encode() + b"\n")
            await writer.drain()
            reply = json.loads(await asyncio.wait_for(reader.readline(), 3))
            if not isinstance(reply, dict) or reply.get("ok") is not True:
                raise OSError(reply.get("error", "invalid server list reply") if isinstance(reply, dict)
                              else "invalid server list reply")
            sessions = reply.get("sessions")
            if not isinstance(sessions, list):
                raise OSError("invalid server session list")
            await self.drop()
        except BaseException:
            if writer is not None:
                writer.close()
                await writer.wait_closed()
            await stack.aclose()
            raise
        self.stack, self.path, self.selected = stack, path, index
        self.reader, self.writer = reader, writer
        reply = {"ok": True, "connection_path": path, "connection_name": profile["name"],
                 "sessions": sessions}
        if profile.get("ssh") and "destination" in profile:
            try:
                await asyncio.to_thread(self.ssh_targets.remember, profile["destination"], profile.get("identity", ""))
            except (OSError, ValueError) as error:
                reply["warning"] = f"Connected; could not save SSH history: {error}"
        if self.client is not None:
            self.forwarding = asyncio.create_task(self.forward())
        return reply

    async def forward(self):
        reader, writer = self.reader, self.writer
        try:
            while data := await reader.readline():
                message = json.loads(data)
                if "key" in message and message.get("ok"):
                    self.attached = True
                if "detached" in message or message.get("event") == "destroyed":
                    self.attached = False
                self.client.write(data)
                await self.client.drain()
            raise OSError("server disconnected")
        except (OSError, ValueError) as error:
            if self.writer is writer:
                await self.drop()
                if self.client is not None:
                    self.client.write(json.dumps({"event": "connection_lost", "error": str(error)}).encode() + b"\n")
                    await self.client.drain()

    async def accept(self, reader, writer):
        if self.client is not None:
            writer.close()
            await writer.wait_closed()
            return
        self.client = writer
        self.handler = asyncio.current_task()
        if self.writer is not None:
            self.forwarding = asyncio.create_task(self.forward())
        try:
            while data := await reader.readline():
                message = json.loads(data)
                op = message.get("op")
                if op not in ("connections", "connection_select", "connection_ssh", "connection_socket", "connections_apply", "ssh_targets",
                              "connection_browse", "route_sockets", "route_select", "route_cancel"):
                    if self.writer is not None:
                        try:
                            command = parse(message.get("text", "")) if op == "command" else message
                            if command and command.get("op") == "edit":
                                view_command()
                        except (OSError, ValueError) as error:
                            reply = {"ok": False, "error": str(error)}
                            if "_trace" in message:
                                reply["_trace"] = message["_trace"]
                            writer.write(json.dumps(reply).encode() + b"\n")
                            await writer.drain()
                            continue
                        self.writer.write(data)
                        await self.writer.drain()
                        continue
                    reply = {"ok": False, "error": "select a connection"}
                else:
                    try:
                        allowed = {"protocol", "version", "op", "_trace"} | (
                            {"connection"} if op == "connection_select" else
                            {"destination", "identity"} if op == "connection_ssh" else
                            {"socket"} if op == "connection_socket" else
                            {"socket"} if op == "route_select" else
                            {"connection", "destination", "identity"} if op == "connection_browse" else
                            {"base", "bookmarks"} if op == "connections_apply" else set())
                        if type(message.get("protocol")) is not int or message["protocol"] != PROTOCOL_VERSION or set(message) - allowed:
                            raise ValueError("invalid connection request")
                        if op == "connections":
                            reply = await self.listing()
                        elif op == "connection_browse":
                            if "destination" in message and not isinstance(message["destination"], str):
                                raise ValueError("SSH destination must be text")
                            if "connection" in message and type(message["connection"]) not in (str, int):
                                raise ValueError("invalid machine connection")
                            if "connection" in message and ("destination" in message or "identity" in message):
                                raise ValueError("select a saved machine or enter a destination")
                            if "identity" in message and "destination" not in message:
                                raise ValueError("SSH identity requires a destination")
                            reply = await self.browse(message.get("connection"), message.get("destination"), message.get("identity", ""))
                        elif op == "route_sockets":
                            reply = await self.route_sockets()
                        elif op == "route_select":
                            reply = await self.select_route_socket(message.get("socket"))
                        elif op == "route_cancel":
                            await self.cancel_route()
                            reply = {"ok": True, "route_cancelled": True}
                        elif op == "connections_apply":
                            self.bookmark_rows = await asyncio.to_thread(self.bookmarks.apply, message.get("base"), message.get("bookmarks"))
                            assigned = {str(before["connection"]): after["connection"]
                                        for before, after in zip(message["bookmarks"], self.bookmark_rows)
                                        if type(before["connection"]) is int and before["connection"] < 0}
                            reply = {"ok": True, "connections": self.roster(), "connection_bookmarks": self.bookmark_rows,
                                     "connection_ids": assigned}
                        elif op == "connection_socket":
                            path = socket_path(message.get("socket"))
                            index = next((i for i, p in enumerate(self.profiles) if p.get("socket") == path), len(self.profiles))
                            profile = {"name": path, "socket": path}
                            reply = await self.select_profile(profile, index)
                            if index == len(self.profiles):
                                self.profiles.append(profile)
                        elif op == "ssh_targets":
                            targets, warning = await asyncio.to_thread(self.ssh_targets.suggestions)
                            reply = {"ok": True, "ssh_targets": targets}
                            if warning:
                                reply["warning"] = warning
                        elif op == "connection_ssh":
                            reply = await self.select_ssh(message.get("destination"), message.get("identity", ""))
                        else:
                            reply = await self.select(message.get("connection"))
                    except (OSError, ValueError) as error:
                        reply = {"ok": False, "error": str(error)}
                        if self.writer is None:
                            reply["connection_path"] = ""
                if "_trace" in message:
                    reply["_trace"] = message["_trace"]
                writer.write(json.dumps(reply).encode() + b"\n")
                await writer.drain()
        except OSError:
            pass
        finally:
            self.client = None
            await self.cancel_route()
            await self.drop()
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    async def close(self):
        await self.cancel_route()
        if self.handler is not None and not self.handler.done():
            self.handler.cancel()
            await asyncio.gather(self.handler, return_exceptions=True)
        else:
            await self.drop()
