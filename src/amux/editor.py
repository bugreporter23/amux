import asyncio
import json
import os
import signal


class Editor:
    """A server-owned headless Neovim; graphical views attach over its RPC socket."""

    kind = "editor"

    def __init__(self, command, address, cwd):
        self.address = address
        pid = os.fork()
        if pid == 0:
            try:
                os.fchdir(cwd)
                os.setsid()
                null = os.open(os.devnull, os.O_RDWR)
                for target in (0, 1, 2):
                    os.dup2(null, target)
                env = dict(os.environ)
                for name in ("NVIM", "NVIM_LISTEN_ADDRESS", "WAYLAND_DISPLAY", "DISPLAY"):
                    env.pop(name, None)
                os.execvpe(command[0], [*command, "--headless", "--listen", address], env)
            except BaseException:
                os._exit(127)
        self.pid = pid
        self.running = True
        self.closed = False
        self.cleanup = None
        self.bridges = set()

    def directory(self):
        return os.open(f"/proc/{self.pid}/cwd", os.O_PATH | os.O_DIRECTORY)

    def set_presented(self, presented, trace_id=None):
        pass

    def disconnect(self):
        for task in list(self.bridges):
            task.cancel()

    async def bridge(self, reader, writer, reply):
        task = asyncio.current_task()
        self.bridges.add(task)
        remote = None
        try:
            deadline = asyncio.get_running_loop().time() + 3
            while remote is None:
                try:
                    remote = await asyncio.open_unix_connection(self.address)
                except (FileNotFoundError, ConnectionRefusedError):
                    if not self.running or asyncio.get_running_loop().time() >= deadline:
                        raise OSError("editor is not listening")
                    await asyncio.sleep(.01)
            writer.write((json.dumps(reply, separators=(",", ":")) + "\n").encode())
            await writer.drain()
            remote_reader, remote_writer = remote

            async def copy(source, destination):
                while data := await source.read(65536):
                    destination.write(data)
                    await destination.drain()
                if destination.can_write_eof():
                    destination.write_eof()

            copies = [asyncio.create_task(copy(reader, remote_writer)),
                      asyncio.create_task(copy(remote_reader, writer))]
            try:
                await asyncio.wait(copies, return_when=asyncio.FIRST_EXCEPTION)
            finally:
                for copying in copies:
                    copying.cancel()
                await asyncio.gather(*copies, return_exceptions=True)
        finally:
            self.bridges.discard(task)
            if remote is not None:
                remote[1].close()

    def close(self):
        if self.closed:
            return self.cleanup
        self.closed = True
        self.disconnect()
        if self.running:
            try:
                os.kill(self.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

        async def finish():
            for _ in range(200):
                if not self.running:
                    break
                await asyncio.sleep(.01)
            else:
                try:
                    os.killpg(self.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            try:
                os.unlink(self.address)
            except FileNotFoundError:
                pass
        self.cleanup = asyncio.create_task(finish())
        return self.cleanup
