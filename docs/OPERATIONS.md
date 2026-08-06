# Factory Operations

## Durable and volatile state

Durable, tracked state:

- `docs/SPEC.md`: approved requirements
- `IMPLEMENTATION_PLAN.md`: task status and verification evidence
- `.ralph/agent/scratchpad.md`: concise crash handoff
- source, tests, README, and operational documentation
- `factory.toml`, Ralph configs, prompts, and project subagent definitions

Volatile, ignored state:

- event streams and pointer files under `.ralph/`
- loop locks, diagnostics, API state, task/memory stores, and TUI exports
- Pi transcripts and scheduled-agent state
- `.factory-lock`
- `.ollama-usage-env`

Git checkpoints make the plan, scratchpad, and implementation recoverable. Event/task files improve same-disk recovery but are not treated as portable project history.

## Branch policy

The autonomous lifecycle runs only on `develop`. `main` is protected by policy and never modified by the factory. `scripts/branch-guard.sh` also rejects multiple Git worktrees.

A boilerplate experiment on a `factory/*` branch requires the explicit temporary override:

```bash
FACTORY_ALLOW_TRIAL_BRANCH=1 ./scripts/ralph-plan.sh
```

Do not carry this override into normal development.

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

## Specification changes

Never edit the specification during implementation. `check-plan-freshness.sh` compares both the latest spec commit and the exact Git blob against plan metadata. If they differ:

1. stop the implementation loop;
2. commit the revised `docs/SPEC.md`;
3. run `./scripts/ralph-plan.sh`;
4. inspect the replacement plan;
5. start a new implementation loop.

## Documentation gate

Every implementation plan ends with **Final documentation and specification audit**. Read-only reviewers compare source, tests, configuration, README, operations, and the specification. The sole writer corrects documentation and runs final verification. `LOOP_COMPLETE` is forbidden until this gate passes.

## Icon mapping

Controller-Box maps InputPlumber `DeviceType` strings to SVG icons using
`/usr/share/controller-box/controller-icons.yaml` (SPEC §8.4).  The mapping file
is a YAML list of entries with `type`, `icon`, and `name` fields.

At runtime, `cbx_icon_map_load()` parses the YAML, and
`cbx_icon_map_lookup(type, ...)` returns the icon name and display name for a
given `DeviceType`.  Unknown types fall back to `generic-gamepad` with the raw
type string as the label.

SVG files live in `/usr/share/controller-box/icons/svg/`.  Controllercons
icons are prefixed `cc-` (e.g. `cc-xbox-360`, `cc-ps5`).  Custom icons use
plain names (`steam-deck`, `generic-gamepad`, `arcade-stick`, `hitbox`,
`mouse`, `keyboard`).

To add a new device type mapping, append an entry to `controller-icons.yaml`
under `virtual_types:`.  To add a new icon, place the SVG in `data/icons/svg/`
and reference it by filename (without `.svg`).

## Troubleshooting

- **`expected develop`**: merge/switch to `develop`; use the trial override only for this boilerplate branch.
- **`exactly one working tree`**: remove stale worktrees and run `git worktree prune`.
- **`plan is unplanned`**: run the planning loop.
- **`specification changed after planning`**: commit the spec and replan.
- **`another factory process holds .factory-lock`**: confirm the existing planner/worker is stopped before deleting a stale `.factory-lock`.
- **quota wait appears idle**: the guard prints each usage poll; lower the polling interval temporarily for diagnostics.
- **cookie expired**: refresh with `source scripts/update-ollama-cookies.sh`.
