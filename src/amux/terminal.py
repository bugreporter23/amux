import asyncio
import fcntl
import json
import os
import re
import signal
import struct
import sys
import termios
import time


from .state import State, frame
from .terminal_io import read_fd, write_fd


class Terminal:
    kind = "terminal"

    def __init__(self, shell, gone, trace, pane, cwd=None, focus=None):
        master, slave = os.openpty()
        try:
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
            pid = os.fork()
        except BaseException:
            os.close(master)
            os.close(slave)
            raise
        if pid == 0:
            try:
                if cwd is not None:
                    os.fchdir(cwd)
                    os.close(cwd)
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
                for target in (0, 1, 2):
                    os.dup2(slave, target)
                os.close(master)
                if slave > 2:
                    os.close(slave)
                env = dict(os.environ, TERM="alacritty", COLORTERM="truecolor")
                if trace.enabled:
                    env.setdefault("ZSH_PROFILE_STARTUP", "1")
                    env.setdefault("ZSH_PROFILE_OUT", f"{env['AMUX_TRACE_DIR']}/zsh-{os.getpid()}.prof")
                os.execvpe(shell[0], [*shell, "-i"], env)
            except BaseException as error:
                try:
                    os.write(2, f"terminal launch failed: {error}\n".encode())
                finally:
                    os._exit(127)
        os.close(slave)
        os.set_blocking(master, False)
        self.fd = master
        self.pid = pid
        self.connection = None
        self.stream = None
        self.stream_trace = None
        self.writer = None
        self.presented = False
        self.presentation = None
        self.needs_checkpoint = True
        self.output_drain = None
        self.closed = False
        self.running = True
        self.columns, self.rows = 80, 24
        self.lock = asyncio.Lock()
        self.input = asyncio.Queue()
        self.gone = gone
        self.focus = focus
        self.misrouted = None
        self.trace, self.pane = trace, pane
        self.prompt_probe = b"" if trace.enabled else None
        self.first_read = True
        try:
            self.state = State()
        except BaseException:
            os.close(self.fd)
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            raise
        self.pump = asyncio.create_task(self.output())
        self.input_task = asyncio.create_task(self.inputs())
        self.cleanup = None

    def directory(self):
        return os.open(f"/proc/{self.pid}/cwd", os.O_PATH | os.O_DIRECTORY)

    async def state_call(self, kind, data=b""):
        replies, checkpoint = await self.state.call(kind, data)
        if replies:
            if self.prompt_probe is not None:
                self.trace.emit("terminal_reply_queued", pane=self.pane, bytes=len(replies))
            self.input.put_nowait(replies)
        return checkpoint

    async def resize(self, columns, rows):
        async with self.lock:
            if (columns, rows) == (self.columns, self.rows):
                return
            await self.state_call(b"R", json.dumps([columns, rows]).encode())
            self.needs_checkpoint = True
            fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
            self.columns, self.rows = columns, rows
            writer = self.writer
            if writer is not None:
                checkpoint = await self.state_call(b"L")
                if self.writer is writer:
                    writer.write(frame(b"C", checkpoint))
                    self.needs_checkpoint = False

    def live_frame(self, data):
        descriptor = json.dumps(self.state.descriptor, separators=(",", ":")).encode()
        return frame(b"U", struct.pack("!I", len(descriptor)) + descriptor + data)

    async def history(self, read):
        async with self.lock:
            if not self.presented:
                raise ValueError("pane is not visible")
            if not isinstance(read, dict):
                raise ValueError("history read must be an object")
            self.trace.emit("history_read_begin", pane=self.pane, projection=read.get("projection"), kind=read.get("kind"))
            data = await self.state_call(b"H", json.dumps(read).encode())
            if not self.presented:
                raise ValueError("pane is not visible")
            self.trace.emit("history_read_end", pane=self.pane, bytes=len(data))
            return json.loads(data)

    async def capture(self, history):
        async with self.lock:
            data = json.loads(await self.state_call(b'T', json.dumps(history).encode()))
            if "error" in data:
                raise ValueError(data["error"])
            return data["text"]

    async def clear_history(self):
        async with self.lock:
            checkpoint = await self.state_call(b'X')
            self.needs_checkpoint = True
            if self.writer is not None:
                self.writer.write(frame(b'C', checkpoint))
                self.needs_checkpoint = False

    def set_presented(self, presented, trace_id=None):
        if presented and not self.presented:
            self.stream_trace = trace_id
        self.presented = presented
        if not presented:
            self.pause()
        self.resume_if_needed()

    def pause(self):
        if self.writer is not None:
            self.writer.write(frame(b"P", b""))
        self.writer = None
        if self.output_drain is not None:
            self.output_drain.cancel()

    def resume_if_needed(self):
        if (self.presented and self.stream is not None and self.writer is None
                and self.presentation is None and not self.closed):
            self.presentation = asyncio.create_task(self.resume(self.stream))

    async def resume(self, writer):
        identity = self.stream_trace
        try:
            async with self.lock:
                if not self.presented or self.stream is not writer:
                    return
                if self.needs_checkpoint:
                    self.trace.emit("terminal_checkpoint_begin", pane=self.pane, trace_id=identity)
                    checkpoint = await self.state_call(b"L")
                    self.trace.emit("terminal_checkpoint_end", pane=self.pane, trace_id=identity,
                                    bytes=len(checkpoint))
                    if not self.presented or self.stream is not writer:
                        return
                    writer.write(frame(b"C", checkpoint))
                    self.needs_checkpoint = False
                    self.trace.emit("terminal_checkpoint_queued", pane=self.pane, trace_id=identity)
                self.writer = writer
            await writer.drain()
        except ConnectionError:
            if self.stream is writer:
                self.disconnect()
        except OSError:
            if not self.closed:
                self.gone()
        finally:
            self.presentation = None
            self.resume_if_needed()

    def check_input(self, data):
        if self.focus is None or data.startswith((b"\x1b[<", b"\x1b[M", b"\x1b[I", b"\x1b[O")):
            return
        focus, since = self.focus()
        if focus is None or focus == self.pane:
            self.misrouted = None
        elif self.misrouted != focus and time.monotonic() - since >= .5:
            self.misrouted = focus
            print(f"amux server: input for focused pane {focus} reached pane {self.pane} ({len(data)} bytes)",
                  file=sys.stderr, flush=True)

    async def inputs(self):
        try:
            while True:
                data = await self.input.get()
                await write_fd(self.fd, data)
                if self.prompt_probe is not None:
                    self.trace.emit("terminal_input_written", pane=self.pane, bytes=len(data))
        except OSError:
            if not self.closed:
                self.gone()

    async def output(self):
        first = True
        try:
            while True:
                try:
                    deadline = self.state.sync_deadline
                    if deadline is None:
                        data = await read_fd(self.fd)
                    else:
                        timeout = max(0, deadline - asyncio.get_running_loop().time())
                        data = await asyncio.wait_for(read_fd(self.fd), timeout)
                except TimeoutError:
                    async with self.lock:
                        checkpoint = await self.state_call(b"D")
                        if self.writer is None:
                            self.needs_checkpoint = True
                        elif checkpoint:
                            self.writer.write(frame(b"C", checkpoint))
                    continue
                if not data:
                    break
                if self.first_read:
                    self.trace.emit("terminal_pty_first", pane=self.pane, bytes=len(data))
                    self.first_read = False
                if self.prompt_probe is not None:
                    probe = self.prompt_probe + data
                    if re.search(rb"\x1b\]133;A(?:;[^\x07\x1b]{0,256})?(?:\x07|\x1b\\)", probe):
                        self.trace.emit("terminal_prompt_first", pane=self.pane)
                        self.prompt_probe = None
                    else:
                        self.prompt_probe = probe[-300:]
                async with self.lock:
                    checkpoint = await self.state_call(b"D", data)
                    writer = self.writer
                    if first:
                        self.trace.emit("terminal_output_first", pane=self.pane, bytes=len(data))
                        first = False
                    if writer is not None:
                        writer.write(frame(b"C", checkpoint) if checkpoint else self.live_frame(data))
                    else:
                        self.needs_checkpoint = True
                if writer is not None:
                    self.output_drain = asyncio.create_task(writer.drain())
                    try:
                        await self.output_drain
                    except asyncio.CancelledError:
                        if asyncio.current_task().cancelling():
                            raise
                    except ConnectionError:
                        if self.writer is writer:
                            self.disconnect()
                    finally:
                        self.output_drain = None
            if not self.closed:
                self.gone()
        except OSError:
            if not self.closed:
                self.gone()

    def disconnect(self):
        self.stream = None
        self.pause()
        if self.presentation is not None and not self.presentation.cancelling():
            self.presentation.cancel()
        if self.connection is not None:
            if self.connection is not asyncio.current_task():
                self.connection.cancel()
            self.connection = None

    def close(self):
        if self.closed:
            return self.cleanup
        self.closed = True
        tasks = [self.pump, self.input_task]
        if self.connection is not None:
            tasks.append(self.connection)
        if self.presentation is not None:
            tasks.append(self.presentation)
        self.stream = None
        self.writer = None
        self.connection = None
        for task in tasks:
            task.cancel()
        self.state.stop()
        if self.running:
            for target in (os.killpg, os.kill):
                try:
                    target(self.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        async def finish():
            await asyncio.gather(*tasks, return_exceptions=True)
            os.close(self.fd)
            self.state.close()
        self.cleanup = asyncio.create_task(finish())
        return self.cleanup

    async def bridge(self, reader, writer, reply):
        writer.write((json.dumps(reply, separators=(",", ":")) + "\n").encode())
        self.stream = writer
        self.needs_checkpoint = True
        self.stream_trace = reply.get("_trace")
        self.resume_if_needed()
        await writer.drain()
        while data := await reader.read(65536):
            self.check_input(data)
            self.input.put_nowait(data)
