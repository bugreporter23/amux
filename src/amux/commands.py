import shlex

HELP = """state | zoom | split h|v [pane] | close [pane] | edit [pane]
focus <pane|left|right|up|down|next|prev> | resize h|v <pixels> [pane]
move left|right|up|down [pane]
move window <number> [h|v] [pane] | join [pane] [h|v]
window new|next|prev|last|select <number>|close [number]
session new [name]|last|select <id> | rename session|window|pane <name>
clear-history [pane] | capture [pane] [history-rows]
viewport <width> <height> | detach | destroy | help"""


def parse(line):
    parts = shlex.split(line)
    if not parts:
        return None
    operation, *arguments = parts
    if operation in ("state", "zoom", "detach", "destroy", "help") and not arguments:
        return {"op": operation}
    if operation == "split" and len(arguments) in (1, 2):
        result = {"op": operation, "axis": arguments[0]}
        if len(arguments) == 2:
            result["pane"] = int(arguments[1])
        return result
    if operation in ("close", "edit") and len(arguments) in (0, 1):
        return {"op": operation, **({"pane": int(arguments[0])} if arguments else {})}
    if operation in ("clear-history", "capture") and len(arguments) <= (2 if operation == "capture" else 1):
        result = {"op": operation.replace("-", "_")}
        if arguments:
            result["pane"] = int(arguments[0])
        if len(arguments) == 2:
            result["history"] = int(arguments[1])
        return result
    if operation == "focus" and len(arguments) == 1:
        field = ("direction" if arguments[0] in ("left", "right", "up", "down", "next", "prev") else "pane")
        value = arguments[0] if field == "direction" else int(arguments[0])
        return {"op": operation, field: value}
    if operation == "resize" and len(arguments) in (2, 3):
        result = {"op": operation, "axis": arguments[0], "pixels": int(arguments[1])}
        if len(arguments) == 3:
            result["pane"] = int(arguments[2])
        return result
    if operation == "move" and arguments and arguments[0] == "window" and len(arguments) in (2, 3, 4):
        result = {"op": "pane_move", "number": int(arguments[1])}
        if len(arguments) >= 3:
            result["axis"] = arguments[2]
        if len(arguments) == 4:
            result["pane"] = int(arguments[3])
        return result
    if operation == "join" and len(arguments) <= 2:
        result = {"op": "join"}
        if arguments:
            result["pane"] = int(arguments[0])
        if len(arguments) == 2:
            result["axis"] = arguments[1]
        return result
    if operation == "move" and len(arguments) in (1, 2):
        result = {"op": operation, "direction": arguments[0]}
        if len(arguments) == 2:
            result["pane"] = int(arguments[1])
        return result
    if operation == "viewport" and len(arguments) == 2:
        return {"op": operation, "width": int(arguments[0]), "height": int(arguments[1])}
    if operation == "rename" and len(arguments) == 2 and arguments[0] in ("session", "window", "pane"):
        return {"op": arguments[0] + "_rename", "name": arguments[1]}
    if operation == "session" and arguments:
        if len(arguments) == 2 and arguments[0] == "new":
            return {"op": "session_new", "name": arguments[1]}
        if arguments in (["new"], ["last"]):
            return {"op": "session_" + arguments[0]}
        if len(arguments) == 2 and arguments[0] == "select":
            return {"op": "session_select", "session": int(arguments[1])}
    if operation == "window" and arguments:
        action, *values = arguments
        if action in ("new", "next", "prev", "last") and not values:
            return {"op": f"window_{action}"}
        if action == "select" and len(values) == 1:
            return {"op": "window_select", "number": int(values[0])}
        if action == "close" and len(values) <= 1:
            return {"op": "window_close", **({"number": int(values[0])} if values else {})}
    raise ValueError("invalid command; use help")
