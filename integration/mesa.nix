{ pkgs, amux }:
let
  fonts = pkgs.makeFontsConf {
    fontDirectories = [ pkgs.nerd-fonts.jetbrains-mono pkgs.dejavu_fonts ];
  };
  wrapper = pkgs.writeShellApplication {
    name = "amux";
    text = ''
      export AMUX_GUI_MESA=${pkgs.mesa}
      export AMUX_GUI_FONTCONFIG=${fonts}
      exec ${amux}/bin/amux "$@"
    '';
  };
in import ../nix/desktop.nix { inherit pkgs; amux = wrapper; }
