"""Build an SSH transport using the execution environment's SSH command."""

import fcntl
import glob
import json
import os
from pathlib import Path
import re
import shlex
import tempfile


def identity_path(value):
    if not isinstance(value, str) or len(value) > 511 or (value and not value.isprintable()):
        raise ValueError("SSH identity must be an absolute printable path")
    if value and not os.path.isabs(value):
        raise ValueError("SSH identity must be an absolute path")
    return os.path.normpath(value) if value else ""


def identities(directory=None):
    """Suggest filenames only; OpenSSH owns reading and unlocking key material."""
    directory = Path.home() / ".ssh" if directory is None else Path(directory)
    try:
        entries = list(directory.iterdir())
    except FileNotFoundError:
        return []
    paths = set()
    for entry in entries:
        name = entry.name
        if name.endswith("-cert.pub"):
            continue
        if name.endswith(".pub"):
            private = entry.with_name(name[:-4])
            candidate = private if private.is_file() else entry
        elif name.startswith("id_") and "." not in name:
            candidate = entry
        else:
            continue
        if candidate.is_file():
            try:
                paths.add(identity_path(str(candidate.absolute())))
            except ValueError:
                continue
    return [{"name": Path(path).name, "identity": path} for path in sorted(paths)]


def command(destination, ssh, identity="", *, browse=False):
    if (not isinstance(destination, str) or not destination or len(destination) > 511
            or destination.startswith("-")
            or any(char.isspace() or not char.isprintable() for char in destination)):
        raise ValueError("SSH destination must be a host, user@host or SSH URI")
    identity = identity_path(identity)
    options = ["-i", identity, "-o", "IdentitiesOnly=yes", "-o", "PreferredAuthentications=publickey"] if identity else []
    return [*ssh, *options, "-T", "--", destination, "exec amux " + ("route-stdio" if browse else "serve-stdio")]


class Targets:
    """Optional SSH suggestions and local connection history, independent of auth."""

    def __init__(self, history=None, config=None):
        state = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
        self.history = Path(history) if history is not None else state / "amux/ssh-history.json"
        self.config = Path(config) if config is not None else Path.home() / ".ssh/config"

    def recent(self):
        try:
            records = json.loads(self.history.read_text())
        except FileNotFoundError:
            return []
        if not isinstance(records, list):
            raise ValueError("invalid SSH history")
        result, seen = [], set()
        for record in records[:100]:
            if not isinstance(record, dict):
                raise ValueError("invalid SSH history entry")
            destination = record.get("destination")
            identity = identity_path(record.get("identity", ""))
            command(destination, ["ssh"], identity)
            if (destination, identity) in seen:
                continue
            seen.add((destination, identity))
            result.append({"destination": destination})
            if identity:
                result[-1]["identity"] = identity
        return result

    def aliases(self):
        visited, result = set(), []

        def read(path):
            path = path.resolve()
            if path in visited or len(visited) >= 128:
                return
            visited.add(path)
            try:
                text = path.read_text()
            except FileNotFoundError:
                return
            for line in text.splitlines():
                match = re.match(r"\s*(Host|Include)(?:\s*=\s*|\s+)(.*)$", line, re.I)
                if not match:
                    continue
                args = shlex.split(match[2], comments=True)
                if match[1].lower() == "include":
                    for pattern in args:
                        expanded = Path(os.path.expanduser(pattern))
                        if not expanded.is_absolute():
                            expanded = self.config.parent / expanded
                        for included in sorted(glob.glob(str(expanded))):
                            read(Path(included))
                else:
                    for alias in args:
                        if any(char in alias for char in "*?!["):
                            continue
                        command(alias, ["ssh"])
                        if alias not in result:
                            result.append(alias)
        read(self.config)
        return result

    def suggestions(self):
        records, errors = [], []
        try:
            for record in self.recent():
                name = record["destination"]
                records.append({**record, "name": name, "source": "recent"})
        except (OSError, ValueError) as error:
            errors.append(f"SSH history: {error}")
        try:
            destinations = {record["destination"] for record in records}
            records.extend({"destination": alias, "name": alias, "source": "alias"}
                           for alias in self.aliases() if alias not in destinations)
        except (OSError, ValueError) as error:
            errors.append(f"SSH aliases: {error}")
        return records, "; ".join(errors)

    def remember(self, destination, identity=""):
        self.history.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        lock = os.open(str(self.history) + ".lock", os.O_CREAT | os.O_RDWR, 0o600)
        with os.fdopen(lock, "w") as stream:
            fcntl.flock(stream, fcntl.LOCK_EX)
            record = {"destination": destination}
            if identity_path(identity):
                record["identity"] = identity_path(identity)
            records = [record, *(entry for entry in self.recent() if entry != record)][:100]
            fd, temporary = tempfile.mkstemp(prefix=".ssh-history-", dir=self.history.parent)
            try:
                with os.fdopen(fd, "w") as output:
                    json.dump(records, output, ensure_ascii=False)
                    output.write("\n")
                os.replace(temporary, self.history)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
