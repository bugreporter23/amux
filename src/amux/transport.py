"""Carry amux socket streams over one binary stdio connection."""

import asyncio
from contextlib import asynccontextmanager
from collections import deque
import itertools
import json
import os
from pathlib import Path
import struct
import sys
import tempfile

from . import PROTOCOL_VERSION
from .launcher import ensure_server, verify_server
from .terminal_io import write_fd
from .trace import Trace

HEADER = struct.Struct("!cII")
CHUNK = 32768


class Channel:
    def __init__(self, peer, number, reader, writer, traffic=0, initial=b""):
        self.peer, self.number = peer, number
        self.reader, self.writer = reader, writer
        self.ready = asyncio.get_running_loop().create_future()
        self.queue = asyncio.Queue(maxsize=9)
        self.credit = 8
        self.writable = asyncio.Event()
        self.writable.set()
        self.tasks = []
        self.upload_done = self.download_done = self.remote_eof = False
        self.traffic, self.initial = traffic, initial

    def start(self):
        self.tasks = [asyncio.create_task(self.upload()), asyncio.create_task(self.download())]

    async def upload(self):
        try:
            while True:
                await self.writable.wait()
                self.credit -= 1
                if not self.credit:
                    self.writable.clear()
                data = self.initial[:CHUNK] if self.initial else await self.reader.read(CHUNK)
                self.initial = self.initial[CHUNK:]
                if not data:
                    break
                priority = self.traffic if self.peer.server_path is not None or self.traffic == 2 else 0
                await self.peer.send(b"D", self.number, data, priority)
                await asyncio.sleep(0)
            await self.peer.send(b"E", self.number)
            self.upload_done = True
            self.finish()
        except OSError:
            await self.peer.abort(self.number, notify=True)

    async def download(self):
        try:
            while (data := await self.queue.get()) is not None:
                self.writer.write(data)
                await self.writer.drain()
                await self.peer.send(b"W", self.number)
            self.writer.write_eof()
            await self.writer.drain()
            self.download_done = True
            self.finish()
        except OSError:
            await self.peer.abort(self.number, notify=True)

    def finish(self):
        if self.upload_done and self.download_done:
            self.peer.channels.pop(self.number, None)
            self.writer.close()

    def close(self):
        if not self.ready.done():
            self.ready.cancel()
        for task in self.tasks:
            if task is not asyncio.current_task():
                task.cancel()
        self.writer.close()


