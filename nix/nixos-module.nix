{ amuxPackages }:
{ config, lib, pkgs, ... }:
let
  cfg = config.programs.amux;
in {
  options.programs.amux = {
    enable = lib.mkEnableOption "Amux, a persistent application pane multiplexer";

    package = lib.mkOption {
      type = lib.types.package;
      default = amuxPackages.${pkgs.stdenv.hostPlatform.system}.amux;
      defaultText = lib.literalExpression "amux.packages.\${pkgs.stdenv.hostPlatform.system}.amux";
      description = "Amux package to install.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];
  };
}
