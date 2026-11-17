# Standalone native launcher: nix-shell launcher/shell.nix --run 'bash launcher/build.sh'
{ pkgs ? import <nixpkgs> {}, launcherBuild ? "" }:
pkgs.mkShell {
  packages = (with pkgs; [ gcc cmake ninja pkg-config sdl3 libpng freetype openssl python3 fontconfig glib libdbusmenu ])
    ++ [ (pkgs.callPackage ../packaging/ayatana-glib.nix {}) ];
  BB_LAUNCHER_BUILD = launcherBuild;
}
