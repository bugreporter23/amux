# Dependencies and runtime boundaries

The Nix build pins the application libraries. The outer desktop compositor
communicates with Amux through Wayland.

| Packaged component | Responsibility |
| --- | --- |
| Patched Alacritty 0.17.0 and VTE 0.15.0 | Terminal rendering, parser/checkpoint state and copy mode |
| Patched wlroots 0.20.2 | Inner Wayland server, rendering, allocation and outer Wayland client |
| Wayland and libxkbcommon | Protocol transport and keyboard-layout interpretation |
| json-c, pixman, Cairo/Pango, Fontconfig/FreeType/HarfBuzz | Messages, regions, CPU image surfaces and text/font handling |
| libdrm | Linux graphics-device interfaces |
| GLVND | EGL/OpenGL vendor loading and dispatch |
| libgbm | Buffer allocation/sharing through a separately loaded backend |
| Python and native terminal-state helper | Server, transport, PTYs and authoritative terminal state |

Alacritty and wlroots use Amux's patches.
The wlroots headers and library must be the same patched build. Compiler/language
requirements are Rust/Cargo, C11 with GNU extensions and Python, supplied by Nix.
The nixpkgs pin selects the build dependencies.

## Two external interfaces

```text
Alacritty --pane buffers/input--> Amux --one window/buffers/input--> outer Wayland desktop

Alacritty OpenGL/EGL --GLVND--> graphics provider --Linux kernel driver--> GPU
Amux wlroots GLES/EGL/GBM --GLVND/libgbm--> graphics provider
```

The outer compositor communicates through Wayland. It may use a different
wlroots version, Smithay or another implementation. Its graphics libraries run
in its own process. Submitted buffers need
compatible devices, formats, modifiers and synchronization.

The graphics provider is loaded into Amux's rendering processes. Its architecture,
required library symbols, EGL vendor interface, GBM backend interface and GPU/kernel
support must be compatible with the packaged application libraries.

## Provider selection

`#amux-mesa` packages Mesa 26.2.3 from the application pin, libgbm 26.1.3 and fonts.
The GUI launch selects that Mesa's EGL manifest and GBM backend and the
wlroots GLES2 renderer. Servers, shells and transport commands retain the
caller's graphics environment.

`#amux` leaves provider selection external. Its reference loaders discover:

```text
/run/opengl-driver/share/glvnd/egl_vendor.d/   EGL manifests
/run/opengl-driver/lib/gbm/                   GBM backends
```

GLVND also supports other discovery locations and explicit overrides. Configure
the EGL vendor and GBM backend together for the rendering processes.

libgbm and its backend must support their negotiated backend ABI and required
buffer operations. Keep the selected Mesa EGL, GBM/DRI backend and rendering
implementation coordinated. GLVND supplies vendor loading and dispatch; libdrm
supplies Linux graphics-device interfaces. See [GLVND's vendor enumeration](https://github.com/NVIDIA/libglvnd/blob/v1.7.0/src/EGL/icd_enumeration.md)
and [Mesa's implementation ABI](https://docs.mesa3d.org/egl.html#packaging).

## Host requirements

Linux, the selected CPU architecture, GPU kernel driver/firmware, render-node
permissions and an existing Wayland session remain host requirements. The
reference hardware path needs `wl_compositor` ≥4, `xdg_wm_base` ≥1, `wl_seat` ≥5,
`wl_data_device_manager` ≥3 and DMA-BUF feedback via `zwp_linux_dmabuf_v1` ≥4,
usable buffer formats/modifiers and transparent ARGB8888 output. It needs ordinary
account data and writable runtime/cache directories.

Builds are selected by CPU architecture. The Mesa wrapper requires a
Mesa-supported GPU. `#amux` uses an externally configured graphics provider.

For startup diagnostics, run `AMUX_GRAPHICS_DEBUG=1 amux gui --foreground`.
Amux reports configured EGL/GBM selectors and enables existing renderer logs;
those logs identify the initialized implementation. Failed initialization names
the stage and reports provider configuration. The background launcher prints
its log path.
