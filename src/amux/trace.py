import json
import os
from pathlib import Path
import sys
import time


class Trace:
    def __init__(self, component):
        self.component = component
        self.pid = os.getpid()
        self.sequence = 0
        self.fd = None
        directory = os.environ.get("AMUX_TRACE_DIR")
        if directory:
            Path(directory).mkdir(mode=0o700, parents=True, exist_ok=True)
            self.fd = os.open(Path(directory) / f"{component}-{self.pid}.jsonl",
                              os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_CLOEXEC, 0o600)
            self.emit("process_start")

    @property
    def enabled(self):
        return self.fd is not None

    def request_id(self):
        if not self.enabled:
            return None
        self.sequence += 1
        return f"{self.pid}:{self.sequence}"

    def emit(self, event, *, at_ns=None, **fields):
        if not self.enabled:
            return
        record = {"ts_ns": at_ns if at_ns is not None else time.monotonic_ns(),
                  "component": self.component, "pid": self.pid, "event": event, **fields}
        data = (json.dumps(record, separators=(",", ":")) + "\n").encode()
        try:
            while data:
                data = data[os.write(self.fd, data):]
        except OSError as error:
            os.close(self.fd)
            self.fd = None
            print(f"trace disabled: {error}", file=sys.stderr)


def take_id(message):
    identity = message.pop("_trace", None)
    if identity is not None:
        if (not isinstance(identity, str) or len(identity) > 64
                or len(identity.split(":")) != 2
                or not all(part.isascii() and part.isdecimal() for part in identity.split(":"))):
            raise ValueError("_trace must be a pid:sequence identifier")
    return identity


def report(directory):
    from collections import defaultdict
    import math
    import statistics

    paths = sorted(Path(directory).glob("*.jsonl"))
    if not paths:
        raise OSError("no trace files found")
    requests = defaultdict(dict)
    terminals = defaultdict(dict)
    processes = defaultdict(dict)
    components = set()
    feedback = []
    for path in paths:
        for line in path.read_text().splitlines():
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise OSError(f"incomplete or invalid trace in {path}") from error
            components.add(record["component"])
            processes[record["component"], record["pid"]][record["event"]] = record
            if record["component"] == "server" and "pane" in record:
                terminals[record["pid"], record["pane"]][record["event"]] = record
            if record.get("trace_id"):
                requests[record["trace_id"]][record["event"]] = record
            if record["event"] == "output_feedback":
                feedback.append(record)
    print("Components: " + ", ".join(sorted(components)))
    print(f"{'stage':27} {'op':14} {'n':>5} {'p50 ms':>10} {'p95 ms':>10} {'max ms':>10}")
    samples = defaultdict(list)
    slow = []
    pairs = [("command queue", "command_enqueued", "command_written"),
             ("socket round trip", "command_written", "reply_received"),
             ("socket round trip", "request_send", "reply_received"),
             ("server decision", "request_received", "decision_complete"),
             ("reply serialization/queue", "decision_complete", "reply_queued"),
             ("state application", "state_apply_begin", "state_applied"),
             ("PTY creation", "terminal_create_begin", "terminal_create_end"),
             ("terminal checkpoint", "terminal_checkpoint_begin", "terminal_checkpoint_end"),
             ("window launch to map", "window_launch_begin", "surface_mapped")]
    for events in terminals.values():
        for label, end in (("PTY to first read", "terminal_pty_first"),
                           ("PTY to prompt marker", "terminal_prompt_first")):
            if "terminal_create_begin" in events and end in events:
                samples[label, "shell startup"].append(
                    (events[end]["ts_ns"] - events["terminal_create_begin"]["ts_ns"]) / 1e6)
    for events in processes.values():
        for label, start, end in (("transport to greeting", "remote_launch_begin", "remote_greeting"),
                                  ("guest server startup", "server_start_begin", "server_start_end")):
            if start in events and end in events:
                samples[label, "attachment"].append((events[end]["ts_ns"] - events[start]["ts_ns"]) / 1e6)
    for identity, events in requests.items():
        operation = next((event["op"] for event in events.values() if "op" in event), "unknown")
        for label, start, end in pairs:
            if start in events and end in events:
                elapsed = (events[end]["ts_ns"] - events[start]["ts_ns"]) / 1e6
                if elapsed >= 0:
                    samples[label, operation].append(elapsed)
        enqueued = events.get("command_enqueued", {})
        if enqueued.get("input_ns") and "state_applied" in events:
            elapsed = (events["state_applied"]["ts_ns"] - enqueued["input_ns"]) / 1e6
            samples["input to state applied", operation].append(elapsed)
            slow.append((elapsed, identity, operation))
    for (label, operation), values in sorted(samples.items()):
        values.sort()
        p95 = values[math.ceil(len(values) * .95) - 1]
        print(f"{label:27} {operation:14} {len(values):5} {statistics.median(values):10.3f} {p95:10.3f} {values[-1]:10.3f}")
    for elapsed, identity, operation in sorted(slow, reverse=True)[:5]:
        print(f"Slow input: {identity} {operation} {elapsed:.3f} ms")
    if feedback:
        timestamped = sum(bool(event["when_ns"]) and event["presented"] for event in feedback)
        print(f"Output feedback: {len(feedback)} events, {timestamped} with presentation timestamps")
    print("State application and window mapping do not establish visible terminal output.")
