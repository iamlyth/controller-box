# Security Audit — Task 6: Resolve orphaned study-report files

## Scope

Task 6 is a documentation-housekeeping task: it relocated orphaned study-report
markdown files (`architecture-study.md`, `subsystem-manager-study.md`,
`subsystem-ui-report.md`) into `.factory/artifacts/` and tracked them in Git,
and reverted incidental production-code edits that had leaked into the tree
(`src/manager/manager.c`, `settings_tab.c/h`, `tests/CMakeLists.txt`).

The change touches only markdown artifacts and reverts prior incidental code
edits. It adds **no** C source, no input parsing, no buffers, no network/DBus
surface, no privilege boundary, no filesystem writes at runtime, no error paths,
and no dependency manifests.

## Verified state

- `git status --porcelain --untracked-files=all` → **0 untracked files** (empty
  output). Acceptance criterion ("repo root contains no untracked files") met.
- The three study artifacts are committed and tracked under `.factory/artifacts/`.
- No stray orphaned files remain: prior strays `docs/subsystem-study-overlay.md`
  and `.factory/artifacts/substudy-identify.md` were removed.
- No credentials/secrets in the moved files (the only `token` match is a
  substring of `csv_token_count` — a CSV helper function name in documentation,
  not a secret).
- Mutable runtime state (`build/`, `.factory-state/`, `.pi/output/`,
  `.ralph/`) is correctly gitignored rather than accidentally version-controlled.

## Findings

**No findings.** No `BLOCKER` or `WARN` items.

Task 6 introduces no attack surface: it is a pure documentation relocation with a
clean working tree. None of the security focus areas (input validation, buffer
safety, privilege boundaries, DBus authentication, filesystem safety, error
handling, dependencies) are touched by this change.

### INFO (non-blocking observations, out of Task-6 scope)

These are pre-existing, already-documented weaknesses in `src/ui/` surfaced by the
relocated study reports. They are scheduled as separate planner work and are not
introduced by Task 6; noted only for situational awareness:

- **INFO — `src/ui/text.c`**: texture leak on cache-full / over-length path in
  `render_to_texture` (slow memory accumulation in a resident service).
- **INFO — `src/ui/widget_list.c`**: unguarded `y / item_h` divide in
  `list_handle_event` (currently safe because no `item_h` setter exists).
- **INFO — `src/ui/widget_button.c`**: `SDLK_a` triggers focus activation without
  checking the `CBX_CONTROLLER_EVENT_WINDOW_ID` marker — a local input-reliability
  nit, not a privilege issue.

None of these affect Task 6 completion and are not actionable here.

**Result:** No security findings for Task 6. Exiting 0.
