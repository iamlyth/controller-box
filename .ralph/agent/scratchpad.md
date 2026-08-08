# Task 11: Factory backpressure against false completion — COMPLETE

## Outcome
- All acceptance criteria met. Commit `aa39146` on `develop`.

## Changes
- **PROMPT.md** step 6: explicitly lists keyboard proxies, string-only mocks, fixture assembly without production dispatch, missing-backend skips, and bypass tests as categories that cannot mark production requirements verified.
- **prompts/PLAN.md** conformance matrix section: same explicit enumeration; adds "skips of any kind cannot satisfy `verified`."
- **scripts/verify-boilerplate.sh**: 8 new grep checks verify both prompts contain "Keyboard prox", "string-only mock", "fixture assembly", "missing-backend".
- **tests/test-installed-functional-evidence.sh**: enhanced from 3→7 rejection scenarios (valid accept, changed production, skipped, wrong result, wrong test name, wrong schema, missing file).
- **docs/FACTORY.md**: documentation gate section updated with full false-evidence category list including fixture assembly and missing backend.

## Verification
- `verify-boilerplate.sh` — all checks pass including new grep invariants.
- `test-installed-functional-evidence.sh` — all 7 scenarios pass.
- `ctest` — 83/83 pass (1 skip = backend_smoke, needs GPU).

## Next Task
Task 12 (Final documentation and specification audit) — depends on Tasks 1-11 (all complete). This is the final gate: full §11.2 definition of done, conformance matrix, interaction inventory, clean builds, packaging, independent reviews, documentation, clean tree.