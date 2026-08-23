# Development/build shell providing the Controller-Box native dependencies.
# Usage:
#   nix-shell --run "cmake -B build && cmake --build build"
#
# Provides: SDL2, SDL2_ttf, SDL2_image, libsystemd (sd-bus), libyaml, cmocka,
# plus the build toolchain (cmake, pkg-config, gcc).
# Also provides Xvfb, xdotool, and ImageMagick for the installed smoke test.
#
# UNPINNED nixpkgs ceiling (documented): `import <nixpkgs>` resolves the nixpkgs
# channel/flake registry at evaluation time, so the exact toolchain/dependency
# versions are NOT reproducible — there is no flake.lock or lockfile pinning the
# nixpkgs revision. The authenticated Nix gate (scripts/nix-gate.sh) proves the
# declared tools resolve under the immutable, content-addressed /nix/store (so a
# run is genuinely Nix-built, never silently verified against undeclared host
# packages), but it does NOT pin exact versions. Pin nixpkgs (a flake.lock, a
# locked overlay, or a channel revision) before any release that requires
# byte-exact toolchain reproducibility.
{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [ cmake pkg-config gcc python3 python3Packages.pyyaml ];
  buildInputs = with pkgs; [
    SDL2
    SDL2_ttf
    SDL2_image
    harfbuzz          # transitive dep of SDL2_ttf (silences pkg-config)
    glib              # transitive dep of harfbuzz (pkg-config propagation)
    libtiff           # transitive dep of SDL2_image
    freetype          # transitive dep of harfbuzz
    systemd      # provides sd-bus (libsystemd)
    libyaml
    cmocka
    # gpu-compositor probe helper (egl_renderer_probe.c) compile-time deps:
    wayland          # wayland-client.h + wayland-client.pc (EGL/GLES2 come from libglvnd/mesa)
    # Installed smoke test dependencies (Task 9, §11.1.5):
    xorg-server     # Xvfb — headless X11 server
    xauth           # xauth — private MIT-MAGIC-COOKIE-1 for the isolated Xvfb display
    xdotool          # keyboard/mouse input injection
    imagemagick       # import (screenshot), convert/identify (pixel variance)
    bc               # floating-point arithmetic for mean threshold checks
  ];
}