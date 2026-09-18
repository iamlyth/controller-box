# CMake toolchain file for cross-compiling Controller-Box to aarch64-linux.
#
# Usage:
#   nix-shell cross-shell.nix --run \
#     "cmake -S . -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Debug && cmake --build build-aarch64 --parallel"
#
# The cross-shell.nix provides the aarch64-unknown-linux-gnu GCC toolchain and
# cross-compiled SDL2, SDL2_ttf, SDL2_image, systemd (sd-bus), and libyaml
# from nixpkgs pkgsCross.aarch64-multiplatform.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Compiler names — nixpkgs aarch64-multiplatform uses the GNU triplet
set(CMAKE_C_COMPILER aarch64-unknown-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-unknown-linux-gnu-g++)

# pkg-config for the target architecture
set(ENV{PKG_CONFIG} aarch64-unknown-linux-gnu-pkg-config)

# Search paths: find libraries and headers in the cross sysroot only
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)