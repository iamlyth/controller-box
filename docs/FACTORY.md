# Factory Boilerplate

This document covers the Ralph Software Factory development infrastructure
used to implement Controller-Box. It is not relevant to end users — it
documents the autonomous development loop, branch policy, quota management,
and recovery procedures.

The factory boilerplate is retained on the `develop` branch only. End users
should refer to [README.md](../README.md) for product documentation.

## What this is

A reusable, single-writer implementation of Geoffrey Huntley's Ralph Wiggum
development technique using Ralph Orchestrator, jailed Pi, Ollama, adaptive
read-only subagents, Git checkpoints, quota waiting, and crash recovery.

`docs/SPEC.md` is the source of truth for the product. `IMPLEMENTATION_PLAN.md`
tracks task status and verification evidence.

## Operating model

- `main` is the human-controlled release branch.
- `develop` is the autonomous implementation branch.
- One committed `docs/SPEC.md` is the source of truth; Git versions it.
- A planning-only Ralph loop creates `IMPLEMENTATION_PLAN.md` for the exact spec commit.
- Each implementation iteration selects one bounded task and starts with fresh model context.
- Pi subagents perform parallel read-only planning, research, review, security, and documentation analysis.
- Exactly one primary worker may edit, stage, or commit repository files.
- Tests and documentation are completion gates.
- You review `develop` and manually promote it to `main`.

No Git worktrees are used. `features.parallel` is disabled in both Ralph configurations.

## Relationship to Huntley's playbook

