# Stopped: two production acceptance failures invalidate completion (BUG-0014, BUG-0015)

## State

- Campaign stopped gracefully by lifecycle operator (SIGTERM, status 143) at
  round 2/5 implementation phase, `ralph-campaign.json` preserved
  (active, phase implementation, round 2, verifier 8de1bdd6), supervision
  preserved (completion 4/8, no-progress 4/8, cycle 83b26cbf), tree clean at
  23299b2, stale `.ralph/loop.lock` from PID 40395 requires
  `ralph-recover.sh --mode implementation --prepare-only` before any resume.
- Do NOT resume without human approval. Completion premise is invalidated.

## Blocking defects (human-observed, recorded in the bug ledger)

- BUG-0014: `./build-check/controller-box --manager` shows NO controller
  diagram on the real launch path. Existing goldens/visual tests miss it.
  Required: renderer/asset-path diagnosis and fix + a production-window /
  installed-path semantic test proving recognizable diagram content.
- BUG-0015: manager reports "Topology incomplete: 0 of 4 virtual controllers
  active"; no virtual controller works. `inputplumber-system-dbus` is NOT
  declared in `.factory/environment.toml`, so private/native-signature sd-bus
  tests are not real system-bus evidence. Required: real InputPlumber
  system-bus acceptance (4 objects active/usable through production dispatch)
  or an explicit blocking finding with no `verified` claims.

## Plan state

- `.factory/artifacts/implementation-plan.md` front matter returned to
  `status: active` with a Blocking findings section; rows ARCH-04, SYS-06,
  DBUS-02, DBUS-05, OVL-10, MGR-02, MGR-07, MGR-08, DOD-01, DOD-09
  reclassified verified -> partial; Task 4 set `blocked`. The plan is
  intentionally not `complete`-validatable until the fixes land with real
  acceptance evidence.
- Prior fix committed: 8e0beff (BUG-0013 attestation env isolation, ported
  a8c74b0 to /tmp/unattended-ralph-fix); 2d1fb2d closes BUG-0013.

## Next (Ralph, after human resume approval)

- Diagnose and fix BUG-0014 (diagram rendering) and BUG-0015 (real system-bus
  topology) as product work; add the semantic production-window tests;
  reclassify the ten partial rows only with real acceptance evidence; never
  mark a row verified with private-mock, /dev/uinput-presence, installed
  smoke, inferred-request, or golden-only evidence. Do not emit the
  completion token while the ledger is non-empty or the matrix has partial
  rows.
