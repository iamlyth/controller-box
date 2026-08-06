# Development/build shell providing the Controller-Box native dependencies.
# Usage:
#   nix-shell --run "cmake -B build && cmake --build build"
#
# Provides: SDL2, SDL2_ttf, SDL2_image, libsystemd (sd-bus), libyaml, cmocka,
# plus the build toolchain (cmake, pkg-config, gcc).
{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [ cmake pkg-config gcc python3 python3Packages.pyyaml ];
  buildInputs = with pkgs; [
    SDL2
    SDL2_ttf
    SDL2_image
    harfbuzz          # transitive dep of SDL2_ttf (silences pkg-config)
    libtiff           # transitive dep of SDL2_image
    freetype          # transitive dep of harfbuzz
    systemd      # provides sd-bus (libsystemd)
    libyaml
    cmocka
  ];
}