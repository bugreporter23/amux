"""Machine-local endpoint bookmarks; editing them never contacts a server."""

import fcntl
import json
import os
from pathlib import Path
import re
import tempfile
import uuid

from .ssh import command, identity_path


def socket_path(value):
    if not isinstance(value, str) or not value or len(value) > 511 or not value.isprintable():
        raise ValueError("socket endpoint must be a printable path")
    if value.startswith("unix://"):
        value = value[7:]
    value = os.path.expanduser(os.path.expandvars(value))
    if not os.path.isabs(value):
        raise ValueError("socket endpoint must be an absolute path")
    return os.path.normpath(value)


class Bookmarks:
    def __init__(self, path=None):
        state = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
        self.path = Path(path) if path is not None else state / "amux/connections.json"

    @staticmethod
    def validate(rows, existing=None):
        if not isinstance(rows, list) or len(rows) > 1000:
            raise ValueError("connection bookmarks must be an array of at most 1000 entries")
        result, seen = [], set()
        for row in rows:
            required = {"connection", "name", "kind", "address"}
            if not isinstance(row, dict) or not required <= set(row) or set(row) - required - {"identity"}:
                raise ValueError("bookmark requires connection, name, kind and address")
            identity = row["connection"]
            fresh = type(identity) is int and identity < 0 and existing is not None
            if not fresh and (not isinstance(identity, str) or not re.fullmatch(r"[0-9a-f]{32}", identity)
                              or (existing is not None and identity not in existing)):
                raise ValueError("unknown connection bookmark")
            if identity in seen:
                raise ValueError("connection bookmarks must occur at most once")
            seen.add(identity)
            name, kind, address = (row[key] for key in ("name", "kind", "address"))
            identity_file = identity_path(row.get("identity", ""))
            if not isinstance(name, str) or not name.strip() or not name.isprintable() or len(name) > 511:
                raise ValueError("name every connection with printable text")
            if kind == "ssh":
                command(address, ["ssh"])
            elif kind == "socket":
                address = socket_path(address)
                if identity_file:
                    raise ValueError("socket bookmarks have an empty SSH identity")
            else:
                raise ValueError("connection kind must be ssh or socket")
            result.append({"connection": uuid.uuid4().hex if fresh else identity,
                           "name": name, "kind": kind, "address": address})
            if identity_file:
                result[-1]["identity"] = identity_file
        return result

    def read(self):
        try:
            rows = json.loads(self.path.read_text())
            if isinstance(rows, list):
                rows = [{key: value for key, value in row.items() if key != "amux"}
                        if isinstance(row, dict) else row for row in rows]
            return self.validate(rows)
        except FileNotFoundError:
            return []

    def apply(self, base, rows):
        base = self.validate(base)
        proposed = self.validate(rows, {row["connection"] for row in base})
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        lock = os.open(str(self.path) + ".lock", os.O_CREAT | os.O_RDWR, 0o600)
        with os.fdopen(lock, "w") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX)
            if self.read() != base:
                raise ValueError("connection bookmarks changed; reload before applying")
            fd, temporary = tempfile.mkstemp(prefix=".connections-", dir=self.path.parent)
            try:
                with os.fdopen(fd, "w") as output:
                    json.dump(proposed, output, ensure_ascii=False)
                    output.write("\n")
                os.replace(temporary, self.path)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
        return proposed
