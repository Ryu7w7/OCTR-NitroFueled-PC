{
  description = "SacredFire AppImage bundler with host NVIDIA initialization";
  inputs.nix-appimage.url = "github:ralismark/nix-appimage";
  inputs.nixpkgs.follows = "nix-appimage/nixpkgs";

  outputs = { self, nix-appimage, nixpkgs }: {
    bundlers.x86_64-linux.default = drv:
      let
        system = "x86_64-linux";
        pkgs = import nixpkgs { inherit system; };
        # nix-appimage's own AppRun (userns-chroot), built as it builds it, plus a relative
        # entrypoint link: appimage.sh makes every link relative, since RecompLauncher unpacks
        # the AppImage into a plain folder and leaves absolute links out.
        apprun = pkgs.pkgsStatic.runCommandCC "sacredfire-AppRun" {} ''
          mkdir -p $out/mountroot
          patch -o main.c ${nix-appimage}/appruns/userns-chroot/main.c ${./apprun-relative-entrypoint.patch}
          $CC main.c -o $out/AppRun.nix
          $CC -O2 -Wall -Wextra -Werror ${./appimage-bootstrap.c} -o $out/AppRun
        '';
        bundle = nix-appimage.lib.${system}.mkAppImage.override {
          mkappimage-apprun = apprun;
        };
      in bundle { program = if drv.type == "app" then drv.program else pkgs.lib.getExe drv; };
  };
}
