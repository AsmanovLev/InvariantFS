# packaging/nix/module.nix — NixOS module for InvariantFS
{ config, lib, pkgs, ... }:

with lib;

let
  cfg = config.programs.invariantfs;
  invfsPkg = pkgs.callPackage ./default.nix {};
in {
  options.programs.invariantfs = {
    enable = mkEnableOption "InvariantFS filesystem engine and user-space utilities";

    package = mkOption {
      type = types.package;
      default = invfsPkg;
      description = "The InvariantFS package to use.";
    };
  };

  config = mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];

    # Ensure fuse kernel module is loaded
    boot.kernelModules = [ "fuse" ];
  };
}
