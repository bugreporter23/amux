import asyncio
import json
import os
import shlex
import signal
import sys
import tempfile

from . import envelope


async def present(path, key, pane):
    """Run Neovide as the graphical view of a server-owned editor pane.

    Neovide connects to a private local socket; each connection is relayed through
    the amux server socket, which may itself be a stdio transport bridge.
    """
    command = shlex.split(os.environ.get("AMUX_NEOVIDE", "neovide"))
    connections = set()

    async def relay(local_reader, local_writer):
        task = asyncio.current_task()
        connections.add(task)
        remote_writer = None
        try:
            remote_reader, remote_writer = await asyncio.open_unix_connection(path)
            remote_writer.write((json.dumps(envelope({"op": "editor", "key": key, "pane": pane}),
                                            separators=(",", ":")) + "\n").encode())
            await remote_writer.drain()
            reply = json.loads(await remote_reader.readline() or b'{"ok":false,"error":"server disconnected"}')
            if not reply["ok"]:
                raise OSError(reply["error"])

            async def copy(source, destination):
                while data := await source.read(65536):
                    destination.write(data)
                    await destination.drain()
                if destination.can_write_eof():
                    destination.write_eof()

            copies = [asyncio.create_task(copy(local_reader, remote_writer)),
                      asyncio.create_task(copy(remote_reader, local_writer))]
            try:
                await asyncio.wait(copies, return_when=asyncio.FIRST_EXCEPTION)
            finally:
                for copying in copies:
                    copying.cancel()
                await asyncio.gather(*copies, return_exceptions=True)
        except (OSError, ValueError) as error:
            print(f"amux editor view: {error}", file=sys.stderr, flush=True)
        finally:
            connections.discard(task)
            local_writer.close()
            if remote_writer is not None:
                remote_writer.close()

    with tempfile.TemporaryDirectory(prefix="editor-", dir=os.environ.get("XDG_RUNTIME_DIR")) as runtime:
        address = os.path.join(runtime, "nvim.sock")
        listener = await asyncio.start_unix_server(relay, address)
        os.chmod(address, 0o600)
        process = await asyncio.create_subprocess_exec(
            *command, "--server", address, "--wayland_app_id", f"amux-pane-{pane}",
            "--frame", "none", "--no-fork", stdin=asyncio.subprocess.DEVNULL)
        loop = asyncio.get_running_loop()
        for signum in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(signum, process.terminate)
        try:
            return await process.wait()
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()
            listener.close()
            for task in list(connections):
                task.cancel()
            await asyncio.gather(*connections, return_exceptions=True)


def run(path, key, pane):
    return asyncio.run(present(path, key, pane))
