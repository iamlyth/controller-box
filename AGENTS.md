# Controller-Box Operational Guide

Keep this file brief and operational. Progress, task status, and verification evidence belong in `IMPLEMENTATION_PLAN.md`; the latest recovery handoff belongs in `.ralph/agent/scratchpad.md`.

## Sources of truth

- Product contract: `docs/SPEC.md`
- Declared factory capabilities: `factory-environment.toml` (never invent undeclared runners)
- Active work and evidence: `IMPLEMENTATION_PLAN.md`
- Ordinary defects: `open-bugs.md` and `closed-bugs.md`
- Work only on `develop`; the human promotes to `main`.
- Do not use Git worktrees or change the committed specification during implementation.

## Build

Use Nix so SDL2, systemd, YAML, cmocka, Xvfb, xdotool, and image tools are consistent:

```bash
nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel'
```

For a targeted rebuild, use `cmake --build build-check --target <target>`.

## Immediate validation

```bash
# Targeted CTest by exact name or regex
nix-shell --run "ctest --test-dir build-check -R '<regex>' --output-on-failure"

# Complete clean project gate: build, CTest, packaging, installed smoke
nix-shell --run './scripts/verify-project.sh'

# Ralph/factory policy and orchestration
./scripts/verify-boilerplate.sh

# Finite fresh-plan/implementation/audit campaign
./scripts/ralph-campaign.sh --rounds 3
```

Run build/test commands serially. Do not dismiss an unrelated failure as pre-existing: determine its cause, fix it when safe, or append a remediation task with evidence.

## Run and inspect

```bash
./build-check/controller-box --manager --dry-run
./build-check/controller-box --overlay-service --dry-run
```

Use `test_manager_visual`, `test_overlay_visual`, and `test_golden` for framebuffer behavior. Use `test_installed_smoke` for the installed X11 production path. Golden updates must be explicit and reviewed; never regenerate them merely to make a test pass.

A custom `CMAKE_INSTALL_PREFIX` build is for isolated install/UI testing and should exclude the default-prefix packaging assertion. Use a separate default-prefix build for full packaging verification.

## Code and test patterns

- Search before assuming behavior is missing; reuse established `src/` utilities instead of ad-hoc copies.
- Production acceptance must use real initialization, SDL event dispatch, rendering, backend, persistence, and shutdown paths.
- UI tests must assert semantic outcomes as well as pixels; direct callback tests are supplemental.
- Route InputPlumber operations through the DBus backend abstraction so tests can observe exact requests.
- Scope temporary-file assertions to files owned by the test; never assert global `/tmp/controller-box-*` emptiness.
- Do not leave placeholders, stubs, weakened assertions, unexplained skips, or test-only production bypasses.
- Documentation records why a constraint or test matters, not iteration history.
