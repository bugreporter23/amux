"""Socket suggestions from Amux's runtime directories, without session requests."""

import os
from pathlib import Path
import re
import stat

from . import DEFAULT_SOCKET_NAME


def guest_socket():
    runtime = Path("/tmp") / f"amux-{os.getuid()}"
    runtime.mkdir(mode=0o700, exist_ok=True)
    info = runtime.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise OSError("unsafe guest mux runtime directory")
    return str(runtime / DEFAULT_SOCKET_NAME)


def catalog(remote=False):
    from .launcher import default_socket
    default = guest_socket() if remote else default_socket()
    directories = {Path(default).parent, Path("/tmp") / f"amux-{os.getuid()}"}
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime and os.path.isabs(runtime):
        directories.add(Path(runtime) / "amux")
    rows = [{"socket": "", "name": "Default server", "path": default, "source": "default"}]
    warnings = []
    for directory in sorted(directories):
        try:
            info = directory.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o022:
                continue
            for entry in sorted(directory.iterdir()):
                info = entry.lstat()
                if (not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or str(entry) == default
                        or len(str(entry)) > 511 or not str(entry).isprintable()):
                    continue
                row = {"socket": str(entry), "path": str(entry), "name": entry.name, "source": "runtime"}
                hint = re.fullmatch(r"protocol-(\d+)\.sock", entry.name)
                if hint:
                    row["protocol_hint"] = int(hint[1])
                rows.append(row)
                if len(rows) >= 256:
                    return rows, "; ".join(warnings)
        except FileNotFoundError:
            pass
        except OSError as error:
            warnings.append(f"{directory}: {error}")
    return rows, "; ".join(warnings)


def validate_catalog(value):
    if not isinstance(value, list) or len(value) > 256:
        raise ValueError("invalid remote socket catalog")
    for row in value:
        if not isinstance(row, dict):
            raise ValueError("invalid remote socket entry")
        for key in ("socket", "path", "name", "source"):
            text = row.get(key)
            if not isinstance(text, str) or len(text) > 511 or (text and not text.isprintable()) or (key != "socket" and not text):
                raise ValueError("invalid remote socket entry")
        if row["socket"] and not os.path.isabs(row["socket"]):
            raise ValueError("invalid remote socket path")
        if not os.path.isabs(row["path"]):
            raise ValueError("invalid remote socket path")
        if "protocol_hint" in row and (type(row["protocol_hint"]) is not int or row["protocol_hint"] < 0):
            raise ValueError("invalid remote protocol hint")
    return value
