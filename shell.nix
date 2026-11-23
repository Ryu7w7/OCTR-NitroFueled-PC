# Build environment: `nix-shell native_probe/shell.nix --run 'bash native_probe/run.sh'`
{ pkgs ? import <nixpkgs> {} }:
let
  # SDL3 with libdecor's cairo plugin only (as packaging/default.nix does for MangoHud): its GTK 3
  # plugin brought GTK 3, tinysparql and ICU into the AppImage (~45 MB) for the same window
  # decorations on Wayland.
  libdecor = pkgs.libdecor.overrideAttrs (old: {
    buildInputs = pkgs.lib.remove pkgs.gtk3 old.buildInputs;
    mesonFlags = old.mesonFlags ++ [ "-Dgtk=disabled" ];
  });
  sdl3 = pkgs.sdl3.override { inherit libdecor; };
in
pkgs.mkShell {
  FONTCONFIG_FILE = pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts pkgs.noto-fonts pkgs.noto-fonts-cjk-sans ]; };
  packages = (with pkgs; [
    gcc gnumake cmake ninja pkg-config python3 binutils git
    vulkan-headers vulkan-loader xz libpng freetype pango openssl fontconfig glib libdbusmenu
    directx-shader-compiler  # the native Vulkan engine compiles its shaders with it (scripts/native_shaders.py)
    # GPU library (gpu/): shadPS4 video core dependencies
    ffmpeg-headless libopus boost fmt magic-enum robin-map xxhash vulkan-memory-allocator glslang spirv-cross xbyak zydis spirv-headers miniz libx11 libxcb xorgproto wayland
  ]) ++ [ sdl3 (pkgs.callPackage ./packaging/ayatana-glib.nix {}) ];
}
