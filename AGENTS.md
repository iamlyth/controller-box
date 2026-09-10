# Controller-Box Operational Guide

Keep this file brief and operational. Progress, task status, and verification
evidence live in `.factory/artifacts/implementation-plan.md`; the factory
keeps canonical mutable loop state in `.factory-state/factory-loop.json` and
campaign-scoped runtime state below `.factory-state/campaigns/`
(methodology: `docs/FACTORY-LOOP-SPEC.md`).

## Sources of truth

- Product contract: `docs/SPEC.md`
- Factory loop spec: `docs/FACTORY-LOOP-SPEC.md`
- Declared factory capabilities: `.factory/environment.toml` (never invent
  undeclared runners)
- Active work and evidence: `.factory/artifacts/implementation-plan.md`
- Ordinary defects: `.factory/bugs/open.md` and `.factory/bugs/closed.md`
- Work only on `develop`; the human promotes to `main`.
- Do not use Git worktrees or change the committed specification during
  implementation.

## Build

Use Nix so SDL2, systemd, YAML, cmocka, Xvfb, xdotool, and image tools are
consistent:

```bash
nix-shell --run 'cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel'
```

For a targeted rebuild, use `cmake --build build --target <target>`.

## Immediate validation

```bash
# Full project gate: build, CTest, packaging, installed smoke
./scripts/verify.sh

# Targeted CTest by exact name or regex
ctest --test-dir build -R '<regex>' --output-on-failure

# Sanitizer gate (ASan + UBSan)
./scripts/verify-sanitizers.sh

# Factory campaign
python3 .factory/bin/factory-campaign run \
  --campaign-id controller-box-v1 --rounds 20 --branch develop \
  --provider ollama --model deepseek-v4-flash
```

Run build/test commands serially. Do not dismiss an unrelated failure as
pre-existing: determine its cause, fix it when safe, or append a remediation
task with evidence.

## Run and inspect

```bash
./build/controller-box --manager --dry-run
./build/controller-box --overlay-service --dry-run
```

Use `test_manager_visual`, `test_overlay_visual`, and `test_golden` for
framebuffer behavior. Use `test_installed_smoke` for the installed X11
production path. Golden updates must be explicit and reviewed; never
regenerate them merely to make a test pass.

A custom `CMAKE_INSTALL_PREFIX` build is for isolated install/UI testing and
should exclude the default-prefix packaging assertion. Use a separate
default-prefix build for full packaging verification.

## Runners

Three runners are declared in `.factory/environment.toml`:

- **dev-runner-vm** — general-purpose dev VM (systemd, /dev/uinput, packages).
  Capabilities: `remote-project-gate`, `systemd-user`, `kernel-uinput`,
  `installed-package`.
- **iprunner** — physical InputPlumber system bus, real controllers.
  Capabilities: `inputplumber-system-dbus`, `physical-controller`,
  `target-consumer`, `controller-production-routing`.
- **gpurunner** — GPU compositor for accelerated rendering tests.
  Capabilities: `gpu-compositor`, `installed-licensed-diagram`.

All runners execute `./scripts/verify.sh` (build + ctest). Tests that need
hardware not present on a runner skip with exit 77. The planner routes
tasks to the appropriate runner based on declared capabilities. An
unreachable runner marks the task `blocked`, never a silent skip or fake
pass.

## Code and test patterns

- Search before assuming behavior is missing; reuse established `src/`
  utilities instead of ad-hoc copies.
- Production acceptance must use real initialization, SDL event dispatch,
  rendering, backend, persistence, and shutdown paths.
- UI tests must assert semantic outcomes as well as pixels; direct callback
  tests are supplemental.
- Route InputPlumber operations through the DBus backend abstraction so
  tests can observe exact requests.
- Scope temporary-file assertions to files owned by the test; never assert
  global `/tmp/controller-box-*` emptiness.
- Do not leave placeholders, stubs, weakened assertions, unexplained skips,
  or test-only production bypasses.
- Documentation records why a constraint or test matters, not iteration
  history.

## Git commit boundary

- The orchestrator is the sole Git writer. Roles never run `git commit`.
- After each implementation+verification cycle the orchestrator commits
  with `git add -A && git commit -m "factory: task {N} round {R}"`.
- Do not bypass hooks (`--no-verify`, `core.hooksPath`, `GIT_CONFIG_*`).

## Acceptance evidence

- Tests must actually pass: the tester runs the real verification command
  and records the real exit code. Declaring evidence is not evidence.
- The auditor checks for weakened assertions, removed or skipped tests,
  tautological passes, and verification commands that do not test the
  implementation. Findings become plan tasks in the next planning round.
- Pixel/offscreen checks are not real visual acceptance, private/session
  DBus is not the real system service, a uinput producer is not the target
  consumer, and declaring evidence is not evidence.