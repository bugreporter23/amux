from dataclasses import asdict, dataclass
from fractions import Fraction


class CommandError(ValueError):
    pass


def integer(value, name):
    if type(value) is not int:
        raise CommandError(f"{name} must be an integer")
    return value


def label(value):
    if not isinstance(value, str) or not value.strip() or not value.isprintable():
        raise CommandError("name must be nonempty printable text")
    return value.strip()


@dataclass
class Leaf:
    pane: int


@dataclass
class Split:
    axis: str
    first: object
    second: object
    ratio: Fraction = Fraction(1, 2)


@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    width: int
    height: int


def minimum(node):
    if node is None:
        return 0, 0
    if isinstance(node, Leaf):
        return 1, 1
    aw, ah = minimum(node.first)
    bw, bh = minimum(node.second)
    return (aw + bw, max(ah, bh)) if node.axis == "h" else (max(aw, bw), ah + bh)


def placements(node, rect):
    if node is None:
        return
    yield node, rect
    if isinstance(node, Leaf):
        return
    axis = 0 if node.axis == "h" else 1
    extent = rect.width if axis == 0 else rect.height
    first = max(minimum(node.first)[axis],
                min(int(extent * node.ratio), extent - minimum(node.second)[axis]))
    if axis == 0:
        a = Rect(rect.x, rect.y, first, rect.height)
        b = Rect(rect.x + first, rect.y, rect.width - first, rect.height)
    else:
        a = Rect(rect.x, rect.y, rect.width, first)
        b = Rect(rect.x, rect.y + first, rect.width, rect.height - first)
    yield from placements(node.first, a)
    yield from placements(node.second, b)


def replace(node, pane, replacement):
    if isinstance(node, Leaf):
        return replacement if node.pane == pane else node
    node.first = replace(node.first, pane, replacement)
    node.second = replace(node.second, pane, replacement)
    if node.first is None:
        return node.second
    if node.second is None:
        return node.first
    return node


def ancestors(node, pane):
    if isinstance(node, Leaf):
        return [] if node.pane == pane else None
    for first, child in ((True, node.first), (False, node.second)):
        path = ancestors(child, pane)
        if path is not None:
            return [(node, first), *path]
    return None


def tree(node):
    if node is None:
        return None
    if isinstance(node, Leaf):
        return {"pane": node.pane}
    return {"axis": node.axis, "ratio": [node.ratio.numerator, node.ratio.denominator],
            "first": tree(node.first), "second": tree(node.second)}


