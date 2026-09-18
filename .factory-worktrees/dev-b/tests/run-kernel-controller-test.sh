#!/usr/bin/env bash
# Stage an isolated install before exercising the kernel-backed controller path.
set -euo pipefail

[[ $# -eq 2 ]] || {
    echo "usage: $0 TEST_EXECUTABLE BUILD_DIRECTORY" >&2
    exit 64
}
test_executable=$1
build_directory=$2
[[ -x "$test_executable" && -d "$build_directory" ]] || {
    echo "kernel-controller wrapper: invalid test executable or build directory" >&2
    exit 1
}

# Preserve the test's intentional capability skip without paying for an install
# on hosts where the kernel device is absent or inaccessible.
if [[ ! -c /dev/uinput || ! -w /dev/uinput ]]; then
    exec "$test_executable" "$build_directory"
fi

stage=$(mktemp -d -t cbx-kernel-install-XXXXXX)
# Invoked by trap.
# shellcheck disable=SC2329
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT INT TERM HUP
cmake --install "$build_directory" --prefix "$stage" >/dev/null
installed_binary="$stage/bin/controller-box"
[[ -x "$installed_binary" ]] || {
    echo "kernel-controller wrapper: staged installed binary is missing" >&2
    exit 1
}

set +e
CBX_TEST_INSTALLED_BINARY="$installed_binary" \
    "$test_executable" "$build_directory"
rc=$?
set -e
exit "$rc"
