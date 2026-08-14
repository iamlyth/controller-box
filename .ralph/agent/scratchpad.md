# Planning Scratchpad — Controller-Box implementation plan

## State: planning complete
- Spec committed (3a10f6b), not dirty. Skeleton metadata preserved byte-for-byte.
- Codebase mature: 89/89 ctest pass (1 env-gated skip). Three planner-scout
  subagents mapped §§4-11 conformance; most requirements verified.
- 9 implementation tasks + 1 final audit written to implementation-plan.md.
- `./scripts/final-gate.sh --planning` accepted. validate-implementation-plan
  planning passed. security/docs review: no blocking issues (path containment
  already verified; Flathub defect tasked; boilerplate gate covered).

## Gaps tasked
- Task 1: overlay latency timing (§4.9/§11) — no timing test existed.
- Task 2: controllers topology reconcile + auto-Unassign (§5.2).
- Task 3: editor unsaved-close prompt, sequential prod capture, clone, determinism.
- Task 4: settings icon override + interaction/visual coverage (§5.5/§5.6).
- Task 5: inventory driven traversal + hover/resize/decorative (§5.7).
- Task 6: controller-transport acceptance + manager-UI recovery (§5.7).
- Task 7: overlay dynamic columns hotplug + visual skip hardening (§4.7/§4.10).
- Task 8: Flatpak experimental + Flathub doc defect + clean-install default (§9.1).
- Task 9: backend smoke CI coverage + BUG-0004 verify/close (§11.1.6/bug).
- Task 10: final audit (§11.2), depends on 1-9.

## Environment honesty
- Declared runner has only remote-project-gate + systemd-user. gpu-compositor,
  physical-controller, target-consumer NOT declared. GPU backend smoke +
  Pi-4 latency + target-hardware visual = human-release-gated (VS-03); plan
  produces strongest deterministic evidence and documents the human remainder.

## Next action
Emit the completion token.
