# Development/build shell providing the Controller-Box native dependencies.
# Usage:
#   nix-shell --run "cmake -B build && cmake --build build"
#
# Provides: SDL2, SDL2_ttf, SDL2_image, libsystemd (sd-bus), libyaml, cmocka,
# plus the build toolchain (cmake, pkg-config, gcc).
# Also provides Xvfb, xdotool, and ImageMagick for the installed smoke test.
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