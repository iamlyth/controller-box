# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0004",
    "title": "Remote project gate bypasses Nix when native dependency names are present",
    "status": "open",
    "severity": "medium",
    "reported": "2026-08-14",
    "external": [],
    "contract_change": false,
    "reproduction": "Transfer the exact clean commit to the Debian factory runner and execute scripts/verify-project.sh in a fresh workspace. Because Debian pkg-config finds every dependency name, the verifier skips nix-shell and compiles against ambient Debian packages, failing on incompatible feature defaults and cmocka APIs.",
    "expected": "The project gate always uses the declared Nix environment when nix-shell is available, independent of ambient host package installation, and a clean strict-C11 build declares POSIX interfaces used by shared test support.",
    "actual": "scripts/verify-project.sh enters nix-shell only when dependency discovery fails, so an arbitrary host package set can masquerade as the declared environment. The first failure also exposed tests/dbus_mock.c relying on an undeclared POSIX strdup interface.",
    "acceptance": "A fresh remote gate enters Nix despite installed native dependency names, compiles without implicit declarations or incompatible ambient test APIs, and both the complete local verifier and exact-commit remote runner gate pass.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```