# GTK-free StatusNotifier/GMenu backend; nixpkgs still packages the deprecated GTK variant.
{ stdenv, fetchurl, cmake, pkg-config, glib }:
stdenv.mkDerivation {
  pname = "libayatana-appindicator-glib";
  version = "2.0.3";
  src = fetchurl {
    url = "https://codeload.github.com/AyatanaIndicators/libayatana-appindicator-glib/tar.gz/4fa65d8ce1a7f1a1f22cefe3d5bdd47667f188d0";
    hash = "sha256-jn52VW/ZXN2p0RQHppJwDsT3QXn62ArsLGVcXHz+aMw=";
  };
  nativeBuildInputs = [ cmake pkg-config glib ];
  buildInputs = [ glib ];
  # The launcher uses the C ABI; language bindings and documentation are not shipped.
  postPatch = ''
    sed -i '/add_subdirectory (bindings)/d; /add_subdirectory (doc)/d' CMakeLists.txt
    sed -i '/# AyatanaAppIndicatorGlib-2.0.gir/,$d' src/CMakeLists.txt
  '';
}