class Workspace:
    def __init__(self, identity, allocate_pane, width, height):
        self.identity = identity
        self.name = f"workspace {identity}"
        self.names = {}
        self.allocate_pane = allocate_pane
        self.root = None
        self.focus = None
        self.zoomed = False
        self.viewport(width, height)
        self.root = Leaf(self.allocate_pane())
        self.focus = self.root.pane
        self.names[self.focus] = f"pane {self.focus}"

    def viewport(self, width, height):
        width, height = integer(width, "width"), integer(height, "height")
        minimum_width, minimum_height = minimum(self.root)
        if width < max(1, minimum_width) or height < max(1, minimum_height):
            raise CommandError("viewport is too small for the layout")
        self.width, self.height = width, height

    def placed(self):
        return list(placements(self.root, Rect(0, 0, self.width, self.height)))

    def leaves(self):
        return [(node, rect) for node, rect in self.placed() if isinstance(node, Leaf)]

    def snapshot(self):
        return {"workspace": self.identity,
                "name": self.name,
                "viewport": {"width": self.width, "height": self.height},
                "focus": self.focus, "zoomed": self.zoomed, "layout": tree(self.root),
                "panes": [{"id": node.pane,
                           "name": self.names[node.pane],
                           "visible": not self.zoomed or node.pane == self.focus,
                           "rect": asdict(Rect(0, 0, self.width, self.height)
                                          if self.zoomed and node.pane == self.focus else rect)}
                          for node, rect in self.leaves()]}

    def target(self, request):
        pane = integer(request.get("pane", self.focus), "pane")
        for leaf, rect in self.leaves():
            if leaf.pane == pane:
                return leaf, rect
        raise CommandError(f"unknown pane {pane}")

    def split(self, request):
        axis = request.get("axis")
        if axis not in ("h", "v"):
            raise CommandError("axis must be h or v")
        leaf, rect = self.target(request)
        if (rect.width if axis == "h" else rect.height) < 2:
            raise CommandError("pane is too small to split on this axis")
        new = Leaf(self.allocate_pane())
        self.names[new.pane] = f"pane {new.pane}"
        self.root = replace(self.root, leaf.pane, Split(axis, leaf, new))
        self.focus = new.pane

    def close(self, request):
        leaf, _ = self.target(request)
        order = [node.pane for node, _ in self.leaves()]
        index = order.index(leaf.pane)
        order.remove(leaf.pane)
        self.root = replace(self.root, leaf.pane, None)
        del self.names[leaf.pane]
        if self.focus == leaf.pane:
            self.zoomed = False
            self.focus = order[min(index, len(order) - 1)] if order else None

    def zoom(self):
        self.target({})
        self.zoomed = not self.zoomed

    def focus_pane(self, request):
        if "direction" not in request:
            self.focus = self.target(request)[0].pane
            return
        neighbor = self.neighbor(self.focus, request["direction"])
        self.zoomed = False
        if neighbor is not None:
            self.focus = neighbor

    def neighbor(self, pane, direction):
        _, current = self.target({"pane": pane})
        if direction in ("next", "prev"):
            order = [leaf.pane for leaf, _ in self.leaves()]
            return order[(order.index(pane) + (1 if direction == "next" else -1)) % len(order)]
        if direction not in ("left", "right", "up", "down"):
            raise CommandError("direction must be left, right, up, down, next or prev")
        horizontal = direction in ("left", "right")
        origin = current.x if horizontal else current.y
        extent = current.width if horizontal else current.height
        cross = current.y if horizontal else current.x
        span = current.height if horizontal else current.width
        candidates = []
        for leaf, rect in self.leaves():
            start = rect.x if horizontal else rect.y
            size = rect.width if horizontal else rect.height
            other_cross = rect.y if horizontal else rect.x
            other_span = rect.height if horizontal else rect.width
            gap = (start - origin - extent if direction in ("right", "down")
                   else origin - start - size)
            overlap = min(cross + span, other_cross + other_span) - max(cross, other_cross)
            if gap >= 0 and overlap > 0:
                distance = abs(2 * cross + span - 2 * other_cross - other_span)
                candidates.append((gap, distance, leaf.pane))
        if candidates:
            return min(candidates)[2]
        return None

    def move(self, request):
        leaf, _ = self.target(request)
        neighbor = self.neighbor(leaf.pane, request.get("direction"))
        if neighbor is None:
            return
        other, _ = self.target({"pane": neighbor})
        leaf.pane, other.pane = other.pane, leaf.pane

    def resize(self, request):
        axis = request.get("axis")
        if axis not in ("h", "v"):
            raise CommandError("axis must be h or v")
        pixels = integer(request.get("pixels"), "pixels")
        leaf, _ = self.target(request)
        path = ancestors(self.root, leaf.pane)
        split = next(((node, first) for node, first in reversed(path) if node.axis == axis), None)
        if split is None:
            raise CommandError("pane has no split on this axis")
        node, first = split
        rect = next(rect for placed, rect in self.placed() if placed is node)
        index = 0 if axis == "h" else 1
        extent = rect.width if axis == "h" else rect.height
        first_size = max(minimum(node.first)[index],
                         min(int(extent * node.ratio), extent - minimum(node.second)[index]))
        new_size = first_size + (pixels if first else -pixels)
        if not minimum(node.first)[index] <= new_size <= extent - minimum(node.second)[index]:
            raise CommandError("resize would leave a subtree too small")
        node.ratio = Fraction(new_size, extent)

    def apply(self, request):
        if not isinstance(request, dict):
            raise CommandError("request must be an object")
        fields = {"state": set(), "zoom": set(), "split": {"axis", "pane"},
                  "pane_rename": {"pane", "name"},
                  "close": {"pane"}, "focus": {"pane", "direction"},
                  "move": {"pane", "direction"},
                  "resize": {"axis", "pixels", "pane"}, "viewport": {"width", "height"}}
        operation = request.get("op")
        if not isinstance(operation, str) or operation not in fields:
            raise CommandError("unknown operation")
        if set(request) - {"op"} - fields[operation]:
            raise CommandError("unexpected request fields")
        if operation == "focus" and "pane" in request and "direction" in request:
            raise CommandError("focus takes a pane or direction, not both")
        if operation == "pane_rename":
            leaf, _ = self.target(request)
            self.names[leaf.pane] = label(request.get("name"))
        elif operation == "zoom":
            self.zoom()
        elif operation == "split":
            self.split(request)
        elif operation == "close":
            self.close(request)
        elif operation == "focus":
            self.focus_pane(request)
        elif operation == "resize":
            self.resize(request)
        elif operation == "move":
            self.move(request)
        elif operation == "viewport":
            self.viewport(request.get("width"), request.get("height"))
        return self.snapshot()


