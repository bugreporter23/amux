import asyncio
import json
import os
import sys
from pathlib import Path
import tempfile

from .trace import Trace
from .config import load_client
from .connections import Connections
from . import PROTOCOL_VERSION, __version__

def graphics_report(env):
    selectors = {key: env[key] for key in (
        "__EGL_VENDOR_LIBRARY_FILENAMES", "__EGL_VENDOR_LIBRARY_DIRS",
        "GBM_BACKENDS_PATH", "GBM_BACKEND", "WLR_RENDERER",
        "LD_LIBRARY_PATH", "LD_PRELOAD", "MESA_LOADER_DRIVER_OVERRIDE", "DRI_PRIME",
    ) if key in env}
    defaults = {}
    if not any(key in env for key in ("__EGL_VENDOR_LIBRARY_FILENAMES", "__EGL_VENDOR_LIBRARY_DIRS")):
        defaults["egl_vendor_dirs"] = ["/run/opengl-driver/share/glvnd/egl_vendor.d",
                                      "/etc/glvnd/egl_vendor.d", "/usr/share/glvnd/egl_vendor.d"]
    if "GBM_BACKENDS_PATH" not in env:
        defaults["gbm_backend_dir"] = "/run/opengl-driver/lib/gbm"
    print("amux graphics configuration: " + json.dumps({"display": env["WAYLAND_DISPLAY"],
          "selectors": selectors, "packaged_loader_defaults": defaults}), file=sys.stderr, flush=True)


def run(path, width, height, session=None, config=None, start_server=False, ready=None, pick_connection=False,
        server_config=None):
    return asyncio.run(run_async(path, width, height, session, config, start_server, ready, pick_connection,
                                server_config=server_config))


async def run_async(path, width, height, session=None, config=None, start_server=False, ready=None,
                    pick_connection=False, initial_transport=None, server_config=None):
    trace = Trace("gui")
    parent_runtime = os.environ.get("XDG_RUNTIME_DIR")
    parent_display = os.environ.get("WAYLAND_DISPLAY")
    if not parent_runtime or not parent_display:
        raise OSError("gui requires a Wayland desktop and XDG_RUNTIME_DIR")
    if width < 1 or height < 1:
        raise OSError("viewport dimensions must be positive")
    if not parent_display.startswith("/"):
        parent_display = str(Path(parent_runtime) / parent_display)
    with tempfile.TemporaryDirectory(prefix="amux-frontend.", dir=parent_runtime) as runtime:
        manager_config = load_client(config)
        initial = ({"name": "Current remote", "transport": initial_transport} if initial_transport is not None
                   else {"name": "Current local", "socket": os.path.abspath(path)})
        settings = json.loads(manager_config)
        connections = Connections(settings["connections"], initial, start_server, server_config,
                                  ssh_command=settings["ssh"])
        env = dict(os.environ, AMUX_VERSION=__version__, XDG_RUNTIME_DIR=runtime, WAYLAND_DISPLAY=parent_display,
                   AMUX_HOST_RUNTIME_DIR=parent_runtime, AMUX_HOST_WAYLAND_DISPLAY=parent_display,
                   AMUX_HOST_X_DISPLAY=os.environ.get("DISPLAY", ""),
                   AMUX_PROTOCOL_VERSION=str(PROTOCOL_VERSION),
                   AMUX_AL_SOCKET=runtime + "/alacritty.sock",
                   AMUX_MANAGER_CONFIG=manager_config)
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_SOCKET", None)
        mesa = os.environ.get("AMUX_GUI_MESA")
        if mesa:
            manifest = Path(mesa) / "share/glvnd/egl_vendor.d/50_mesa.json"
            backend = Path(mesa) / "lib/gbm/dri_gbm.so"
            missing = [str(path) for path in (manifest, backend) if not path.is_file()]
            if missing:
                raise OSError("AMUX_GUI_MESA is missing: " + ", ".join(missing))
            env["__EGL_VENDOR_LIBRARY_FILENAMES"] = str(manifest)
            env["GBM_BACKENDS_PATH"] = str(backend.parent)
            env["GBM_BACKEND"] = "dri"
            env["WLR_RENDERER"] = "gles2"
        if fontconfig := os.environ.get("AMUX_GUI_FONTCONFIG"):
            env["FONTCONFIG_FILE"] = fontconfig
        if env.get("AMUX_GRAPHICS_DEBUG") == "1":
            graphics_report(env)
        listener = process = None
        try:
            pick_session = False
            if not pick_connection:
                selected = await connections.select(connections.initial)
                pick_session = session is None and bool(selected["sessions"])
            trace.emit("frontend_launch")
            args = [env["AMUX_COMPOSITOR"], "--socket", connections.path or "", "--control", runtime + "/connections.sock",
                    "--width", str(width), "--height", str(height)]
            if pick_connection:
                args += ["--pick-connection"]
            elif pick_session:
                args += ["--pick-session"]
            if session is not None:
                args += ["--session", str(session)]
            listener = await asyncio.start_unix_server(connections.accept, runtime + "/connections.sock")
            process = await asyncio.create_subprocess_exec(*args, env=env)
            if ready is not None:
                ready()
            status = await process.wait()
            if status:
                print(f"amux frontend exited with status {status}", file=sys.stderr)
                graphics_report(env)
            return status
        finally:
            if listener is not None:
                listener.close()
                await listener.wait_closed()
            if process is not None and process.returncode is None:
                process.terminate()
                await process.wait()
            await connections.close()
