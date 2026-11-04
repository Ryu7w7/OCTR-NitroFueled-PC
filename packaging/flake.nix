{
  description = "SacredFire AppImage bundler with host NVIDIA initialization";
  inputs.nix-appimage.url = "github:ralismark/nix-appimage";
  inputs.nixpkgs.follows = "nix-appimage/nixpkgs";

  outputs = { self, nix-appimage, nixpkgs }: {
    bundlers.x86_64-linux.default = drv:
      let
        system = "x86_64-linux";
        pkgs = import nixpkgs { inherit system; };
        upstream = nix-appimage.packages.${system}.appimage-appruns.userns-chroot;
        apprun = pkgs.pkgsStatic.runCommandCC "sacredfire-AppRun" {} ''
          mkdir -p $out/mountroot
          cp ${upstream}/AppRun $out/AppRun.nix
          $CC -O2 -Wall -Wextra -Werror ${./appimage-bootstrap.c} -o $out/AppRun
        '';
        bundle = nix-appimage.lib.${system}.mkAppImage.override {
          mkappimage-apprun = apprun;
        };
      in bundle { program = if drv.type == "app" then drv.program else pkgs.lib.getExe drv; };
  };
}
