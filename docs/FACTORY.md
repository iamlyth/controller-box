# Factory Boilerplate

This document covers the fresh-context software factory used to implement
Controller-Box. It is not relevant to end users — it documents the autonomous
development loop, branch policy, quota management, recovery, and evidence
machinery.

The factory boilerplate is retained on the `develop` branch only. End users
should refer to [README.md](../README.md) for product documentation.

## What this is

A reusable, single-writer implementation of Geoffrey Huntley's Ralph Wiggum
development technique using a fresh Python control plane, jailed Pi, Ollama,
four static model roles, and Git/plan/state-based crash recovery — without
Ralph Orchestrator as the control plane. The methodology contract is
`docs/FACTORY-LOOP-SPEC.md`; the product contract remains `docs/SPEC.md`.

`docs/SPEC.md` is the source of truth for the product.
`.factory/artifacts/implementation-plan.md` tracks task status and
verification evidence. `docs/FACTORY-LOOP-SPEC.md` specifies the control
plane, phases, roles, evidence tiers, and acceptance predicates and
supplements (never replaces) the product specification.

## Operating model

- `main` is the human-controlled release branch.
- `develop` is the autonomous implementation branch.
- One committed `docs/SPEC.md` is the source of truth; Git versions it.
- A fresh planner process creates/revises `.factory/artifacts/implementation-plan.md`
  (schema `factory-plan/v1`, parsed by `.factory/loop/plan_parser.py`) for the exact spec commit.
- Each implementation iteration selects one deterministic task from the plan
  (`.factory/loop/selector.py`: priority, then task ID) and starts a fresh
  developer process with no resumed session and no memory injection.
- The four static roles — planner, developer, tester, auditor — are the
  complete model-role set; adaptive specialist subagents and parallel model
  launches are not part of the control plane.
- Exactly one repository writer (the developer) may edit, stage, or commit
  repository files. The planner may modify only the plan; the tester and
  auditor are read-only except for trusted receipt publication.
- Tests, documentation, runner evidence, and audit are deterministic
  completion gates.
- You review `develop` and manually promote it to `main`.

No Git worktrees are used.

## Relationship to Huntley's playbook

