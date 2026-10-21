# Launcher environment: `nix-shell launcher/shell.nix --run 'python3 launcher/sacredfire_launcher.py'`
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    (python3.withPackages (ps: [ ps.pygobject3 ]))
    gtk4 libadwaita gobject-introspection
  ];
}
