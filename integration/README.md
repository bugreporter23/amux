# Linux Mesa integration

On a Linux Wayland desktop with Nix installed:

```sh
./integration/run
```

Nix must be able to use `nix-command` and `flakes`. The script enables those
features for its build, retains the result at
`${XDG_STATE_HOME:-$HOME/.local/state}/amux-integration/mesa`, and launches it.
Pass ordinary amux arguments, for example `./integration/run --version`.

The package includes the application libraries, Mesa from the same nixpkgs pin,
and font configuration.
The execution machine supplies the shell selected in amux config.
The host must already supply a working Wayland session, Linux GPU driver and
firmware, and permission to use the relevant `/dev/dri/renderD*` device.

Driver and font selection applies to the compositor and Alacritty. Amux starts
processes in the calling user's Wayland session. The graphical processes use
the packaged application libraries and selected graphics provider.

Use `#amux` with an externally configured graphics provider and `#amux-mesa`
with packaged Mesa. See [dependencies](../DEPENDENCIES.md) for provider selection.