The prompts are periodically compared against [`ghuntley/how-to-ralph-wiggum`](https://github.com/ghuntley/how-to-ralph-wiggum) (reviewed at commit `88d488a148af97e4a3f22b11b4c3598c79d6a577`). Controller-Box adopts its highest-value context and backpressure patterns:

- deterministic orientation: study the specification, plan, concise `AGENTS.md`, source, tests, and shared patterns every iteration;
- **do not assume functionality is missing**—search and trace production behavior first;
- keep the primary context as scheduler and use parallel subagents as disposable read-only memory;
- derive tests from behavioral acceptance criteria, including performance and edge cases, while leaving implementation choices to the worker;
- keep operational learning in brief `AGENTS.md`, progress/evidence in the plan, and only the current crash handoff in the scratchpad;
- update the plan immediately when discoveries create work, implement completely without placeholders, investigate unrelated failures, and use tests/build/lint/install checks as backpressure;
- capture why tests and documentation constraints matter.

Deliberate safety differences are retained: at most eight adaptive read-only subagents rather than hundreds of mutating agents; one repository writer and serialized builds; a jailed Pi backend rather than skipped permissions; no worktrees; no autonomous specification edits; no pruning of the active-cycle ledger; no automatic push, tag, or promotion to `main`. Fresh planning still discards the prior active plan from working context while Git preserves its history.

## Durable and volatile state

Durable, tracked state:

- `docs/SPEC.md`: approved requirements
- `AGENTS.md`: concise build/run/validation commands and durable operational patterns
- `IMPLEMENTATION_PLAN.md`: feature task status and verification evidence
- `open-bugs.md` / `closed-bugs.md`: portable canonical defect state
- `MAINTENANCE_PLAN.md`: one selected bug, fingerprint, tasks, and evidence
- `.ralph/agent/scratchpad.md`: concise crash handoff
- source, tests, README, and operational documentation
- `factory.toml`, Ralph configs, prompts, and project subagent definitions

Volatile, ignored state:

- event streams and pointer files under `.ralph/`
- loop locks, diagnostics, API state, task/memory stores, and TUI exports
- Pi transcripts and scheduled-agent state
- `.factory-lock`, `.bug-ledger.lock`, and `.factory-state/` lifecycle markers
- `.ollama-usage-env`

Git checkpoints make the plan, scratchpad, and implementation recoverable. Event/task files improve same-disk recovery but are not treated as portable project history.

## Branch policy

The autonomous lifecycle runs only on `develop`. `main` is protected by policy and never modified by the factory. `scripts/branch-guard.sh` also rejects multiple Git worktrees.

A boilerplate experiment on a `factory/*` branch requires the explicit temporary override:

```bash
FACTORY_ALLOW_TRIAL_BRANCH=1 ./scripts/ralph-plan.sh
```

Do not carry this override into normal development.

## Prerequisites

- Ralph Orchestrator with the native Pi backend
- `pi2` configured with the `@tintinweb/pi-subagents` extension
- Ollama provider/model access
- Bash, Git, Python 3.11+, curl, flock, and optionally ShellCheck
- A clean `develop` branch with at least one commit

The project tracks `.pi/subagents.json` with a maximum of eight simultaneous read-only subagents. Project agents in `.pi/agents/` intentionally expose no `bash`, `edit`, or `write` tools.

## Initial setup

1. Merge this boilerplate branch into `develop`.
2. Configure Ollama Cloud usage credentials:

   ```bash
   source scripts/update-ollama-cookies.sh
   ```

3. Confirm access and quota parsing:

   ```bash
   ./scripts/ollama-usage-guard.sh --check
   ```

4. Edit `docs/SPEC.md` and commit it separately:

   ```bash
   git add docs/SPEC.md
   git commit -m "spec: define the next release"
   ```

## Plan

Run the planning-only fresh-context loop:

```bash
./scripts/ralph-plan.sh
```

The planner may only modify `IMPLEMENTATION_PLAN.md` and the recovery scratchpad. A fresh invocation atomically replaces both with minimal cycle state before Ralph starts, so completed tasks are not carried into future prompts. Previous plans remain available through Git history; `--resume` preserves the active draft. The generated plan records:

- the spec path;
- the latest commit that changed the spec;
- the exact spec blob ID;
- the base commit;
- a requirement-by-requirement specification conformance matrix;
- an exhaustive manager-control and overlay-action interaction inventory;
- bounded tasks, dependencies, acceptance evidence, and documentation impact;
- a mandatory final documentation/specification audit that depends on every other task and executes the specification's definition of done.

Inspect the plan before implementation. Every newly accepted task must be `pending`; inherited completed, in-progress, or blocked tasks fail the planning gate. Every `partial`, `missing`, or `ambiguous` conformance row must map to a task. `scripts/check-plan-freshness.sh` prevents a stale plan or altered cycle base from running after the specification changes.

For a headless planning loop:

```bash
./scripts/ralph-plan.sh --no-tui
```

## Implement

Start the single-writer build loop:

```bash
./scripts/ralph-run.sh
```

Each iteration:

1. validates branch and plan freshness;
2. waits for Ollama quota when necessary;
3. selects one ready task;
4. fans out only read-only analysis;
5. implements and tests one task with one writer;
6. updates the plan and recovery scratchpad;
7. creates a Git checkpoint;
8. exits so the next task receives fresh context.

Only the final documentation and specification audit may produce `LOOP_COMPLETE`. It must satisfy `docs/SPEC.md` §11.2: all conformance rows verified, every control exercised through production event dispatch with semantic outcomes, full visual/degraded/installed verification, no contradictory open bugs, adversarial reviews, current documentation, and a clean tree.

There is no minimum iteration count: high quality is determined by evidence, not loop volume. Conversely, completing the originally planned tasks is not enough when acceptance discovers another gap. The worker preserves the ledger, appends a new uniquely numbered remediation task, adds it to the final audit dependencies, returns the audit to pending, and continues. `ralph.yml` permits up to 1000 iterations and a one-year runtime as safety ceilings. If those or an external session ceiling are reached, the plan remains active/blocked with a recovery handoff; a ceiling never constitutes completion.

### Completion protocol and checkpoint guards

Ralph recognizes a completion promise only when the reserved token is the exact final non-empty output line outside all `<event>` tags. A token inside any event payload is deliberately ignored. Each prompt therefore forbids its token in events, summaries, plans, and scratchpads and requires the standalone final line after the normal event is closed.

Before every checkpoint, planning revalidates the launcher's immutable specification metadata and cycle `base_commit`; maintenance planning performs its equivalent freshness check. `scripts/check-scratchpad.sh` requires one level-one handoff document, permits concise subsections within it, and rejects appended documents or completion tokens. Iteration-boundary hooks use `--allow-missing` because Ralph intentionally removes the previous scratchpad before the first iteration of a fresh, non-resumed loop. They also use `--allow-oversize` so a worker that slightly exceeds the 80-line or 8-KiB handoff target receives a warning without deadlocking the next iteration; structural and protocol violations still block. Final gates remain strict and reject missing, malformed, or oversized scratchpads.

A `pre.loop.complete` gate runs through `scripts/ralph-completion-gate.sh`. When that strict gate rejects a premature completion request, it writes an atomic, one-shot marker bound to the current launcher nonce, lifecycle mode, loop ID, and canonical workspace. The supervisor consumes only a matching marker, repairs Ralph's volatile markers, and continues the same cycle with `--continue`, preserving the selected TUI mode. Stale, malformed, mismatched, or symlink markers cannot authorize continuation, and arbitrary non-quota failures remain terminal. Quota exhaustion continues through its independent verified wait path. A failed or stale Ralph process can be accepted as complete only when the normal final gate passes; otherwise its artifacts remain recoverable but explicitly incomplete.

## Maintain one bug

Ordinary defects stay out of `docs/SPEC.md`. Canonical state is tracked in
`open-bugs.md` and `closed-bugs.md`, with optional manual references to GitHub,
Forgejo, or both. After human triage, run:

```bash
./scripts/ralph-maintenance-plan.sh BUG-0001
./scripts/ralph-maintenance-run.sh
```

A fresh invocation replaces the previous maintenance plan and scratchpad with a minimal selected-bug skeleton; prior evidence remains in Git and the closed ledger, while `--resume` preserves an interrupted draft. Every newly accepted maintenance task must be pending. The dedicated plan is bound to the immutable bug intake, committed spec, and
planning checkpoint. The single-writer maintenance loop adds regression tests,
implements the fix, runs the configured project verifier, records closure
evidence, and moves only that bug into the closed ledger. Contract changes are
blocked and returned to the specification workflow. See
[BUG_WORKFLOW.md](BUG_WORKFLOW.md).

## Adaptive concurrency

Configured ceilings live in `factory.toml`:

```toml
[concurrency]
adaptive = true
planning_subagents = 8
research_subagents = 8
review_subagents = 8
implementation_advisors = 2
mutating_workers = 1
integration_workers = 1
min_model_requests = 1
max_model_requests = 8
```

These are ceilings, not targets. The coordinating agent starts with the smallest useful fan-out and increases only for independent read-only work. Source mutation and integration remain serialized.

## Quota states

### Allowed

The session and weekly percentages are below `OLLAMA_THRESHOLD`; Ralph starts the next iteration.

### Waiting

At or above the threshold, the guard sleeps for `OLLAMA_WAIT_INTERVAL_SECONDS` and checks again. A zero `OLLAMA_WAIT_MAX_SECONDS` means unlimited waiting. SIGINT/SIGTERM still stop the process.

### Transient failure

Network and server failures are retried in wait mode. Single-check mode returns status 3 so supervisors can distinguish them from quota and credential failures.

### Fatal failure

Missing/expired cookies or an unparseable settings page return status 2 and require operator action:

```bash
source scripts/update-ollama-cookies.sh
```

## Quota waiting

Every iteration invokes:

```bash
./scripts/ollama-usage-guard.sh --wait
```

When session or weekly utilization reaches the configured threshold, the hook remains alive and polls until usage resets below it. Transient network errors are retried. Expired cookies stop with an actionable error rather than waiting forever.

Useful settings in `.ollama-usage-env`:

```bash
OLLAMA_THRESHOLD=80
OLLAMA_WAIT_INTERVAL_SECONDS=300
OLLAMA_WAIT_MAX_SECONDS=0  # unlimited
```

If the backend reaches quota during an already-running request, `scripts/ralph-run.sh` checks quota, waits, repairs runtime markers, and resumes with `--continue`.

## Clean stop

In TUI or foreground mode, press `Ctrl+C`. Ralph aborts the backend and leaves durable state for recovery. Do not use `kill -9` unless the process cannot terminate normally.

For a headless process, read `.ralph/loop.lock` and send SIGINT to its PID from the host.

## Recovery

1. Confirm no Ralph process is alive.
2. Run:

   ```bash
   ./scripts/ralph-recover.sh --dry-run
   ```

3. Check the inferred loop ID and event stream.
4. Resume:

   ```bash
   ./scripts/ralph-recover.sh
   ```

The script restores a missing tracked scratchpad, removes only a stale lock, recognizes timestamped and fallback event streams, reconstructs pointer files, and starts `ralph-run.sh --resume`.

If unfinished runtime tasks belong to multiple loop IDs, recovery refuses to guess; pass the intended ID explicitly:

```bash
./scripts/ralph-recover.sh --loop-id primary-YYYYMMDD-HHMMSS
```

Planning and maintenance recovery use:

```bash
./scripts/ralph-recover.sh --mode planning
./scripts/ralph-recover.sh --mode maintenance-planning
./scripts/ralph-recover.sh --mode maintenance
```

New loops persist their lifecycle mode and recovery rejects a mismatched mode.
Recovery never resets Git or starts a second writer.

## Specification changes

Never edit the specification during implementation. `check-plan-freshness.sh` compares both the latest spec commit and the exact Git blob against plan metadata. If they differ:

1. stop the implementation loop;
2. commit the revised `docs/SPEC.md`;
3. run `./scripts/ralph-plan.sh`, which seeds minimal plan/scratchpad state and leaves the completed plan only in Git history;
4. inspect the replacement plan and confirm it contains only current pending gaps;
5. start a new implementation loop.

## Documentation gate

Every implementation plan ends with **Final documentation and specification audit**. `scripts/validate-implementation-plan.py` requires the plan to contain a conformance matrix, interaction inventory, canonical task statuses, and a final audit depending on every other task. At implementation completion it rejects unfinished tasks and any matrix classification other than `verified`. The final gate also validates the bug ledgers and rejects unresolved open bugs before running documentation, boilerplate, and project verification.

Read-only reviewers compare source, tests, configuration, README, operations, and the specification, specifically looking for tests that bypass production initialization/event dispatch or assert pixels without semantic behavior. The sole writer corrects documentation and runs final verification. If review finds a gap, Ralph appends remediation and continues; `LOOP_COMPLETE` is forbidden until the complete §11.2 definition of done passes.

## Verify

```bash
./scripts/verify-boilerplate.sh
```

The verifier checks shell syntax, ShellCheck when available, TOML/JSON configuration, read-only agent tools, single-writer settings, quota behavior, plan freshness, branch policy, removed product artifacts, and secret tracking.

## Release

After Ralph reports completion, review `develop`. Release manually:

```bash
git switch main
git merge --no-ff develop
git tag vX.Y.Z
```

For the next release, update the same `docs/SPEC.md` in a dedicated commit, run a new planning loop, and execute a new implementation loop. Git retains prior specifications and plans.