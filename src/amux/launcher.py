import fcntl
import os
from pathlib import Path
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

from . import DEFAULT_SOCKET_NAME, PROTOCOL_VERSION

def launch(action):
    state = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "amux"
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    log, log_path = tempfile.mkstemp(prefix="gui-", suffix=".log", dir=state)
    reader, writer = os.pipe()
    pid = os.fork()
    if pid:
        os.close(log)
        os.close(writer)
        try:
            started = os.read(reader, 1) == b"R"
        finally:
            os.close(reader)
        if started:
            print(f"amux: launched; log: {log_path}")
            return 0
        _, status = os.waitpid(pid, 0)
        print(f"amux: startup failed; see {log_path}", file=sys.stderr)
        return os.waitstatus_to_exitcode(status) or 1

    os.close(reader)
    try:
        os.setsid()
        with open(os.devnull, "rb") as source:
            os.dup2(source.fileno(), 0)
        os.dup2(log, 1)
        os.dup2(log, 2)
        os.close(log)

        def ready():
            nonlocal writer
            os.write(writer, b"R")
            os.close(writer)
            writer = None

        return action(ready)
    finally:
        if writer is not None:
            os.close(writer)


def default_socket():
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime or not os.path.isabs(runtime):
        raise OSError("default socket requires an absolute XDG_RUNTIME_DIR")
    directory = Path(runtime) / "amux"
    directory.mkdir(mode=0o700, exist_ok=True)
    return str(directory / DEFAULT_SOCKET_NAME)


def listening(path, starting=False):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(1)
        try:
            connection.connect(path)
        except FileNotFoundError:
            return False
        except ConnectionRefusedError as error:
            if starting:
                return False
            raise OSError(f"socket is not listening: {path}; clear it with amux prune --socket <path>") from error
    return True


def verify_server(path):
    from .client import request
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(3)
        connection.connect(path)
        with connection.makefile("rwb") as stream:
            greeting = request(stream, {"op": "hello"})
    if not greeting.get("ok"):
        raise OSError(greeting.get("error", "server handshake failed"))
    if type(greeting.get("protocol")) is not int or greeting["protocol"] != PROTOCOL_VERSION:
        raise OSError(f"server protocol mismatch: local {PROTOCOL_VERSION}, server {greeting.get('protocol')}")
    if not isinstance(greeting.get("version"), str) or not isinstance(greeting.get("incarnation"), str):
        raise OSError("invalid server identity")
    return greeting


def prune(path):
    path = Path(path).absolute()
    with open(str(path) + ".lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            original = path.lstat()
        except FileNotFoundError:
            return
        if not stat.S_ISSOCK(original.st_mode):
            raise OSError(f"refusing to prune a non-socket: {path}")
        process = None
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(1)
            try:
                connection.connect(str(path))
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            else:
                pid, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                if uid != os.getuid() or pid <= 0 or pid == os.getpid():
                    raise OSError("refusing to stop a socket peer owned by another user or this process")
                try:
                    process = struct.unpack("i", connection.getsockopt(socket.SOL_SOCKET, 77, 4))[0]
                except ProcessLookupError:
                    pass
        if process is not None:
            try:
                try:
                    signal.pidfd_send_signal(process, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                if not select.select([process], [], [], 3)[0]:
                    raise OSError("server did not stop within three seconds; socket retained")
            finally:
                os.close(process)
        try:
            current = path.lstat()
        except FileNotFoundError:
            return
        if (current.st_dev, current.st_ino) != (original.st_dev, original.st_ino):
            raise OSError("socket changed during prune; replacement retained")
        path.unlink()


def ensure_server(path, server_config=None):
    path = os.path.abspath(path)
    with open(path + ".lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if listening(path):
            return
        log_path = path + ".log"
        command = [sys.executable, "-m", "amux", "server", "--socket", path]
        if server_config is not None:
            command += ["--config", str(Path(server_config).absolute())]
        with open(log_path, "ab") as log:
            process = subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                start_new_session=True,
            )
        deadline = time.monotonic() + 3
        while process.poll() is None:
            if listening(path, starting=True):
                return
            if time.monotonic() >= deadline:
                process.terminate()
                process.wait(timeout=1)
                raise OSError(f"server startup timed out; see {log_path}")
            time.sleep(0.02)
        raise OSError(f"server startup failed (exit {process.returncode}); see {log_path}")
