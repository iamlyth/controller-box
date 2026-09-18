# Cross-compilation shell for building Controller-Box to aarch64-linux.
#
# Usage:
#   nix-shell cross-shell.nix --run \
#     "cmake -S . -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Debug && cmake --build build-aarch64 --parallel"
#
# Provides: aarch64-unknown-linux-gnu GCC toolchain and cross-compiled
# SDL2, SDL2_ttf, SDL2_image, systemd (sd-bus), libyaml.
{ pkgs ? import <nixpkgs> {} }:

let
  crossPkgs = pkgs.pkgsCross.aarch64-multiplatform;
in
crossPkgs.mkShell {
  # Native build tools (cmake, pkg-config run on the host)
  nativeBuildInputs = with pkgs; [ cmake pkg-config python3 ];

  # Cross-compiled libraries and compiler
  buildInputs = with crossPkgs; [
    SDL2
    SDL2_ttf
    SDL2_image
    harfbuzz
    glib
    libtiff
    freetype
    systemd
    libyaml
  ];
}