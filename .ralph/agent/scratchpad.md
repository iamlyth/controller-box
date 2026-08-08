# Task 4 Complete — Security-hardened profile save in production path

## What was done

### Source changes

1. **`src/manager/profile_save.c` — TOCTOU fix**: Modified `verify_path_within_dir()` to return the canonicalized target path via an out-parameter (`canonical_out`, `canonical_len`). `cbx_profile_save_to_dir()` now passes the canonicalized path to `cbx_profile_save()` instead of the original uncanonicalized path — eliminating the TOCTOU race between the `realpath()` check and the file write.

2. **`src/manager/profiles_tab.c` — Direct-save bypass fix**: Replaced the direct `cbx_profile_save()` call in `cbx_profiles_tab_create()` with `cbx_profile_save_to_dir()`, which enforces:
   - Filename validation (`^[a-zA-Z0-9_-]+$`)
   - Profile structure validation (version=1, kind="DeviceProfile")
   - NES minimum binding validation (A, B, Up, Down, Left, Right)
   - Path canonicalization and containment check (TOCTOU-safe)
   Added `#include "manager/profile_save.h"`. Removed unused `build_profile_path()` function. Added error status label display for missing NES bindings.

3. **Verification**: `grep -rn 'cbx_profile_save(' src/ --include='*.c' | grep -v 'profile_save.c' | grep -v 'config_profile.c'` returns zero matches — confirming all production save paths go through `cbx_profile_save_to_dir`.

### Test changes

1. **`tests/test_profiles_tab.c`**: Added `write_nes_profile_yaml()` helper that writes profiles with all 6 NES button bindings. Updated fixture default profile to use NES bindings. Updated tests that created empty profiles for other purposes (delete tests, name input confirm, production dispatch) to use `CREATE_DEFAULT_COPY`. `test_create_empty` now expects `-EINVAL`. Added `test_create_reject_missing_nes` to verify NES rejection when cloning a profile without NES bindings. Updated mapping count expectations from 3 to 6. Changed `test_name_input_via_dispatch` to select Default Copy (index 0) instead of Empty (index 1).

2. **`tests/test_profile_save.c`**: Added `test_save_through_symlink_uses_canonical` — saves through a symlinked profiles directory and verifies the file is written to the canonical (resolved) path, confirming the TOCTOU fix.

3. **`tests/test_manager_integration.c`**: Added `write_nes_default()` helper. Added `profiles_dir` field to fixture struct to fix a dangling pointer issue (the old `profiles_dir` local variable was stored as `test_user_dir` in the profiles tab, but became dangling after setup returned — `realpath()` on the dangling pointer caused `-ENOENT`). Changed `CBX_PT_CREATE_EMPTY` to `CBX_PT_CREATE_DEFAULT_COPY` in 3 tests that create profiles.

## Test results
74/74 pass (1 skip: backend_smoke). No regressions.

## Commit
`8e767a1` on `develop`

## Next task
Task 5: Wire profile editor, create-to-editor flow, and editor UI entry points into manager production path. Dependencies: Task 3, Task 4 (both complete). Ready to start.