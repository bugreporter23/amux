import copy
import json
import os
from pathlib import Path
import tomllib

from .commands import parse


def read(kind, path):
    config = tomllib.loads((Path(__file__).parent.parent / f"{kind}.toml").read_text())
    user_path = (Path(path) if path is not None else
                 Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / f"amux/{kind}.toml")
    try:
        supplied = tomllib.loads(user_path.read_text())
    except FileNotFoundError:
        if path is not None:
            raise
        supplied = {}
    unknown = supplied.keys() - config.keys()
    if unknown:
        raise ValueError(f"unknown amux {kind} setting: {sorted(unknown)[0]}")
    for key, value in supplied.items():
        if key == "bindings":
            if not isinstance(value, dict) or value.keys() - config[key].keys():
                raise ValueError("bindings must contain normal, prefix or resize tables")
            for mode, bindings in value.items():
                if not isinstance(bindings, dict):
                    raise ValueError(f"bindings.{mode} must be a table")
                config[key][mode].update(bindings)
        else:
            config[key] = value
    return config


def load_server(path=None):
    config = read("server", path)
    for key, command in config.items():
        if (not isinstance(command, list) or not command
                or any(not isinstance(arg, str) or not arg or "\0" in arg for arg in command)):
            raise ValueError(f"{key} must be a nonempty command argument array")
    return config


def load_client(path=None):
    config = read("client", path)
    if not isinstance(config["prefix"], str) or not config["prefix"]:
        raise ValueError("prefix must be a key combination")
    notify = config["notify"]
    if not isinstance(notify, list) or any(not isinstance(arg, str) or not arg or "\0" in arg for arg in notify):
        raise ValueError("notify must be an argument array (empty disables notifications)")
    for key in ("repeat_rate", "repeat_delay"):
        if type(config[key]) is not int or config[key] < 0 or config[key] > 2**31 - 1:
            raise ValueError(f"{key} must be a nonnegative 32-bit integer")
    normalized = copy.deepcopy(config)
    if not isinstance(config["connections"], list):
        raise ValueError("connections must be an array of tables")
    names = set()
    for connection in normalized["connections"]:
        if (not isinstance(connection, dict) or set(connection) - {"name", "socket", "transport"}
                or ("socket" in connection) == ("transport" in connection)):
            raise ValueError("connection requires name and exactly one of socket or transport")
        name = connection.get("name")
        if not isinstance(name, str) or not name.strip() or not name.isprintable() or name in names:
            raise ValueError("connection names must be unique nonempty printable text")
        names.add(name)
        if "socket" in connection:
            value = connection["socket"]
            if not isinstance(value, str) or not value:
                raise ValueError("connection socket must be a path")
            connection["socket"] = os.path.abspath(os.path.expanduser(os.path.expandvars(value)))
        else:
            value = connection["transport"]
            if not isinstance(value, list) or not value or any(not isinstance(v, str) or not v or "\0" in v for v in value):
                raise ValueError("connection transport must be a nonempty command argument array")
    for mode, bindings in config["bindings"].items():
        normalized["bindings"][mode] = {}
        for key, text in bindings.items():
            if not isinstance(text, str):
                raise ValueError(f"binding {key} must be a command string")
            if not text:
                continue
            if text.startswith("ui "):
                action = text[3:]
                if action not in ("windows", "sessions", "connections", "command", "session-name", "window-name", "pane-name", "help"):
                    raise ValueError(f"unknown UI action: {action}")
                command = {"ui": action}
            elif text.startswith("mode "):
                action = text[5:]
                if action not in ("normal", "resize"):
                    raise ValueError(f"unknown manager mode: {action}")
                command = {"mode": action}
            else:
                command = parse(text)
                if not command:
                    raise ValueError(f"binding {key} has no command")
            normalized["bindings"][mode][key] = command
    return json.dumps(normalized, ensure_ascii=False)