class Peer:
    def __init__(self, reader, write, server_path=None):
        self.reader, self.write, self.server_path = reader, write, server_path
        self.channels = {}
        self.ids = itertools.count(1)
        self.pending = [deque(), deque(), deque()]
        self.sending = asyncio.Event()
        self.sender = None
        self.foreground = 0
        self.connecting = set()
        self.closed = False

    async def send(self, kind, number, data=b"", priority=0):
        if self.closed:
            raise OSError("mux transport disconnected")
        done = asyncio.get_running_loop().create_future()
        self.pending[priority].append((HEADER.pack(kind, number, len(data)) + data, done))
        self.sending.set()
        if self.sender is None:
            self.sender = asyncio.create_task(self.flush())
        await done

    async def flush(self):
        current = None
        try:
            while True:
                await self.sending.wait()
                priority = 2 if self.pending[2] and self.foreground >= 8 else next(i for i, q in enumerate(self.pending) if q)
                data, current = self.pending[priority].popleft()
                if not any(self.pending):
                    self.sending.clear()
                if current.cancelled():
                    continue
                await self.write(data)
                if not current.done():
                    current.set_result(None)
                self.foreground = 0 if priority == 2 else min(8, self.foreground + 1)
                current = None
                await asyncio.sleep(0)
        except (OSError, asyncio.CancelledError) as error:
            self.closed = True
            waiting = ([current] if current is not None else []) + [done for q in self.pending for _, done in q]
            for done in waiting:
                if not done.done():
                    done.set_exception(OSError("mux transport disconnected"))
            for q in self.pending:
                q.clear()
            if isinstance(error, OSError):
                self.reader.set_exception(error)

    async def abort(self, number, notify=False):
        channel = self.channels.pop(number, None)
        if channel is not None:
            channel.close()
        if notify and not self.closed:
            await self.send(b"C", number)

    async def accept(self, reader, writer):
        task = asyncio.current_task()
        self.connecting.add(task)
        number = next(self.ids)
        try:
            initial = await asyncio.wait_for(reader.readline(), 3)
            message = json.loads(initial)
            if not isinstance(message, dict):
                raise OSError("invalid stream request")
            traffic = 2 if message.get("op") in ("history", "capture") else 1 if message.get("op") in ("terminal", "editor") else 0
            channel = Channel(self, number, reader, writer, traffic, initial)
            self.channels[number] = channel
            await self.send(b"O", number, bytes([traffic]))
            await asyncio.wait_for(channel.ready, 3)
            channel.start()
        except (OSError, ValueError, TimeoutError, asyncio.CancelledError):
            await self.abort(number, notify=True)
            writer.close()
        finally:
            self.connecting.discard(task)

    async def open(self, number, traffic):
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_unix_connection(self.server_path), 3)
            if number not in self.channels or self.closed:
                writer.close()
                await writer.wait_closed()
                return
            channel = Channel(self, number, reader, writer, traffic)
            self.channels[number] = channel
            channel.ready.set_result(None)
            await self.send(b"A", number)
            channel.start()
        except OSError:
            await self.abort(number, notify=True)

    async def run(self):
        try:
            while True:
                kind, number, length = HEADER.unpack(await self.reader.readexactly(HEADER.size))
                if not number or length > CHUNK or (kind not in (b"D", b"O") and length) or (kind == b"O" and length != 1):
                    raise OSError("invalid transport frame")
                data = await self.reader.readexactly(length)
                if kind == b"O":
                    if self.server_path is None or number in self.channels or data[0] > 2:
                        raise OSError("invalid transport stream open")
                    self.channels[number] = None
                    task = asyncio.create_task(self.open(number, data[0]))
                    self.connecting.add(task)
                    task.add_done_callback(self.connecting.discard)
                    continue
                channel = self.channels.get(number)
                if kind == b"C":
                    await self.abort(number)
                elif kind in (b"A", b"D", b"E", b"W"):
                    if channel is None:
                        continue
                    if kind == b"A":
                        if channel.ready.done() or self.server_path is not None:
                            raise OSError("unexpected transport open acknowledgement")
                        channel.ready.set_result(None)
                    elif kind == b"W":
                        if channel.credit >= 8:
                            raise OSError("invalid transport credit")
                        channel.credit += 1
                        channel.writable.set()
                    else:
                        if channel.remote_eof:
                            raise OSError("transport data after stream EOF")
                        channel.remote_eof = kind == b"E"
                        try:
                            channel.queue.put_nowait(None if kind == b"E" else data)
                        except asyncio.QueueFull:
                            await self.abort(number, notify=True)
                else:
                    raise OSError("unknown transport frame")
        except asyncio.IncompleteReadError:
            return
        finally:
            self.closed = True
            if self.sender is not None:
                self.sender.cancel()
                await asyncio.gather(self.sender, return_exceptions=True)
            channels = [channel for channel in self.channels.values() if channel is not None]
            for task in list(self.connecting):
                task.cancel()
            for channel in channels:
                channel.close()
            await asyncio.gather(*list(self.connecting),
                                 *(task for channel in channels for task in channel.tasks),
                                 return_exceptions=True)
            self.channels.clear()


async def serve_stdio(path=None, config=None, reader=None):
    trace = Trace("stdio")
    trace.emit("server_start_begin")
    if path is None:
        from .endpoints import guest_socket
        path = guest_socket()
    ensure_server(path, config)
    trace.emit("server_start_end")
    expected = verify_server(path)
    del expected["ok"]
    trace.emit("server_verified")
    os.set_blocking(1, False)
    await write_fd(1, json.dumps(expected).encode() + b"\n")
    pipe = None
    if reader is None:
        reader = asyncio.StreamReader()
        protocol = asyncio.StreamReaderProtocol(reader)
        pipe, _ = await asyncio.get_running_loop().connect_read_pipe(lambda: protocol, sys.stdin.buffer)
    try:
        await Peer(reader, lambda data: write_fd(1, data), path).run()
    finally:
        if pipe is not None:
            pipe.close()


