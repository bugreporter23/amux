{ pkgs, amux }:
let
  desktop = pkgs.makeDesktopItem {
    name = "amux";
    desktopName = "Amux";
    comment = "Persistent application panes and terminal sessions";
    exec = "amux";
    icon = "utilities-terminal";
    terminal = false;
    categories = [ "System" "TerminalEmulator" ];
    startupWMClass = "amux";
  };
in amux.overrideAttrs (old: {
  buildCommand = old.buildCommand + ''
    install -Dm644 ${desktop}/share/applications/amux.desktop "$out/share/applications/amux.desktop"
  '';
  meta = (old.meta or { }) // {
    description = "Persistent multiplexer and presentation manager for application panes";
    license = pkgs.lib.licenses.mit;
    platforms = [ "x86_64-linux" "aarch64-linux" ];
    mainProgram = "amux";
  };
})
