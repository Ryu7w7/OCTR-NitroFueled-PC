# Packaged port: the prebuilt game binaries (build.sh first), the start-up scripts and the GTK4
# launcher, with their whole Nix closure and Mesa's Vulkan drivers. `bash packaging/appimage.sh`
# turns it into an AppImage (Steam Deck); `nix-build packaging` alone gives result/bin/bbport.
{ pkgs ? import <nixpkgs> { }
  # Store paths the prebuilt binaries load libraries from (their RUNPATHs), written by
  # appimage.sh. Nix only finds references to its inputs, and the binaries were built outside.
, runtimePaths ? (if builtins.pathExists ./runtime-paths.nix then import ./runtime-paths.nix else [ ])
}:
let
  lib = pkgs.lib;
  root = ./..;
  assetDirs = [ "scripts" "patches" "launcher" "shaders" "third_party/gcn2hlsl" ];
  # Only what the package needs (the tree also holds builds, profiles and captures).
  wanted = [
    "run.sh" "run.py" "out" "out/sacredfire" "out/bb-gpu-capabilities" "out/gpu" "out/gpu/libbbgpu.so"
    "out/VERSION" "tools" "tools/gen_import_names.py"
    # the native Vulkan engine's shaders are built on the player's PC (scripts/native_shaders.py)
    "tools/shaders" "tools/shaders/build_spirv.py" "third_party"
  ] ++ assetDirs;
  src = builtins.path {
    name = "bbport-src";
    path = root;
    filter = path: type:
      let rel = lib.removePrefix (toString root + "/") (toString path);
      in builtins.elem rel wanted
        || lib.any (dir: lib.hasPrefix (dir + "/") rel) assetDirs;
  };
  python = pkgs.python3.withPackages (ps: [ ps.pygobject3 ]);
  # Mesa's Vulkan drivers come with the package (RADV and ANV); bbport_vulkan.py adds the host
  # NVIDIA ICD and only its vendor libraries (matching the host's kernel module). Only the two
  # drivers, not the whole Mesa: its OpenGL drivers and LLVM were ~800 MB of the package. RADV
  # without LLVM (it compiles with ACO; LLVM was only an optional backend); the drivers' one
  # reference to Mesa's store path (share/drirc.d, per-application tweaks: none for this game)
  # is removed.
  mesaVulkan = (pkgs.mesa.override {
    galliumDrivers = [ "radeonsi" "iris" "llvmpipe" "zink" ];
    # Dozen (microsoft-experimental) only because the recipe expects its spirv2dxil output.
    vulkanDrivers = [ "amd" "intel" "microsoft-experimental" ];
  }).overrideAttrs (old: {
    mesonFlags = old.mesonFlags ++ [ "-Damd-use-llvm=false" ];
  });
  vulkanDrivers = pkgs.runCommand "bbport-vulkan-drivers" {
    nativeBuildInputs = [ pkgs.removeReferencesTo pkgs.jq ];
  } ''
    mkdir -p $out/lib $out/share/vulkan/icd.d
    for name in radeon intel; do
      cp ${mesaVulkan}/lib/libvulkan_$name.so $out/lib/
      chmod u+w $out/lib/libvulkan_$name.so
      remove-references-to -t ${mesaVulkan} $out/lib/libvulkan_$name.so
      jq ".ICD.library_path = \"$out/lib/libvulkan_$name.so\"" \
        ${mesaVulkan}/share/vulkan/icd.d/''${name}_icd.x86_64.json > $out/share/vulkan/icd.d/''${name}_icd.x86_64.json
    done
  '';
  icds = lib.concatMapStringsSep ":" (name: "${vulkanDrivers}/share/vulkan/icd.d/${name}")
    [ "radeon_icd.x86_64.json" "intel_icd.x86_64.json" ];
  # MangoHud (its Vulkan layer, XDG_DATA_DIRS) without its 32-bit half: the game is 64-bit.
  # xdg-utils (Perl, ~55 MB) only opens links: the host's xdg-open instead.
  # libdecor without its GTK 3 plugin (as in shell.nix) for MangoHud's glfw: GTK 3, tinysparql, ICU.
  libdecor = pkgs.libdecor.overrideAttrs (old: {
    buildInputs = lib.remove pkgs.gtk3 old.buildInputs;
    mesonFlags = old.mesonFlags ++ [ "-Dgtk=disabled" ];
  });
  mangohud = pkgs.mangohud.override {
    lowerBitnessSupport = false;
    glfw = pkgs.glfw.override { inherit libdecor; };
    xdg-utils = pkgs.writeShellScriptBin "xdg-open" ''
      host=$(command -v xdg-open) && exec "$host" "$@"
      exit 1
    '';
  };
  # The launcher's GTK without GStreamer (video playback: flite, x265, ... ~200 MB), printing and
  # the search indexer, which it never uses; libadwaita on top of it.
  gtk4 = (pkgs.gtk4.override { cupsSupport = false; trackerSupport = false; }).overrideAttrs (old: {
    buildInputs = lib.filter (p: !(lib.hasPrefix "gst-" (p.pname or ""))) old.buildInputs;
    mesonFlags = old.mesonFlags ++ [ "-Dmedia-gstreamer=disabled" ];
  });
  libadwaita = pkgs.libadwaita.override { inherit gtk4; };
  # Fonts: bundled DejaVu and Adwaita plus the host's usual font directories, but not the host's
  # /etc/fonts: on NixOS it names fonts in the host's /nix/store, which the AppImage hides behind
  # its own store in some environments (Steam's FHS sandbox), and the launcher showed boxes.
  fontsConf = pkgs.writeText "bbport-fonts.conf" ''
    <?xml version="1.0"?>
    <!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
    <fontconfig>
      <dir>${pkgs.dejavu_fonts}/share/fonts</dir>
      <dir>${pkgs.adwaita-fonts}/share/fonts</dir>
      <dir>/usr/share/fonts</dir>
      <dir>/usr/local/share/fonts</dir>
      <dir prefix="xdg">fonts</dir>
      <cachedir prefix="xdg">bbport/fontconfig</cachedir>
      <include ignore_missing="yes">${pkgs.fontconfig.out}/etc/fonts/conf.d</include>
    </fontconfig>
  '';
  # Environment the closure needs on any host: icon themes, SVG icon loader, fonts (above) and
  # a UTF-8 locale built into glibc.
  common = ''
      --prefix XDG_DATA_DIRS : ${mangohud}/share:${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share:${gtk4}/share/gsettings-schemas/${gtk4.name} \
      --set-default GDK_PIXBUF_MODULE_FILE ${pkgs.librsvg}/${pkgs.gdk-pixbuf.moduleDir}.cache \
      --set-default FONTCONFIG_FILE ${fontsConf} \
      --set-default LC_ALL C.UTF-8 \
      --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.libglvnd pkgs.libx11 pkgs.libxext ]} \
      --set BB_VULKANINFO ${pkgs.vulkan-tools}/bin/vulkaninfo \
  '';
