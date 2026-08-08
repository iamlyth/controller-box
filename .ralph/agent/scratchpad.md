# Implementation Recovery Handoff

- Active branch: `develop`; tree was clean at handoff.
- Canonical recovery plan: Tasks 1, 3, and 6 are complete; Tasks 2, 4, 5, and 7–12 remain pending.
- Runtime checkpoint: `16f1ec1` adds native typed DBus decoding, Manager SDL controller lifecycle, immutable Default profile, explicit profile Save/Discard, empty-profile sequential entry, overlay visibility/recovery, target reconciliation, and live assignment/profile application.
- Follow-up commits: `0c2b22b` fixes renderer readback; `27a1b76` confirms created targets; `8fdd4ac` adds a private native sd-bus fixture; `e8fe463` drives Manager dispatch with an SDL virtual controller.
- Factory backpressure: `3f6d500`, `f302d6f`, and `49c4d49` require commit-bound, zero-skip installed-functional evidence. Boilerplate parity is at `37fff1a` and `042d540`.
- Verification: clean build succeeds; full CTest passes 81/81 with only the optional accelerated backend smoke skipped; `verify-boilerplate.sh` passes; `test_native_dbus` and SDL virtual-controller transport pass.
- Important blocker by design: `verify-project.sh` now fails until a non-skippable `test_installed_functional` exists. Do not weaken this gate or synthesize evidence.
- Next task: Task 2. Finish visible degraded-state distinctions and production owner-loss/reacquisition tests for both Manager and overlay, then continue exact Remove/Type confirmation and topology rollback.
- Installed acceptance must ultimately use the installed binary, native private DBus service, real observable target/routing semantics, profile save/reload, overlay mapping, and zero skips. This jailed host currently has no `/dev/uinput`; record a genuine environment blocker rather than substituting keyboard proxies if kernel-backed input is required.
