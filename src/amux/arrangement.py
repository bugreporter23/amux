"""Validate a complete, staged arrangement before changing live sessions."""

from .model import CommandError, integer, label, minimum


def snapshot(sessions):
    return [{"session": session.identity, "name": session.name,
             "windows": [{"window": window.identity, "name": window.name}
                         for window in session.windows.values()]}
            for session in sessions.values()]


def prepare(sessions, base, proposed, viewport=None):
    if base != snapshot(sessions):
        raise CommandError("session/window arrangement changed; reload before applying")
    if not isinstance(proposed, list):
        raise CommandError("arrangement must be a session array")
    windows = {window.identity: window for session in sessions.values() for window in session.windows.values()}
    seen_sessions, seen_windows, plan = set(), set(), []
    for item in proposed:
        if not isinstance(item, dict) or set(item) != {"session", "name", "windows"}:
            raise CommandError("each session requires session, name and windows")
        identity = integer(item["session"], "session")
        if identity in seen_sessions or (identity >= 0 and identity not in sessions):
            raise CommandError("sessions must occur exactly once")
        seen_sessions.add(identity)
        session = sessions[identity] if identity >= 0 else identity
        if isinstance(session, int):
            if viewport is None:
                raise CommandError("new sessions require a destination viewport")
            destination_width, destination_height = viewport
        else:
            destination_width, destination_height = session.width, session.height
        name = label(item["name"])
        if not isinstance(item["windows"], list):
            raise CommandError("windows must be an array")
        ordered = []
        for entry in item["windows"]:
            if not isinstance(entry, dict) or set(entry) != {"window", "name"}:
                raise CommandError("each window requires window and name")
            identity = integer(entry["window"], "window")
            if identity in seen_windows or (identity >= 0 and identity not in windows):
                raise CommandError("windows must occur exactly once; copying is not supported")
            seen_windows.add(identity)
            window = windows[identity] if identity >= 0 else identity
            window_name = label(entry["name"])
            width, height = (1, 1) if isinstance(window, int) else minimum(window.root)
            if width > destination_width or height > destination_height:
                raise CommandError("destination viewport is too small for a window layout")
            ordered.append((window, window_name))
        plan.append((session, name, ordered))
    if not sessions.keys() <= seen_sessions:
        raise CommandError("every session must occur exactly once")
    return plan