class Session:
    def __init__(self, identity, allocate_workspace, allocate_pane, width, height):
        self.identity = identity
        self.name = f"session {identity}"
        self.allocate_workspace = allocate_workspace
        self.allocate_pane = allocate_pane
        self.workspaces = {}
        self.active = None
        self.previous = None
        self.next_number = 1
        self.viewport(width, height)
        self.new_workspace()

    @property
    def current(self):
        return self.workspaces.get(self.active)

    def new_workspace(self):
        workspace = Workspace(self.allocate_workspace(), self.allocate_pane, self.width, self.height)
        number = self.next_number
        workspace.name = f"workspace {number}"
        self.next_number += 1
        self.workspaces[number] = workspace
        self.select(number)

    def select(self, number):
        number = integer(number, "workspace number")
        if number not in self.workspaces:
            raise CommandError(f"unknown workspace number {number}")
        if number != self.active:
            self.previous, self.active = self.active, number

    def cycle(self, delta):
        if self.active is None:
            raise CommandError("session has no workspaces; use workspace new")
        numbers = list(self.workspaces)
        self.select(numbers[(numbers.index(self.active) + delta) % len(numbers)])

    def close_workspace(self, number):
        number = integer(number, "workspace number")
        if number not in self.workspaces:
            raise CommandError(f"unknown workspace number {number}")
        numbers = list(self.workspaces)
        index = numbers.index(number)
        del self.workspaces[number]
        numbers.remove(number)
        if self.active == number:
            self.active = numbers[min(index, len(numbers) - 1)] if numbers else None
        if self.previous in (number, self.active):
            self.previous = None

    def last_workspace(self):
        if self.previous not in self.workspaces:
            raise CommandError("no previous workspace")
        return self.workspaces[self.previous]

    def transfer(self, request, joining):
        axis = request.get("axis", "v")
        if axis not in ("h", "v"):
            raise CommandError("axis must be h or v")
        pane = request.get("pane")
        if pane is None:
            workspace = self.last_workspace() if joining else self.current
            pane = workspace.focus if workspace else None
        pane = integer(pane, "pane")
        source = self.workspace_for(pane)
        if source is None:
            raise CommandError(f"unknown pane {pane}")
        number = self.active if joining else integer(request.get("number"), "workspace number")
        destination = self.workspaces.get(number)
        if destination is None:
            raise CommandError(f"unknown workspace number {number}")
        if source is destination:
            raise CommandError("pane is already in the destination workspace")
        target, rect = destination.target({"pane": request.get("target", destination.focus)})
        if (rect.width if axis == "h" else rect.height) < 2:
            raise CommandError("destination pane is too small to split on this axis")
        name = source.names[pane]
        self.apply_workspace(source, {"op": "close", "pane": pane})
        destination.root = replace(destination.root, target.pane, Split(axis, target, Leaf(pane)))
        destination.names[pane] = name
        destination.focus = pane
        self.select(number)

    def viewport(self, width, height):
        width, height = integer(width, "width"), integer(height, "height")
        if width < 1 or height < 1:
            raise CommandError("viewport dimensions must be positive")
        for workspace in self.workspaces.values():
            minimum_width, minimum_height = minimum(workspace.root)
            if width < minimum_width or height < minimum_height:
                raise CommandError("viewport is too small for a workspace layout")
        self.width, self.height = width, height
        for workspace in self.workspaces.values():
            workspace.viewport(width, height)

    def workspace_for(self, pane):
        return next((workspace for workspace in self.workspaces.values()
                     if any(leaf.pane == pane for leaf, _ in workspace.leaves())), None)

    def apply_workspace(self, workspace, request):
        workspace.apply(request)
        if workspace.root is None:
            number = next(number for number, item in self.workspaces.items() if item is workspace)
            self.close_workspace(number)

    def snapshot(self):
        return {"session": self.identity,
                "name": self.name,
                "viewport": {"width": self.width, "height": self.height},
                "active_workspace": self.current.identity if self.current else None,
                "workspaces": [{"number": number, **workspace.snapshot()}
                               for number, workspace in self.workspaces.items()]}

    def apply(self, request):
        if not isinstance(request, dict):
            raise CommandError("request must be an object")
        operation = request.get("op")
        fields = {"state": set(), "viewport": {"width", "height"},
                  "session_rename": {"name"}, "workspace_rename": {"name", "number"},
                  "workspace_new": set(), "workspace_next": set(), "workspace_prev": set(),
                  "workspace_last": set(), "join": {"pane", "axis", "target"},
                  "pane_move": {"number", "pane", "axis", "target"},
                  "workspace_select": {"number"}, "workspace_close": {"number"}}
        if isinstance(operation, str) and operation in fields:
            if set(request) - {"op"} - fields[operation]:
                raise CommandError("unexpected request fields")
            if operation == "viewport":
                self.viewport(request.get("width"), request.get("height"))
            elif operation == "session_rename":
                self.name = label(request.get("name"))
            elif operation == "workspace_rename":
                number = integer(request.get("number", self.active), "workspace number")
                if number not in self.workspaces:
                    raise CommandError(f"unknown workspace number {number}")
                self.workspaces[number].name = label(request.get("name"))
            elif operation == "workspace_new":
                self.new_workspace()
            elif operation == "workspace_select":
                self.select(request.get("number"))
            elif operation == "workspace_last":
                self.last_workspace()
                self.select(self.previous)
            elif operation in ("join", "pane_move"):
                self.transfer(request, operation == "join")
            elif operation in ("workspace_next", "workspace_prev"):
                self.cycle(1 if operation == "workspace_next" else -1)
            elif operation == "workspace_close":
                self.close_workspace(request.get("number", self.active))
        else:
            if self.current is None:
                raise CommandError("session has no workspaces; use workspace new")
            self.apply_workspace(self.current, request)
        return self.snapshot()
