{
  description = "amux: always multiplexing, with server-owned sessions and local presentation";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/34ca302a9572963c02e385c056be37c85ff51b77";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          version = pkgs.lib.removeSuffix "\n" (builtins.readFile ./src/amux/VERSION);
          alacritty = pkgs.alacritty.overrideAttrs (old: {
            cargoDeps = pkgs.runCommand "amux-alacritty-vendor" { nativeBuildInputs = [ pkgs.python3 ]; } ''
              cp -R ${pkgs.alacritty.cargoDeps} "$out"
              chmod -R u+w "$out"
              cd "$out/source-registry-0/vte-0.15.0"
              patch -p1 < ${./patches/vte-checkpoint.patch}
              python3 - <<'PY'
              import hashlib, json
              from pathlib import Path
              path = Path('.cargo-checksum.json')
              checksums = json.loads(path.read_text())
              for name in ('src/lib.rs', 'src/params.rs', 'src/ansi.rs'):
                  checksums['files'][name] = hashlib.sha256(Path(name).read_bytes()).hexdigest()
              path.write_text(json.dumps(checksums))
              PY
            '';
            patches = (old.patches or [ ]) ++ [
              ./patches/alacritty-hide-unfocused-cursor.patch
              ./patches/alacritty-window-timings.patch
              ./patches/alacritty-batch-glyph-preload.patch
              ./patches/alacritty-terminal-checkpoint.patch
              ./patches/alacritty-history.patch
              ./patches/alacritty-copy-view.patch
              ./patches/alacritty-replica-resize.patch
            ];
            postPatch = (old.postPatch or "") + ''
              cp ${./src/terminal-state/checkpoint.rs} alacritty_terminal/src/term/checkpoint.rs
              cp ${./src/alacritty-copy.rs} alacritty/src/amux_copy.rs
              cp ${./src/terminal-state/copy.rs} alacritty_terminal/src/term/copy.rs
              cp ${./src/terminal-state/history.rs} alacritty_terminal/src/term/history.rs
              cp ${./src/terminal-state/grid.rs} alacritty_terminal/src/grid/checkpoint.rs
              mkdir -p alacritty_terminal/src/bin
              cp ${./src/terminal-state/main.rs} alacritty_terminal/src/bin/amux-terminal-state.rs
            '';
            postBuild = (old.postBuild or "") + ''
              cargo build --offline --locked --release --package alacritty_terminal --bin amux-terminal-state
            '';
            postInstall = (old.postInstall or "") + ''
              install -Dm755 target/${pkgs.stdenv.hostPlatform.rust.rustcTarget}/release/amux-terminal-state "$out/bin/amux-terminal-state"
            '';
          });
          terminal-state = pkgs.runCommand "amux-terminal-state" { nativeBuildInputs = [ pkgs.patchelf ]; } ''
            install -Dm755 ${alacritty}/bin/amux-terminal-state "$out/bin/amux-terminal-state"
            patchelf --shrink-rpath "$out/bin/amux-terminal-state"
          '';
          server = pkgs.writeShellApplication {
            name = "amux";
            runtimeInputs = [ pkgs.python3 ];
            text = ''
              export PYTHONPATH=${./src}
              export PYTHONDONTWRITEBYTECODE=1
              export AMUX_ENTRY="$0"
              export AMUX_TERMINAL_STATE=${terminal-state}/bin/amux-terminal-state
              export TERMINFO_DIRS=${pkgs.lib.getOutput "terminfo" alacritty}/share/terminfo:''${TERMINFO_DIRS:-}
              exec python3 -m amux "$@"
            '';
          };
          wlroots = pkgs.wlroots.overrideAttrs (old: {
            patches = (old.patches or [ ]) ++ [ ./patches/wlroots-transparent-scene-background.patch ];
          });
          compositor = pkgs.stdenv.mkDerivation {
            pname = "amux-client";
            inherit version;
            src = ./src/compositor;
            nativeBuildInputs = [ pkgs.pkg-config pkgs.wayland-scanner ];
            buildInputs = [ wlroots pkgs.wayland pkgs.wayland-protocols pkgs.libxkbcommon pkgs.json_c pkgs.pixman pkgs.libdrm pkgs.pango pkgs.cairo ];
            buildPhase = ''
              activation_xml=${pkgs.wayland-protocols}/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml
              wayland-scanner client-header "$activation_xml" xdg-activation-v1-client-protocol.h
              wayland-scanner private-code "$activation_xml" xdg-activation-v1-protocol.c
              cc -std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -Wall -Wextra -Wno-unused-parameter -I. \
                client.c manager-state.c xdg-activation-v1-protocol.c -o amux-client $(pkg-config --cflags --libs wlroots-0.20 wayland-server wayland-client xkbcommon json-c pixman-1 libdrm pangocairo)
            '';
            installPhase = ''
              install -Dm755 amux-client "$out/bin/amux-client"
              install -Dm644 LICENSE "$out/share/licenses/amux-client/LICENSE"
            '';
          };
          amux = pkgs.writeShellApplication {
            name = "amux";
            runtimeInputs = [ pkgs.python3 ];
            text = ''
              export PYTHONPATH=${./src}
              export PYTHONDONTWRITEBYTECODE=1
              export AMUX_COMPOSITOR=${compositor}/bin/amux-client
              export AMUX_ALACRITTY=${alacritty}/bin/alacritty
              export AMUX_ALACRITTY_CONFIG=${./src/alacritty.toml}
              export AMUX_ENTRY="$0"
              export AMUX_TERMINAL_STATE=${terminal-state}/bin/amux-terminal-state
              export TERMINFO_DIRS=${pkgs.lib.getOutput "terminfo" alacritty}/share/terminfo:''${TERMINFO_DIRS:-}
              exec python3 -m amux "$@"
            '';
          };
          amux-mesa = import ./integration/mesa.nix { inherit pkgs amux; };
        in { inherit alacritty terminal-state server amux amux-mesa compositor; default = amux; });

      apps = forSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.amux}/bin/amux";
        };
        amux = {
          type = "app";
          program = "${self.packages.${system}.amux}/bin/amux";
        };
      });
    };
}
