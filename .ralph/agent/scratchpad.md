# Maintenance BUG-0001 — Scratchpad

## Iteration 1: Task 1 complete

- Transitioned BUG-0001 from `planned` to `in_progress` via `bug-ledger.py`.
- Created runtime tasks for all 4 maintenance plan tasks.
- Implemented Task 1: Add runtime font discovery to config_paths.
  - Added `cbx_font_path()` and `cbx_font_dir()` to `src/config/config_paths.{c,h}`.
  - `cbx_font_path()` searches XDG_DATA_HOME, $HOME/.local/share/fonts, $HOME/.fonts,
    and system font dirs (including NixOS paths) for DejaVuSans.ttf. Returns const char*
    to static buffer or NULL.
  - Added `FONT_DIR` to `config.h.in` and `CBX_FONT_DIR` to `CMakeLists.txt`.
  - Added install rule for `data/fonts/` with `.gitkeep`.
  - Added `test_font_path.c` unit test (5 test cases). All pass.
  - Build succeeds in Debug mode with no warnings.
  - Full suite: 64/64 tests pass (was 63, +1 new test).

### Format-truncation lesson (reinforces existing memory)
GCC Debug `-Werror=format-truncation` flags `snprintf` into a buffer when the source
variable has the same or larger declared size. Fix: make destination buffers
progressively larger than source buffers by at least the suffix length. Used
PATH_MAX+16, PATH_MAX+32, PATH_MAX+64, PATH_MAX+128 in the chain.

### Next iteration
- Task 2: Wire font discovery into manager launch (main.c, manager.c).
  Depends on Task 1 (now complete). Runtime task task-1786062044-7006 is open.