async def route_stdio():
    """Host discovery precedes the exact-version session transport on the same SSH."""
    from .endpoints import catalog, guest_socket
    from .bookmarks import socket_path
    os.set_blocking(1, False)
    reader = asyncio.StreamReader()
    protocol = asyncio.StreamReaderProtocol(reader)
    pipe, _ = await asyncio.get_running_loop().connect_read_pipe(lambda: protocol, sys.stdin.buffer)
    try:
        async def listing():
            rows, warning = catalog(remote=True)
            await write_fd(1, json.dumps({"discovery": 1, "sockets": rows, "warning": warning}).encode() + b"\n")
        await listing()
        while data := await reader.readline():
            try:
                request = json.loads(data)
                if request == {"op": "sockets"}:
                    await listing()
                    continue
                if not isinstance(request, dict) or set(request) != {"op", "socket"} or request["op"] != "select":
                    raise ValueError("invalid route request")
                path = guest_socket() if request["socket"] == "" else socket_path(request["socket"])
                if request["socket"] == "":
                    ensure_server(path)
                verify_server(path)
            except (OSError, ValueError) as error:
                await write_fd(1, json.dumps({"error": str(error)}).encode() + b"\n")
                continue
            await serve_stdio(path, reader=reader)
            return
    finally:
        pipe.close()


class Route:
    """A pending SSH host connection; authentication is reused for socket selection."""

    def __init__(self, process):
        self.process = process

    async def reply(self, timeout=3):
        data = await asyncio.wait_for(self.process.stdout.readline(), timeout)
        if not data:
            raise OSError("remote socket discovery disconnected")
        value = json.loads(data)
        if not isinstance(value, dict):
            raise ValueError("invalid remote socket reply")
        if "error" in value:
            raise OSError(value["error"])
        return value

    async def request(self, message):
        self.process.stdin.write(json.dumps(message).encode() + b"\n")
        await self.process.stdin.drain()
        return await self.reply(15)


@asynccontextmanager
async def route(command):
    process = await asyncio.create_subprocess_exec(*command, stdin=asyncio.subprocess.PIPE,
                                                  stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                                                  limit=256 * 1024,
                                                  env=dict(os.environ, SSH_ASKPASS_REQUIRE="never" if os.environ.get("SSH_ASKPASS_REQUIRE") == "never" else "force"))
    errors = bytearray()
    async def diagnostics():
        while data := await process.stderr.read(4096):
            errors.extend(data); del errors[:-4096]
            sys.stderr.write(data.decode(errors="replace")); sys.stderr.flush()
    diagnostic = asyncio.create_task(diagnostics())
    endpoint = Route(process)
    try:
        try:
            greeting = await endpoint.reply(120)
            if greeting.get("discovery") != 1 or not isinstance(greeting.get("sockets"), list):
                raise ValueError("invalid remote socket discovery greeting")
        except (OSError, ValueError, TimeoutError) as error:
            if process.returncode is None:
                process.terminate()
            await process.wait()
            try:
                await asyncio.wait_for(diagnostic, 1)
            except TimeoutError:
                pass
            detail = errors.decode(errors="replace").strip()
            raise OSError(f"SSH connection failed: {str(error) or 'socket discovery timed out'}" + (f"; {detail}" if detail else "")) from error
        yield endpoint, greeting
    finally:
        if process.returncode is None:
            process.terminate()
        await process.wait()
        diagnostic.cancel()
        await asyncio.gather(diagnostic, return_exceptions=True)


