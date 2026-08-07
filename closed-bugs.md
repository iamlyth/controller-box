# Closed Bugs

Completed defects and their verification evidence.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0001",
    "title": "Manager launches with an effectively blank interface",
    "status": "closed",
    "severity": "critical",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "On NixOS, build and install to a local CMake prefix, then run the installed controller-box binary with --manager. The window opens, but only dark backgrounds, tab rectangles, and the window decoration are visible.",
    "expected": "The manager visibly renders the Controllers, Profiles, and Settings tab labels plus the controls, lists, status text, and other content required to operate the controller-only UI.",
    "actual": "The manager window renders structural rectangles but no application text or usable controls. Startup also prints a non-fatal OpenGL alpha-blending verification warning; the unrelated GTK theme warning originates from the desktop theme.",
    "acceptance": "A locally installed NixOS build visibly renders manager labels and controls using a reliably available font; missing font resources produce an actionable failure rather than a blank UI; an automated regression exercises real font initialization/text rendering; the full project verifier passes.",
    "resolution": "Fixed by adding runtime font discovery (cbx_font_path() in config_paths.c) that searches XDG_DATA_HOME, ~/.local/share/fonts, ~/.fonts, and system font directories including NixOS paths for DejaVuSans.ttf. main.c now calls cbx_font_path() instead of passing NULL to cbx_manager_init(). When no font is found, an actionable error is printed to stderr and the manager exits non-zero. manager.c now checks cbx_text_load_font() return value and returns an error code with a diagnostic message naming the path and error code when font loading fails, instead of silently continuing with font_id=-1. Added FONT_DIR to config.h.in and CMake install rule for data/fonts/.",
    "verification": "Full project verifier (scripts/verify-project.sh) passes: clean Debug build with 0 warnings, 65/65 ctest tests pass (including new test_font_init with 4 cases: font init+render, invalid path fails, NULL path, empty path), packaging integration checks pass. test_font_path unit test verifies cbx_font_path() returns readable .ttf or NULL safely. test_font_init regression test exercises real font init (font_id >= 0) and text rendering (non-NULL SDL_Texture*), and asserts invalid font path returns non-zero. docs/SPEC.md unchanged (git diff --exit-code = 0). spec_blob matches HEAD:docs/SPEC.md. No misleading Non-fatal comments in manager init path. bug-ledger validate reports valid.",
    "closed": "2026-08-06"
  }
]
```
