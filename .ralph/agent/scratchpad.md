# Task 8: Flatpak experimental status, documentation defects, clean-install default

## Outcome
Task 8 complete (commit a71af29). All four acceptance criteria met.

## Deliverables

### 1. Flatpak manifest experimental marker (PK-02)
`packaging/org.shadowblip.ControllerBox.yaml`: Added EXPERIMENTAL comment + `Publication marker: false` at top of manifest.

### 2. No Flathub pre-publication advertising (PK-02)
- `README.md`: Replaced `flatpak install flathub org.shadowblip.ControllerBox` with local build instructions; section renamed "Flatpak (experimental)"
- `docs/PACKAGING.md`: Removed "Published on Flathub." assertion; added experimental status paragraph
- `tests/test_flatpak_manifest.py`: Extended to enforce:
  - EXPERIMENTAL comment present in manifest
  - Publication marker is false
  - No `flatpak install flathub org.shadowblip.ControllerBox` in README.md or docs/PACKAGING.md
  - No "Published on Flathub" standalone claim in docs/PACKAGING.md (regex excludes "not yet published")

### 3. test_packaging.sh documented optional step
Replaced silent `SKIP: flatpak-builder not installed` with documented optional gate message explaining that flatpak-builder is optional, with installation reference to docs/PACKAGING.md.

### 4. Clean-install test (PR-04)
- `test_profile_list.c`: `test_clean_install_builtin_default` — sets HOME to temp dir with empty user dirs, calls real `cbx_profile_list_enumerate()`, verifies builtin Default found (is_default, read_only, path=builtin dir), loads it (6 NES bindings), saves a copy, reloads copy (6 bindings match)
- `test_profiles_tab.c`: `test_clean_install_default_copy_uses_shipped` — uses `init_tab_real` (no test dirs, real enumerate path), verifies builtin Default found, creates "Default copy" via `cbx_profiles_tab_create`, verifies copy file exists with 6 NES bindings from shipped default

## Verification
- `ctest -R 'test_profile_list|test_profiles_tab|test_flatpak_manifest|test_packaging'` → 4/4 pass
- Full gate: 90/91 pass (1 pre-existing `test_pi2_ollama_wrapper` failure, needs ollama)
- `verify-boilerplate.sh` — same pre-existing exit 1 (pi2-ollama-wrapper + active plan validation)

## Next task
Task 9 (Backend smoke CI coverage and BUG-0004 resolution) — depends on Task 7 (complete).