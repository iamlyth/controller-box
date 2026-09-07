#!/usr/bin/env bash
# BUG-0020 regression: the retained build tree must never contain a nested
# .factory/... object path under CMakeFiles/inputplumber-mediator.dir/.
# A pre-fix build may have left a stale nested .factory object dir inside an
# existing retained build tree; rerunning configure must remove it before
# target generation, and a legacy .factory symlink must be removed as a link
# only (never followed) so it cannot escape build containment.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cbx-mediator-ns.XXXXXX")
EXT=$(mktemp -d "${TMPDIR:-/tmp}/cbx-mediator-ext.XXXXXX")
trap 'rm -rf -- "$TMP" "$EXT"' EXIT HUP INT TERM

# Configure once so the build tree exists, then seed a legacy nested .factory
# object dir exactly as a pre-fix build would have left it inside the retained
# build tree. Rerunning configure must remove it before target generation.
cmake -S "$ROOT" -B "$TMP" -DCMAKE_BUILD_TYPE=Debug >/dev/null
LEGACY="$TMP/CMakeFiles/inputplumber-mediator.dir/.factory/runner"
mkdir -p "$LEGACY"
printf 'stale legacy object\n' > "$LEGACY/inputplumber-mediator.c.o"
cmake -S "$ROOT" -B "$TMP" -DCMAKE_BUILD_TYPE=Debug >/dev/null
if [ -e "$TMP/CMakeFiles/inputplumber-mediator.dir/.factory" ]; then
    echo "test-mediator-build-namespace: legacy .factory object dir not removed on reconfigure" >&2
    exit 1
fi

# Symlink-escape safety: a legacy .factory that is a symlink to an external
# directory must be removed as a link only, never followed, so the external
# target is untouched.
ln -s "$EXT" "$TMP/CMakeFiles/inputplumber-mediator.dir/.factory"
cmake -S "$ROOT" -B "$TMP" -DCMAKE_BUILD_TYPE=Debug >/dev/null
if [ -L "$TMP/CMakeFiles/inputplumber-mediator.dir/.factory" ]; then
    echo "test-mediator-build-namespace: legacy .factory symlink not removed" >&2
    exit 1
fi
if [ ! -d "$EXT" ]; then
    echo "test-mediator-build-namespace: symlink escape deleted external target" >&2
    exit 1
fi

cmake --build "$TMP" --target inputplumber-mediator --parallel 2 >/dev/null
# The generated copy must be byte-identical to the tracked source.
cmp -s "$ROOT/.factory/runner/inputplumber-mediator.c" "$TMP/generated/inputplumber-mediator.c"
# No hidden-namespace segment may appear under the target's object dir.
if find "$TMP/CMakeFiles/inputplumber-mediator.dir" -name '.factory' -print -quit | grep -q .; then
    echo "test-mediator-build-namespace: nested .factory object path emitted" >&2
    exit 1
fi
echo "test-mediator-build-namespace: PASS"
