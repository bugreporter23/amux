import asyncio
import os
import json
import struct
import subprocess

from .terminal_io import ready, write_fd


def frame(kind, data):
    return kind + struct.pack("!I", len(data)) + data


class State:
    def __init__(self):
        binary = os.environ.get("AMUX_TERMINAL_STATE")
        if not binary:
            raise OSError("AMUX_TERMINAL_STATE must name the packaged terminal-state executable")
        self.process = subprocess.Popen([binary],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.input = self.process.stdin.fileno()
        self.output = self.process.stdout.fileno()
        os.set_blocking(self.input, False)
        os.set_blocking(self.output, False)
        self.sync_deadline = None
        self.descriptor = None

    async def read(self, size):
        data = bytearray()
        while len(data) < size:
            try:
                chunk = os.read(self.output, min(size - len(data), 65536))
            except BlockingIOError:
                await ready(self.output)
                continue
            if not chunk:
                raise OSError("terminal state process exited")
            data.extend(chunk)
        return bytes(data)

    async def call(self, kind, data=b""):
        transaction = asyncio.create_task(self.exchange(kind, data))
        try:
            return await asyncio.shield(transaction)
        except asyncio.CancelledError:
            try:
                await transaction
            except OSError:
                pass
            raise

    async def exchange(self, kind, data):
        await write_fd(self.input, frame(kind, data))
        replies = await self.read(struct.unpack("!I", await self.read(4))[0])
        checkpoint = await self.read(struct.unpack("!I", await self.read(4))[0])
        timeout = struct.unpack("!I", await self.read(4))[0]
        self.sync_deadline = asyncio.get_running_loop().time() + timeout / 1000 if timeout else None
        self.descriptor = json.loads(await self.read(struct.unpack("!I", await self.read(4))[0]))
        return replies, checkpoint

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.wait()
