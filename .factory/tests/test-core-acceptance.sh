#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

setup_fixture() {
    local root="$1" checker_exit="$2"
    mkdir -p "$root/scripts" "$root/bin" "$root/.factory/artifacts" "$root/.factory/tools"
    cp "$PROJECT_ROOT/scripts/check-core-acceptance.sh" "$root/scripts/"
    cp "$PROJECT_ROOT/.factory/tools/check-core-acceptance.sh" "$root/.factory/tools/"
    cat > "$root/.factory/tools/check-capability-evidence.py" <<PY
#!/usr/bin/env python3
import sys
expected = ["--root", "$root", "--capabilities", "controller-production-routing,gpu-compositor,installed-licensed-diagram"]
if sys.argv[1:] != expected:
    raise SystemExit(127)
raise SystemExit($checker_exit)
PY
    chmod +x "$root/scripts/check-core-acceptance.sh" "$root/.factory/tools/check-core-acceptance.sh" "$root/.factory/tools/check-capability-evidence.py"
    cat > "$root/bin/ctest" <<'EOF'
#!/usr/bin/env bash
exit "${FIXTURE_CTEST_EXIT:-0}"
EOF
    chmod +x "$root/bin/ctest"
    git -C "$root" init -q
    git -C "$root" config user.email fixture@test
    git -C "$root" config user.name fixture
    git -C "$root" add -A
    git -C "$root" commit -qm fixture
}

# Neither a successful weak system-bus probe nor free-form "4 of 4" prose may
# replace signed exact-HEAD controller-production-routing evidence.
weak="$tmp/weak"
mkdir -p "$weak"
setup_fixture "$weak" 1
cat > "$weak/scripts/probe-inputplumber-system-dbus.sh" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "$weak/scripts/probe-inputplumber-system-dbus.sh"
printf '%s\n' '4 of 4 virtual controllers active' > "$weak/.factory/artifacts/manager-report.txt"
if (cd "$weak" && FACTORY_MANAGER_REPORT=.factory/artifacts/manager-report.txt ./scripts/check-core-acceptance.sh >/dev/null 2>&1); then
    echo "weak system-bus/report evidence passed core acceptance" >&2
    exit 1
fi

# The core detector accepts routing only through the canonical capability
# checker invocation; diagram acceptance remains a separate production check.
strong="$tmp/strong"
mkdir -p "$strong"
setup_fixture "$strong" 0
(cd "$strong" && PATH="$strong/bin:$PATH" ./scripts/check-core-acceptance.sh >/dev/null)

# Local deterministic tests are supplemental and cannot replace either signed
# installed/GPU capability (the checker stub is the sole trust decision).
echo "test: core acceptance requires signed all-four routing and installed licensed GPU evidence"
