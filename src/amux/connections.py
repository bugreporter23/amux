"""Local connection selection and forwarding for one graphical frontend."""

import asyncio
from contextlib import AsyncExitStack
import json
import os

from . import PROTOCOL_VERSION, envelope
from .commands import parse
from .editor_view import view_command
from .launcher import default_socket, ensure_server, verify_server


class Connections:
    def __init__(self, profiles, initial=None, start_server=False, server_config=None):
        self.server_config = server_config
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

    def roster(self):
        return [{"connection": i, "name": p["name"], "remote": "transport" in p}
                for i, p in enumerate(self.profiles)]

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
        if type(index) is not int or not 0 <= index < len(self.profiles):
            raise ValueError("unknown connection")
        profile = self.profiles[index]
        stack = AsyncExitStack()
        writer = None
        try:
            if "transport" in profile:
                from .transport import bridge
                path, _ = await stack.enter_async_context(bridge(profile["transport"]))
            else:
                path = profile["socket"]
                if profile.get("start_server") or path == default_socket():
                    await asyncio.to_thread(ensure_server, path, self.server_config)
            await asyncio.to_thread(verify_server, path)
            reader, writer = await asyncio.open_unix_connection(path, limit=16 * 1024 * 1024)
            await self.drop()
        except BaseException:
            if writer is not None:
                writer.close()
                await writer.wait_closed()
            await stack.aclose()
            raise
        self.stack, self.path, self.selected = stack, path, index
        self.reader, self.writer = reader, writer
        writer.write(json.dumps(envelope({"op": "list"})).encode() + b"\n")
        await writer.drain()
        try:
            reply = json.loads(await asyncio.wait_for(reader.readline(), 3))
            if not reply["ok"]:
                raise OSError(reply["error"])
        except (OSError, ValueError, KeyError, TimeoutError):
            await self.drop()
            raise OSError("selected server disconnected")
        if self.client is not None:
            self.forwarding = asyncio.create_task(self.forward())
        return {"ok": True, "connection_path": path, "connection_name": profile["name"],
                "sessions": reply["sessions"]}

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
                if op not in ("connections", "connection_select"):
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
                        allowed = {"protocol", "version", "op", "_trace"} | ({"connection"} if op == "connection_select" else set())
                        if type(message.get("protocol")) is not int or message["protocol"] != PROTOCOL_VERSION or set(message) - allowed:
                            raise ValueError("invalid connection request")
                        reply = ({"ok": True, "connections": self.roster()} if op == "connections"
                                 else await self.select(message.get("connection")))
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
            await self.drop()
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    async def close(self):
        if self.handler is not None and not self.handler.done():
            self.handler.cancel()
            await asyncio.gather(self.handler, return_exceptions=True)
        else:
            await self.drop()