@asynccontextmanager
async def bridge(command=None, *, capture_stderr=False, greeting_timeout=15, environment=None, process=None, greeting=None):
    trace = Trace("transport")
    trace.emit("remote_launch_begin")
    owned = process is None
    if owned:
        process = await asyncio.create_subprocess_exec(*command, stdin=asyncio.subprocess.PIPE,
                                                  stdout=asyncio.subprocess.PIPE,
                                                  stderr=asyncio.subprocess.PIPE if capture_stderr else None,
                                                  env=dict(os.environ, **environment) if environment else None)
    errors = bytearray()
    async def diagnostics():
        while data := await process.stderr.read(4096):
            errors.extend(data)
            del errors[:-4096]
            sys.stderr.write(data.decode(errors="replace"))
            sys.stderr.flush()
    diagnostic_task = asyncio.create_task(diagnostics()) if capture_stderr else None
    peer_task = None
    try:
        try:
            if greeting is None:
                data = await asyncio.wait_for(process.stdout.readline(), greeting_timeout)
                if not data or len(data) > 512:
                    raise OSError("invalid remote mux greeting")
                hello = json.loads(data)
            else:
                hello = greeting
            if not isinstance(hello, dict):
                raise OSError("invalid remote mux identity")
            if type(hello.get("protocol")) is not int or hello["protocol"] != PROTOCOL_VERSION:
                raise OSError(f"remote mux protocol mismatch: local {PROTOCOL_VERSION}, remote {hello.get('protocol')}")
            if (set(hello) != {"protocol", "version", "incarnation"}
                    or not isinstance(hello["version"], str) or not isinstance(hello["incarnation"], str)):
                raise OSError("invalid remote mux identity")
        except (OSError, ValueError, TimeoutError) as error:
            if not capture_stderr:
                raise
            if process.returncode is None:
                process.terminate()
            await process.wait()
            try:
                await asyncio.wait_for(diagnostic_task, 1)
            except TimeoutError:
                pass
            detail = errors.decode(errors="replace").strip()
            reason = str(error) or "remote Amux greeting timed out"
            raise OSError(f"SSH connection failed: {reason}" + (f"; {detail}" if detail else "")) from error
        trace.emit("remote_greeting", version=hello["version"], protocol=hello["protocol"], incarnation=hello["incarnation"])
        async def write(data):
            process.stdin.write(data)
            await process.stdin.drain()
        peer = Peer(process.stdout, write)
        peer_task = asyncio.create_task(peer.run())
        with tempfile.TemporaryDirectory(prefix="amux-connection-", dir=os.environ.get("XDG_RUNTIME_DIR")) as directory:
            path = str(Path(directory) / "mux.sock")
            listener = await asyncio.start_unix_server(peer.accept, path=path)
            async with listener:
                yield path, peer_task
    finally:
        if peer_task is not None:
            peer_task.cancel()
            await asyncio.gather(peer_task, return_exceptions=True)
        if owned and process.returncode is None:
            process.terminate()
        if owned:
            await process.wait()
        if diagnostic_task is not None:
            diagnostic_task.cancel()
            await asyncio.gather(diagnostic_task, return_exceptions=True)


async def connect(command, session=None, cli=False, config=None, as_json=False, ready=None, server_config=None):
    if as_json and not cli:
        raise ValueError("--json requires --cli")
    if (config is not None or server_config is not None) and cli:
        raise ValueError("--config and --server-config require graphical attachment")
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        raise ValueError("connect requires a stdio transport command after --")
    if not cli:
        from .gui import run_async
        return await run_async(None, 1200, 800, session, config, ready=ready, initial_transport=command,
                               server_config=server_config)
    entry = os.environ["AMUX_ENTRY"]
    async with bridge(command) as (path, peer):
        argv = [entry, "client" if cli else "gui", "--socket", path]
        if not cli:
            argv += ["--foreground"]
        if as_json:
            argv += ["--json"]
        if session is not None:
            argv += ["--session", str(session)]
        if config is not None:
            argv += ["--config", config]
        frontend = await asyncio.create_subprocess_exec(*argv)
        trace = Trace("connect")
        trace.emit("frontend_spawned")
        waiting = asyncio.create_task(frontend.wait())
        try:
            if ready is not None:
                trace.emit("launch_acknowledged")
                ready()
            done, _ = await asyncio.wait([peer, waiting], return_when=asyncio.FIRST_COMPLETED)
            if peer in done:
                peer.result()
                raise OSError("mux transport disconnected")
            return waiting.result()
        finally:
            if frontend.returncode is None:
                frontend.terminate()
            await frontend.wait()
            await waiting