in
pkgs.stdenv.mkDerivation {
  pname = "sacredfire";
  version = "0.1";
  inherit src;
  # The hook adds its GTK to the wrapper (typelibs, schemas): ours, not nixpkgs' with GStreamer.
  nativeBuildInputs = [ pkgs.makeShellWrapper (pkgs.wrapGAppsHook3.override { gtk3 = gtk4; }) pkgs.gobject-introspection ];
  buildInputs = [ gtk4 libadwaita pkgs.adwaita-icon-theme pkgs.librsvg ]
    ++ map builtins.storePath runtimePaths;
  dontBuild = true;
  dontConfigure = true;
  # The binaries live under share/ (next to the scripts run.sh expects): strip them too, which
  # also drops the compiler and header paths their debug info would keep in the closure.
  stripDebugList = [ "share/sacredfire/bin/gpu" ];
  # patchelf (RPATH shrinking) corrupts the non-PIE game binary's symbol versions; it finds
  # its library through $ORIGIN/gpu and its other libraries through the build's RUNPATH.
  dontPatchELF = true;
  dontWrapGApps = true; # wrapped once below, together with the launcher's own variables
  installPhase = ''
    runHook preInstall
    d=$out/share/sacredfire
    mkdir -p $d/bin $out/bin
    cp run.sh run.py $d/   # run.py: scripts/online_config.py reuses its online setup
    cp out/VERSION $d/VERSION   # the build's source revision (build.sh); run.sh and the launcher show it
    cp -r scripts patches launcher $d/
    # The shader recipe (shaders/README.md): prepared for the player's GPU at the first start.
    if [ -d shaders ]; then cp -r shaders $d/; fi
    # run.sh generates the eboot's import names with it (shadPS4's aerolib list, fetched once).
    install -Dm755 tools/gen_import_names.py $d/tools/gen_import_names.py
    # The native Vulkan engine's shader build: its translator and driver script (DXC: BB_DXC below).
    install -Dm644 tools/shaders/build_spirv.py $d/tools/shaders/build_spirv.py
    mkdir -p $d/third_party && cp -r third_party/gcn2hlsl $d/third_party/
    install -m755 out/sacredfire $d/bin/sacredfire
    install -m755 out/bb-gpu-capabilities $d/bin/bb-gpu-capabilities
    install -Dm755 out/gpu/libbbgpu.so $d/bin/gpu/libbbgpu.so
    # Their debug info names the build environment's headers (the full GCC, Vulkan headers): Nix
    # took those for dependencies, ~300 MB. Symbols stay (crash reports).
    ${pkgs.binutils-unwrapped}/bin/strip --strip-debug $d/bin/sacredfire $d/bin/bb-gpu-capabilities
    # Menu entry and icon: nix-appimage takes the .desktop whose Exec is the main program and its
    # 256x256 icon as the AppImage's own (.DirIcon), which AppImage managers (Gear Lever,
    # AppImageLauncher) need to integrate it.
    install -Dm644 launcher/io.github.sacredfire.Launcher.png \
      $out/share/icons/hicolor/256x256/apps/io.github.sacredfire.Launcher.png
    mkdir -p $out/share/applications
    sed -e 's|@LAUNCHER@|sacredfire|' -e 's|@ICON@|io.github.sacredfire.Launcher|' \
      launcher/io.github.sacredfire.Launcher.desktop.in > $out/share/applications/io.github.sacredfire.Launcher.desktop
    runHook postInstall
  '';
  postFixup = ''
    # bin/sacredfire is a static program (bbport-entry.c) that clears the host's LD_PRELOAD (Steam's
    # overlay) and similar before the wrapper's own dynamic programs start.
    mkdir -p $out/libexec
    gcc -O2 -Wall -Werror -static -L${pkgs.glibc.static}/lib -DTARGET="\"$out/libexec/sacredfire\"" \
      ${./bbport-entry.c} -o $out/bin/sacredfire
    makeShellWrapper ${python}/bin/python3 $out/libexec/sacredfire \
      "''${gappsWrapperArgs[@]}" \
      ${common}      --add-flags "$out/share/sacredfire/launcher/bbport_vulkan.py ${python}/bin/python3 $out/share/sacredfire/launcher/sacredfire_launcher.py" \
      --set BB_PREBUILT 1 \
      --set PYTHON ${pkgs.python3}/bin/python3 \
      --set BB_DXC ${pkgs.directx-shader-compiler}/bin/dxc \
      --set BB_BUNDLED_VK_DRIVER_FILES ${icds} \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils pkgs.util-linux pkgs.procps
                                          pkgs.gawk pkgs.gnused pkgs.gnugrep ]} \
      --run 'export BB_DATA_DIR=''${BB_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/sacredfire}; mkdir -p "$BB_DATA_DIR"'
    # The game alone, without the launcher (settings from the data directory's bbport.ini).
    makeShellWrapper ${pkgs.python3}/bin/python3 $out/bin/sacredfire-game \
      ${common}      --add-flags "$out/share/sacredfire/launcher/bbport_vulkan.py ${pkgs.bash}/bin/bash $out/share/sacredfire/run.sh" \
      --set BB_PREBUILT 1 \
      --set PYTHON ${pkgs.python3}/bin/python3 \
      --set BB_DXC ${pkgs.directx-shader-compiler}/bin/dxc \
      --set BB_BUNDLED_VK_DRIVER_FILES ${icds} \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils pkgs.procps ]} \
      --run 'export BB_DATA_DIR=''${BB_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/sacredfire}; mkdir -p "$BB_DATA_DIR"'
  '';
  meta.mainProgram = "sacredfire";
}
