import asyncio
import fcntl
import json
import os
import signal
import struct
import termios
import tty

from .terminal_io import read_fd, write_fd
from .trace import Trace
from pathlib import Path

from . import envelope

async def exchange(reader, writer, message, trace):
    identity = trace.request_id()
    if identity:
        message["_trace"] = identity
    trace.emit("request_send", trace_id=identity, op=message["op"], pane=message["pane"])
    writer.write((json.dumps(envelope(message), separators=(",", ":")) + "\n").encode())
    await writer.drain()
    frame = await reader.readline()
    if not frame:
        raise ConnectionError("server disconnected")
    reply = json.loads(frame)
    if identity and reply.get("_trace") != identity:
        raise ConnectionError("trace reply identifier mismatch")
    trace.emit("reply_received", trace_id=identity, op=message["op"], pane=message["pane"])
    if not reply["ok"]:
        raise ConnectionError(reply["error"])
    return reply


async def attach(path, key, pane):
    trace = Trace("relay")
    trace.emit("relay_start", pane=pane)
    attributes = termios.tcgetattr(0)
    blocking = os.get_blocking(0), os.get_blocking(1)
    control_reader, control_writer = await asyncio.open_unix_connection(path)
    try:
        reader, writer = await asyncio.open_unix_connection(path)
    except BaseException:
        control_writer.close()
        await control_writer.wait_closed()
        raise
    history_path = Path(os.environ["XDG_RUNTIME_DIR"]) / f"history-{pane}.sock"
    history_clients = set()

    async def history(reader, writer):
        task = asyncio.current_task()
        history_clients.add(task)
        remote_writer = None
        try:
            remote_reader, remote_writer = await asyncio.open_unix_connection(path, limit=1048576)
            while raw := await reader.readline():
                read = json.loads(raw)
                response = await exchange(remote_reader, remote_writer,
                    {"op": "history", "key": key, "pane": pane, "read": read}, trace)
                writer.write(json.dumps(response, separators=(",", ":")).encode() + b"\n")
                await writer.drain()
                await asyncio.sleep(0)
        except (OSError, ValueError) as error:
            writer.write(json.dumps({"error": str(error)}).encode() + b"\n")
            await writer.drain()
        finally:
            history_clients.discard(task)
            writer.close()
            if remote_writer is not None:
                remote_writer.close()
                await remote_writer.wait_closed()
            await writer.wait_closed()

    listener = await asyncio.start_unix_server(history, str(history_path), limit=65536, cleanup_socket=True)
    os.chmod(history_path, 0o600)
    tasks = []
    resize = asyncio.Event()
    loop = asyncio.get_running_loop()
    loop.add_signal_handler(signal.SIGWINCH, resize.set)
    try:
        rows, columns, _, _ = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8))
        await exchange(control_reader, control_writer,
                       {"op": "terminal_size", "key": key, "pane": pane,
                        "columns": columns, "rows": rows}, trace)
        await exchange(reader, writer, {"op": "terminal", "key": key, "pane": pane}, trace)
        tty.setraw(0)
        os.set_blocking(0, False)
        os.set_blocking(1, False)
        trace.emit("relay_ready", pane=pane)

        async def input_():
            while data := await read_fd(0):
                writer.write(data)
                await writer.drain()

        async def output():
            first = True
            while data := await reader.read(65536):
                if first:
                    trace.emit("relay_output_first", pane=pane, bytes=len(data))
                await write_fd(1, data)
                if first:
                    trace.emit("relay_output_written", pane=pane)
                    first = False

        async def sizes():
            resize.set()
            while True:
                await resize.wait()
                resize.clear()
                rows, columns, _, _ = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8))
                await exchange(control_reader, control_writer,
                               {"op": "terminal_size", "key": key, "pane": pane,
                                "columns": columns, "rows": rows}, trace)

        tasks = [asyncio.create_task(input_()), asyncio.create_task(output()), asyncio.create_task(sizes())]
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            task.result()
    finally:
        listener.close()
        await listener.wait_closed()
        history_tasks = list(history_clients)
        for task in history_tasks:
            task.cancel()
        await asyncio.gather(*history_tasks, return_exceptions=True)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        loop.remove_signal_handler(signal.SIGWINCH)
        termios.tcsetattr(0, termios.TCSANOW, attributes)
        os.set_blocking(0, blocking[0])
        os.set_blocking(1, blocking[1])
        writer.close()
        control_writer.close()
        await asyncio.gather(writer.wait_closed(), control_writer.wait_closed(), return_exceptions=True)


def run(path, key, pane):
    asyncio.run(attach(path, key, pane))