The prompts are periodically compared against
[`ghuntley/how-to-ralph-wiggum`](https://github.com/ghuntley/how-to-ralph-wiggum)
(reviewed at commit `88d488a148af97e4a3f22b11b4c3598c79d6a577`). Controller-Box
adopts its highest-value context and backpressure patterns:

- deterministic orientation: study the specification, plan, concise `AGENTS.md`, source, tests, and shared patterns every iteration;
- **do not assume functionality is missing** — search and trace production behavior first;
- keep the primary context as scheduler and use fresh read-only role processes as disposable context;
- derive tests from behavioral acceptance criteria, including performance and edge cases, while leaving implementation choices to the worker;
- keep operational learning in brief `AGENTS.md`, progress/evidence in the plan, and only what Git/plan/state can re-derive for recovery;
- update the plan immediately when discoveries create work, implement completely without placeholders, investigate unrelated failures, and use tests/build/lint/install checks as backpressure;
- capture why tests and documentation constraints matter.

Deliberate safety differences are retained: four static roles in fresh
processes rather than many mutating agents; one repository writer and
serialized builds; a jailed Pi backend rather than skipped permissions; no
worktrees; no autonomous specification edits; no pruning of the active-cycle
ledger; no automatic push, tag, or promotion to `main`. Fresh planning still
discards the prior active plan from working context while Git preserves its
history.

## Durable and volatile state

Durable, tracked state:

- `docs/SPEC.md`: approved requirements (canonical product contract)
- `docs/FACTORY-LOOP-SPEC.md`: methodology contract (supplements the spec)
- `AGENTS.md`: concise build/run/validation commands and durable operational patterns
- `.factory/artifacts/implementation-plan.md`: feature task status and verification evidence
- `.factory/artifacts/maintenance-plan.md`: one selected bug, fingerprint, tasks, and evidence
- `.factory/bugs/open.md` / `.factory/bugs/closed.md`: portable canonical defect state
- source, tests, README, and operational documentation
- `.factory/loop/`, `.factory/bin/`, `.factory/prompts/`, `.factory/schemas/`: the fresh Python harness (hidden namespace)
- `.factory/environment.toml`, `.factory/config.toml`, `.factory/capability-contracts.json`: declarations
- `.factory/ralph-freeze`: non-executable tombstone (see below)

Volatile, ignored state:

- `.factory-state/` — the current-user-owned mode-0700 runtime root for
  coordinator/runner evidence, audit receipts, and production campaigns
- `.factory-state/campaigns/<campaign-id>/` — one fresh mode-0700 private
  production campaign state/result/receipt namespace, created no-replace and
  never shared; reservation lstats only the exact state root and fixed parent
  and never enumerates, reads, renames, removes, or overwrites foreign entries.
  A canonical five-round production run begins in `readiness` at round zero.
  Static preflight and held descriptors precede coordinator-only runner
  acquisition; strong aggregate validation, required capability/core checks,
  conformance core-row mapping, and the committed three-editor-state human
  graphics approval all precede planner 1. `readiness-result.json` is atomically
  published and digest-bound into control/campaign state. Interrupted acquisition
  is ambiguous and is never rerun; only a same-nonce completed aggregate is reused
- `.bug-ledger.lock`, `.ollama-usage-env`, `logs/`, test fixtures

**Retired Ralph control plane.** `.factory/ralph-freeze` is a tracked,
non-executable tombstone: it records that Ralph Orchestrator launchers,
prompts, queues, scratchpads, summaries, lifecycle tokens, and resumed-session
state have been removed and that the fresh `.factory/loop/` control plane is
authoritative. It is documentation only and is never executed. Ignored foreign
`.ralph/` bytes left over from earlier checkouts are never read, imported,
bound into prompts, or deleted by the factory; a `.ralph/**` path may only be
deleted from the index and never added or modified again. Historical plans,
bug records, and audit reports may still quote the old system, and the
tracked schemas keep their historical `ralph-*` names for compatibility, but
no operative control flow depends on any of them.

## Branch policy

The autonomous lifecycle runs only on `develop`. `main` is protected by policy
and never modified by the factory. `scripts/branch-guard.sh` also rejects
multiple Git worktrees and non-`develop` autonomous branches.

## Prerequisites

- Python 3.11+ (the control plane is standard-library Python), Bash, Git, flock, and optionally ShellCheck
- The jailed Pi2 runtime with the secure wrapper and authenticated fixed-provider/model access
- A clean `develop` branch with at least one commit

## Initial setup

1. Merge this boilerplate into `develop`.
2. Configure Ollama usage credentials:

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

A planning phase runs inside the finite campaign (below) as the first phase of
each round. The planner is the only role that creates, removes, splits, or
reorders plan tasks; it may modify only `.factory/artifacts/implementation-plan.md`.
A fresh invocation atomically replaces the active plan with minimal cycle
state before the planner starts, so completed tasks are not carried into
future prompts. Previous plans remain available through Git history. The
generated plan records:

- the spec path, the latest commit that changed the spec, and the exact spec blob ID;
- the cycle base commit;
- a requirement-by-requirement specification conformance matrix;
- an exhaustive manager-control and overlay-action interaction inventory;
- bounded tasks with dependencies, acceptance checks, and documentation impact;
- a mandatory final documentation/specification audit that depends on every other task.

Inspect the plan before implementation. Every newly accepted task must be
`pending`; inherited completed, in-progress, or blocked tasks fail the
planning gate. Every `partial`, `missing`, or `ambiguous` conformance row must
map to a task. `scripts/check-plan-freshness.sh` prevents a stale plan or
altered cycle base from running after the specification changes.

## Implement

A finite campaign drives planning, implementation attempts, verification, and
audit rounds through one trusted orchestrator:

The operator first accepts and commits the implementation on `develop`, then
uses a clean exact commit, a fresh external production install, and a unique
campaign ID:

```bash
ACCEPTED_COMMIT=$(git rev-parse HEAD)
test -z "$(git status --porcelain --untracked-files=all)"
INSTALL_PARENT=$(mktemp -d)
INSTALL_PREFIX="$INSTALL_PARENT/controller-box-harness"
INSTALL_MANIFEST="$INSTALL_PARENT/install-manifest.json"
CAMPAIGN_ID="controller-box-$(date +%Y%m%dT%H%M%S)-$$"
python3 .factory/loop/installer.py install --root "$PWD" --commit "$ACCEPTED_COMMIT" --prefix "$INSTALL_PREFIX" --manifest-out "$INSTALL_MANIFEST"
python3 "$INSTALL_PREFIX/.factory/loop/installer.py" verify --root "$PWD" --commit "$ACCEPTED_COMMIT" --prefix "$INSTALL_PREFIX" --manifest "$INSTALL_MANIFEST"
"$INSTALL_PREFIX/.factory/bin/factory-campaign" --root "$PWD" run --campaign-id "$CAMPAIGN_ID" --rounds 5 --branch develop --provider "${PI_PROVIDER:?set provider}" --model "${PI_MODEL:?set model}" --backend "${PI2_BACKEND:?set trusted pi2 executable}" --accepted-commit "$ACCEPTED_COMMIT" --install-manifest "$INSTALL_MANIFEST" --campaign-timeout 21600 --verification-command ./scripts/verify-project.sh --runner-command ./scripts/run-factory-runners.py --capability-command ./scripts/check-capability-evidence.py --acceptance-command '["./scripts/final-gate.sh","--implementation"]'
```

Production has no synthetic provider/model/backend/gate/deadline defaults. Runner
acquisition is an explicit coordinator-only phase: the exact no-shell argv is
bound before roles launch, rerun after every HEAD change, and followed by the
strong signed aggregate checker immediately before capability validation.
Transport unavailability is retained as an honest finding/blocker; command,
protocol, or aggregate-integrity failures are infrastructure failures. Before
any role, the installed campaign bytes prove their production manifest and
accepted commit, bind the exact verification, capability-evidence, and final
acceptance argv, recheck branch and clean status, and create the no-collision mode-0700
`.factory-state/campaigns/$CAMPAIGN_ID/` namespace. The pre-existing
`.factory-state` root must be a real current-user-owned mode-0700 directory;
only the exact root/fixed parent components are lstat'ed, never enumerated.
State, structured role results, receipts, and final result stay in the fresh
child; foreign entries retain their bytes, mode, and mtime. `.factory/bin/factory-launch` remains the single-role
supervisor; the installed `.factory/bin/factory-campaign` owns finite rounds.

Each campaign round:

1. validates branch, plan freshness, and the single control-state file;
2. runs every enabled exact-commit pre-round hook once, in registry order, before the planner;
3. selects one ready task deterministically (priority, then task ID) from the committed plan;
4. launches one fresh developer process for that task, with a byte-bound task excerpt;
5. implements and tests one task with one writer;
6. updates the plan task status and evidence, and commits one coherent checkpoint;
7. exits so the next task receives fresh context.

Outcomes are derived from plan state, Git state, exit status, and
deterministic gates, never from model completion tokens. Planner, tester, or
auditor nonzero exit is failing even if it left syntactically valid output.
The 21600-second wall-clock deadline bounds the whole campaign, including
quota waits. Per-task completion uses the exact verification command as its
deterministic acceptance contract; final success additionally requires the
capability-evidence command and `final-gate.sh --implementation` (which checks
capability contracts/evidence, conformance, blocked facts, and final product
acceptance) to exit zero without a skip marker:
`task_completed`, `task_progress`, `task_failed`, `interrupted`,
`work_exhausted`, or `blocked`. `work_exhausted` and `blocked` are not product
acceptance. A failed or interrupted attempt retries the same task while its
bounded attempt budget remains; exhausting the budget with dirty work
terminates the campaign `interrupted`, and a clean reproducible task failure
records a finding and proceeds to verification/audit at the last coherent
commit.

There is no minimum iteration count: quality is determined by evidence, not
volume. Completing the originally planned tasks is not enough when acceptance
discovers another gap: the planner appends a uniquely numbered remediation
task, adds it to the final audit dependencies, and continues. The
implementation plan is the sole task ledger; no runtime task queue, memory
store, or context summary participates in task selection.

## Finite campaign semantics

Each round is `planning -> implementation -> verification -> audit`. The
campaign never remains indefinitely in implementation merely because external
acceptance is unavailable: `work_exhausted`/`blocked` still run verification
and audit, whose findings feed the next round's planner revision.

Finite outcomes (documented in FACTORY-LOOP-SPEC §13–§15):

- `success`: final-round product acceptance and audit pass;
- `findings`: unresolved software/test/documentation/security/audit defect in the final round (or mid-campaign verification findings);
- `blocked`: no software/test finding remains and every unresolved item requires unavailable external/hardware/capability/human authority;
- `failed`: planning attempts exhausted;
- `interrupted`: dirty implementation-attempt exhaustion or operator/process interruption;
- `infrastructure_failure`: untrusted verifier/control-plane failure (fails closed).

One mutable control-state file,
`.factory-state/campaigns/<campaign-id>/factory-loop.json`, records
exactly the schema, repository identity, branch, campaign ID, rounds, current
round, current phase, specification/plan/prompt-set digests, phase base
commit, selected task ID, attempt counters, and a trusted `last_outcome` enum.
It contains no model prose, memories, or evidence claims. All writes are
atomic, no-follow, ownership/mode/link-count checked, and validated against
the documented phase transition table; any same-UID mutation not produced by
the trusted transition fails closed. Phases never move backward within a
round, and round advances only on `audit --nonfinal`.

The trusted control plane holds an exclusive `flock` on an already-open
canonical repository-root directory descriptor and closes it in every child
before exec, so untrusted model processes inherit no lock authority and cannot
unlock the holder through a separately opened descriptor. Only the developer
role may modify product code; the planner may modify only the plan; tester and
auditor are read-only. The Git command-boundary guard
(`scripts/git-commit-guard.sh` hooks + `scripts/pi-cli-shims/git`) rejects
hook bypass, hook-path overrides, alternate worktrees, amend/merge/rebase
bypasses, and forged handoffs; only `git commit` creates commits from the
model boundary, and every commit must carry at least one substantive tracked
path.

## Recovery

Recovery is derived from Git, the canonical plan, the one control-state file,
and process liveness — there is no separate recovery launcher and no resumed
model session.

- A clean committed task resumes from the next deterministic task.
- An `in_progress` task resumes from current code and Git diff in a fresh
  context; its tests are rerun and its untrusted prose is not preserved as memory.
- An ambiguous live process, changed repository identity, changed branch,
  unsafe state file, stale specification binding, changed plan base, rewound
  counter, or invalid state transition fails closed for human/operator review.
- Recovery never resets, discards, or silently overwrites dirty work; a
  `scratchpad` is not part of the system.

## Declared tools and runners

`.factory/environment.toml` is the tracked, credential-free declaration of
what the factory can actually execute. Agents must not invent undeclared
hardware, GPU/controller coverage, or external evidence. Hostnames,
usernames, ports, private-key paths, passwords, tokens, and secrets remain
outside Git.

The declared runner classes in `.factory/environment.toml` are:

- `dev-runner-vm` (SSH transport) declares `remote-project-gate`,
  `systemd-user`, `kernel-uinput`, and `installed-package`.
- `iprunner` (SSH transport) declares `inputplumber-system-dbus`,
  `physical-controller`, `target-consumer`, and
  `controller-production-routing`.
- `gpurunner` (SSH transport) declares `gpu-compositor` and
  `installed-licensed-diagram`.

The six `iprunner`/`gpurunner` capabilities are declared contracts. Routing
requires four independently consumed real-source events; installed diagram
evidence requires licensed hashes, exact model/no fallback, accelerated capture,
and independent-oracle alignment. The previously
designed-only `candidate` contracts are now `declared` contracts in
`.factory/capability-contracts.json`, each with a `runner_class` matching a
declared runner. The root runner endpoint refuses to execute a contract
until the capability is declared, its contract is promoted to `declared`,
and the runner-class allowlist grants it. Declared is distinct from
provisioned/evidenced: a declared runner class makes the runner-class
binding explicit, but a capability is evidenced only by an exact-commit,
non-skipped, signed runner receipt. No such receipt exists yet for the
`iprunner`/`gpurunner` capabilities, so Controller-Box's open
hardware/GPU/system-bus/target boundaries — BUG-0015 and BUG-0018, and facts
FACT-002 through FACT-007 — stay open and non-elevated until their
requirement-specific real-system, target-hardware, signed current-commit
runner, or human evidence exists. The legacy `26df6c0` receipt is
unsigned/unevidenced. Valid signed evidence exists for the older `c45336a`
commit, but it is historical and stale; neither is claimed as current runner
evidence (Task 26).

Validation:

```bash
./scripts/check-factory-environment.py
```

During verification, the installed campaign coordinator invokes its bound
`scripts/run-factory-runners.py` descriptor (never a model role) to create a history-free
`git archive` of the exact clean commit, rejects tracked symlinks, gitlinks,
or special modes that this protocol cannot reproduce safely, sends the
archive through the pinned SSH alias, verifies the extracted Git tree
remotely, runs the fixed argv without reusing a checkout or HOME, and cleans
the remote workspace. Local receipts and bounded logs are written beneath
`.factory-state/runner-evidence/` and validated by
`scripts/check-factory-runner-evidence.py`. Transport or remote-verification
unavailability becomes honest findings/blocked capability evidence; a command,
tree, protocol, signer, or aggregate-integrity failure becomes campaign
infrastructure failure.

Runner receipts are signed by a root-owned signer on the disposable runner VM
(`scripts/factory-runner-signer.py`, installed root-owned and reached only
through a narrow sudoers rule). The unprivileged forced-command endpoint never
signs: after the exact archive/tree/environment/verifier/probes all pass, the
root signer re-validates every manifest field (clean pass only, supported
capabilities, bound digests, no caller-supplied signer identity), rebuilds the
canonical signed manifest itself, and returns the detached signature plus
aggregate signer metadata. The private signing key is root-owned mode 0600 on
the runner, never printed or copied into Git. The signer resolves authorization
from sudo's numeric caller UID (with NSS name/UID consistency), not from a
caller-supplied class name; `devrunner` is therefore bound by policy to class
`dev-runner-vm`, while `iprunner` and `gpurunner` remain isolated classes. It
opens the key, principal, and a fixed absolute root-owned `ssh-keygen` through
validated non-symlink chains and uses descriptor-bound inodes.

This repository carries only canonical two-field ed25519 public keys in
`.factory/signer-trust.json`, exactly one distinct key per declared principal.
The checker reads issuance trust from the exact evidence commit and revocation
trust from committed `HEAD`, never from mutable worktree bytes, and requires
the same exact principal/key pair in both. Schema v1 has no explicit rotation
window, so multiple keys for one principal fail closed; commit the replacement
only when old receipts are intentionally revoked. Signer principal, aggregate
runner, manifest runner, and declared runner class must all be identical.

`.factory/config.toml` lists product-specific capabilities required for a
clean audit. Only capabilities covered by accepted exact-commit evidence
count; all others remain findings until their production probes and artifacts
are implemented. Runner provisioning and credentials are maintained outside
this repository.

## Maintain one bug

Ordinary defects stay out of `docs/SPEC.md`. Canonical state is tracked in
`.factory/bugs/open.md` and `.factory/bugs/closed.md`, with optional manual
references to GitHub, Forgejo, or both. After human triage, the selected bug
and cycle base are recorded in ignored `.factory-state/`
(`scripts/factory-state-file.py` reads/writes `maintenance-bug-id` and
`maintenance-base-commit`), and a canonical
`.factory/artifacts/maintenance-plan.md` is validated by
`scripts/validate-maintenance-plan.py planning|complete` and
`scripts/check-maintenance-freshness.sh`. The maintenance lifecycle runs
through the same fresh-context control plane as implementation (fresh planner
seeds the minimal selected-bug skeleton, fresh developer implements and tests,
verification runs the configured project verifier, and the final maintenance
audit closes the ledger record). Every newly accepted maintenance task must be
pending; contract changes are blocked and returned to the human specification
workflow. See [BUG_WORKFLOW.md](BUG_WORKFLOW.md).

```bash
./scripts/bug-ledger.py validate
```

## Quota states

### Allowed

The session and weekly percentages are below `OLLAMA_THRESHOLD`; the control plane starts the next attempt.

### Waiting

At or above the threshold, the guard sleeps for `OLLAMA_WAIT_INTERVAL_SECONDS` and checks again. A zero `OLLAMA_WAIT_MAX_SECONDS` means unlimited waiting. SIGINT/SIGTERM still stop the process.

### Transient failure

Network and server failures are retried in wait mode. Single-check mode returns status 3 so the control plane can distinguish them from quota and credential failures.

### Fatal failure

Missing/expired cookies or an unparseable settings page return status 2 and require operator action:

```bash
source scripts/update-ollama-cookies.sh
```

## Ordered pre-round hooks

`.factory/pre-round-hooks.json` is an exact-commit ordered registry of fixed,
mandatory control-plane implementations. Every enabled hook runs once before
each round's planner; planner retries do not rerun it. A durable started cursor
prevents an ambiguous crash from causing duplicate execution, and canonical
typed result digests are chained into `factory-loop.json` before planning.
The committed registry currently contains only the enabled mandatory branch
guard. No Ollama quota hook is implemented or configured; adding one is a
future explicit change. Per-model `authorize_launch` has no quota/cookie
options and performs no usage check. Existing standalone operator usage tools
remain separate from campaign hook execution. Their optional settings live in
the operator-owned `.ollama-usage-env`:

```bash
OLLAMA_THRESHOLD=80
OLLAMA_WAIT_INTERVAL_SECONDS=300
OLLAMA_WAIT_MAX_SECONDS=0  # unlimited
```

## Clean stop

In the foreground, press `Ctrl+C`; the campaign forwards TERM/INT/HUP/QUIT to
the current role child, bounds the reap, and leaves durable state for
recovery. Do not use `kill -9` unless the process cannot terminate normally.
For a headless campaign, read the explicitly selected
`.factory-state/campaigns/<campaign-id>/factory-loop.json` to identify
the running campaign and phase, then send SIGINT to the orchestrator PID.
Recovery is Git+plan+state derived (see above); there is no event stream or
loop-lock file to repair.

## Specification changes

Never edit the specification during implementation.
`check-plan-freshness.sh` compares both the latest spec commit and the exact
Git blob against plan metadata. If they differ:

1. stop the campaign;
2. commit the revised `docs/SPEC.md`;
3. start a new planning phase, which seeds minimal plan/state and leaves the completed plan only in Git history;
4. inspect the replacement plan and confirm it contains only current pending gaps;
5. start a new implementation campaign.

## Documentation gate

Every implementation plan ends with **Final documentation and specification
audit**. `scripts/validate-implementation-plan.py` requires the plan to
contain a conformance matrix, interaction inventory, canonical task statuses,
and a final audit depending on every other task. At implementation completion
it rejects unfinished tasks and any matrix classification other than
`verified`. The final gate also validates bug ledgers, rejects unresolved open
bugs, runs project verification, and requires commit-bound
`test_installed_functional` evidence with zero skips. `scripts/verify-project.sh`
writes local evidence only after the mandatory test and packaging gates pass;
`scripts/check-installed-functional-evidence.sh` invalidates it if production
or acceptance inputs change afterward. This prevents string mocks, keyboard
proxies, fixture assembly without production dispatch, missing-backend skips,
or optional smoke skips from satisfying installed production behavior.
`scripts/check-docs-sync.sh` additionally requires README/docs to change
whenever implementation changes.

Read-only reviewers (tester, auditor) compare source, tests, configuration,
README, operations, and the specification — looking for tests that bypass
production initialization/event dispatch or assert pixels without semantic
behavior. The developer corrects documentation and runs final verification.
If review finds a gap, the planner appends remediation and continues;
completion is forbidden until the complete definition of done passes.

## Machine-readable acceptance evidence

Proxy evidence must not be promoted to production verification. Four
tracked artifacts make acceptance machine-checked:

- `.factory/artifacts/conformance.json` (schema `ralph-conformance/v1`, kept
  for compatibility) is the only authority for `verified` claims. Each
  requirement row declares classification (`verified`/`partial`/`missing`/
  `ambiguous`/`blocked`/`not_applicable`), evidence tier (`unit`/`simulated`/
  `private_integration`/`installed`/`real_system`/`human`), required
  capabilities, the exact evidence commit, and receipt/artifact refs.
  `scripts/validate-conformance.py planning|complete` checks the schema,
  cross-checks the plan matrix, and rejects `verified` rows below the
  normative tier, unevidenced, or backed by an undeclared capability.
- `.factory/capability-contracts.json` (schema `ralph-capability-contract/v1`)
  defines one probe per declared/required capability: probe argv,
  must-execute marker, must-not-skip tokens, deny-simulated markers.
  `scripts/check-capability-contracts.py` rejects contracts for undeclared
  capabilities and declared capabilities without contracts;
  `scripts/check-capability-evidence.py` requires a fresh exact-commit runner
  receipt whose probe section executed (no skip) and shows no simulated
  marker. Missing contract, probe, or receipt is unevidenced and never
  auto-reclassified.
- Audit reports must cite machine receipts: coordinator-executed commands are
  wrapped by `scripts/machine-receipt.py --tag <tag> -- <argv...>` and
  recorded under `.factory-state/audit-receipts/`. Each command runs below a
  fresh command-only subreaper broker; the coordinator waits only for that
  broker. A trusted direct-child stage stops before command exec and retains
  only the read end of a private CLOEXEC authorization pipe. SIGSTOP is a
  scheduling barrier, not authorization: after any resume the stage blocks
  and accepts only the exact unpredictable one-shot token. The broker opens
  and revalidates the stage pidfd, proves signal-0, revalidates the stopped
  identity, resumes through that pidfd, and only then writes the token. A
  pidfd support/resource failure closes the pipe without a token, kills and
  reaps the retained stage, and starts no command. The capability descriptor
  and token are consumed before exec and never reach command/model argv,
  environment, descriptors, transcripts, or logs. Broker-proven identities
  are pidfd-signaled and reaped before the retained leader status is consumed.
  Unrelated coordinator children, their workers, and their wait statuses are
  outside receipt ownership.
  `scripts/check-audit-receipts.py` requires every executable-evidence line to
  carry PASS/FAIL/BLOCKED plus a `[receipt: ...]`/`[manifest: ...]` reference,
  PASS requires exit 0, and any BLOCKED evidence forces `result: findings`.
  Subagent prose cannot certify runtime.
- Runner/capability receipts remain signed, exact-commit, non-skipped, and
  non-simulated (`.factory/signer-trust.json` public keys only; the private
  key stays on the disposable runner).

Pixel/offscreen framebuffer checks are not real visual acceptance,
private/session DBus is not the real system service, a uinput producer is not
the target consumer, and an evidence declaration is not evidence.
`final-gate.sh` `--planning|--implementation|--campaign-audit|--maintenance-planning|--maintenance` run the relevant layers; the campaign is accepted
only when the evidence actually exists.

## Blocked facts, campaign objectives, and goldens

- `.factory/artifacts/blocked-facts.json` (schema `ralph-blocked-facts/v1`) is
  an append-only ledger. Every requirement whose evidence is unavailable
  (undeclared capability, missing system service, missing hardware target,
  open product defect, or pending human decision) is an `open` fact;
  `blocked`/`partial` conformance rows reference it through `fact_refs`.
  `scripts/validate-blocked-facts.py planning|complete` enforces strictly
  ascending unique IDs and resolution discipline: a `receipt` resolution needs
  an exact clean-pass machine receipt at the evidence commit, an `artifact`
  resolution needs a real non-documentation artifact (`.md` files can never
  resolve a normative requirement), and a `decision` resolution requires an
  explicit human identity plus a specification-permitted location. Completion
  fails while any fact is open.
- `.factory/campaign-objectives.json` (schema `ralph-campaign-objectives/v1`)
  binds each audit round to one product-neutral falsification objective with
  its own receipt categories. `scripts/check-campaign-objectives.py` requires
  the round's report to carry machine receipts (or accepted runner manifests)
  covering every category of that round's objective; replaying the same
  generic suite cannot satisfy all rounds.
- Golden baselines are protected by `.factory/golden-policy.json` and
  `.factory/golden-review.json` (schema `ralph-golden-review/v1`). Generation
  can never overwrite active goldens: `scripts/generate-golden.sh` refuses to
  run without a review manifest path and human reviewer identity, and
  `scripts/check-golden-policy.py` requires every working-tree golden change
  to carry an exact before/after SHA-256 plus reviewer/human identity;
  committed changes stay valid against real Git transitions. Never regenerate
  goldens merely to make a test pass.

## Machine visual audit

The optional visual-audit framework captures serialized, installed
exact-commit Controller-Box states and binds each image to its bytes, commit,
tree, environment, prompt, schema, model, role, state, and request nonce.
Review may run in parallel only after immutable captures exist. A current
non-skipping probe and independently accepted calibration controls are
required before live review. Machine vision is supplemental falsification
evidence: it may add findings but never elevates an evidence tier, certifies a
golden, replaces compositor/physical/target-consumer evidence, or substitutes
for human acceptance. Completion runs `scripts/visual-audit-gate.sh`, which
only validates an existing report and never captures or invokes a model under
the lifecycle lock.

Controller-Box fixes production machine review to the authenticated Pi2
identity `ollama/kimi-k2.6`; the SDK treats its `--model` argument only as an
exact binding assertion and refuses caller-selected production models. The
framework remains disabled. A genuine non-skipping image-byte probe is
required at the exact framework commit, and activation is currently blocked
because the tracked calibration declarations have no accepted image SHA-256
bindings and the required known-bad/current-bad/human-reviewed-good calibration
PNGs are absent. These controls must be independently supplied and calibrated
before `enabled = true`; no synthetic fixture or machine verdict may stand in
for the human-reviewed-good prerequisite.

Mutable captures, receipts, leases, and reports remain ignored factory state;
tracked configuration contains no credentials. Machine review can only add
supplemental falsification findings: it cannot approve a golden, raise an
evidence tier, satisfy FACT-006, or establish hardware, real-system-service,
target-consumer, or runner claims.

## Credential boundary guard

`scripts/credential-guard.py` and the project-local Pi extension block
sensitive command/path reads before execution and redact tool-result strings
before display or session persistence. Candidate command/output text is sent
to the guard on standard input, never process arguments. Guard errors fail
closed, direct file paths and literal Bash path arguments resolve symlinks,
and nested result values are redacted by one bounded subprocess. Git boundary
redirection and hook-bypass forms are rejected case-insensitively.

## Verify

```bash
./scripts/verify-boilerplate.sh
```

The verifier checks shell syntax, ShellCheck when available, TOML/JSON
configuration, role prompt integrity, single-writer settings, quota behavior,
plan freshness, branch policy, removed legacy artifacts (including the
`.ralph` and legacy `ralph-*` removal), and secret tracking.

## Release

After the factory reports completion, review `develop`. Release manually:

```bash
git switch main
git merge --no-ff develop
git tag vX.Y.Z
```

For the next release, update the same `docs/SPEC.md` in a dedicated commit,
run a new planning phase, and execute a new implementation campaign. Git
retains prior specifications and plans.
