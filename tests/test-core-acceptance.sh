#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

setup_fixture() {
    local root="$1" checker_exit="$2"
    mkdir -p "$root/scripts" "$root/src/manager" "$root/.factory/artifacts"
    cp "$PROJECT_ROOT/scripts/check-core-acceptance.sh" "$root/scripts/"
    cat > "$root/scripts/check-capability-evidence.py" <<PY
#!/usr/bin/env python3
import sys
expected = ["--root", "$root", "--capabilities", "controller-production-routing"]
if sys.argv[1:] != expected:
    raise SystemExit(127)
raise SystemExit($checker_exit)
PY
    chmod +x "$root/scripts/check-core-acceptance.sh" "$root/scripts/check-capability-evidence.py"
    cat > "$root/src/manager/profile_diagram.c" <<'EOF'
static const int s_device_layouts[] = {
    { "cc-xbox-360", 0 },
};
EOF
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
(cd "$strong" && ./scripts/check-core-acceptance.sh >/dev/null)

# Routing evidence cannot mask a missing licensed production diagram.
rm "$strong/src/manager/profile_diagram.c"
if (cd "$strong" && ./scripts/check-core-acceptance.sh >/dev/null 2>&1); then
    echo "missing licensed diagram passed core acceptance" >&2
    exit 1
fi

echo "test: core acceptance requires canonical production-routing evidence"
