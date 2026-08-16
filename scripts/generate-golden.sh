#!/usr/bin/env bash
# generate-golden.sh — Regenerate golden image baselines for visual tests.
#
# This is an explicit, reviewed action — never automatic.  After running,
# review the generated PNGs in tests/golden/ and commit them if correct.
#
# Usage: scripts/generate-golden.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(dirname "$SCRIPT_DIR")"

cd "$SOURCE_DIR"

echo "Building test_golden..."
nix-shell --run "cmake --build build-maintenance-verify --target test_golden"

echo ""
echo "Generating golden baselines..."
CBX_GENERATE_GOLDEN=1 SDL_VIDEODRIVER=dummy \
  ctest --test-dir build-maintenance-verify -R test_golden --output-on-failure

echo ""
echo "Golden baselines generated in tests/golden/"
echo "Review the images and commit them if they look correct."
echo "Do NOT commit tests/golden-fail/ — it is for temporary failure artifacts only."