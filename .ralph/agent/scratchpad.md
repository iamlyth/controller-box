# Handoff: HEAD 2703586 green on every software gate; completion blocked only on external hardware/capability/signer facts

Re-verified directly; no code changed, tree clean (only scratchpad dirty).

## Outcome this iteration
- Plan `active` + fresh (spec=3a10f6b7d04a, blob=58f5d3cb72bc, `check-plan-freshness.sh` exit 0).
- HEAD 2703586 unchanged; branch `develop`, tree clean. `ralph tools task ready` → empty; no runtime tasks.
- `check-factory-runner-evidence.py` → "aggregate Git/environment binding is stale"; `check-capability-evidence.py` → "not bound to HEAD 27035861f274" — exactly FACT-007 (runner receipt bound to c45336a, not the current implementation commit; dev-runner-vm not re-runnable here), NOT a software regression.
- Blocked-facts ledger: 6 open (FACT-002..007) / 2 resolved (FACT-001, FACT-008). All open facts are external hardware/capability/signer blockers: real InputPlumber system bus (FACT-002/003), target-consumer/Pi 4 runtime (FACT-004/006), gpu-compositor (FACT-005), unsigned/unbound runner receipt (FACT-007).

No ready task; no software-addressable remediation exists.

## Next
Final audit (Task 4) is the sole gate and stays blocked on external hardware/capability/signer facts; requires a human to provision the real InputPlumber system bus / target consumer / Pi runtime / gpu-compositor / signer and re-run the runners on the current HEAD, or an explicit spec-scoped human deferral per §11.2.6. Do not emit the completion token; ledger stays open; plan stays `active`.

## Iteration: BUG-0018 diagram geometry (task-1787407706-bacd)

Chose the software-addressable P1 task over BUG-0015 (externally hardware-blocked: real InputPlumber system-bus/capability FACT-002/003). Started task-1787407706-bacd. Handed off to Factory Worker via factory.implement to:
1. Append remediation task to plan (uniquely numbered), keep plan active.
2. Diagnose source aspect/raster scaling + marker coordinate transforms in src/manager/profile_diagram.c.
3. Repair installed exact-commit capture driver (real window title/class polling, manager/overlay modes, isolated DISPLAY/HOME/XDG, bounded cleanup).
4. Add semantic installed-window tests that fail for stretch/pixelation/misalignment; machine vision findings-only; no golden regen; no human acceptance claim.
Next iteration: after BUG-0018, evaluate BUG-0015 software-addressable portion vs external block.

## Iteration: BUG-0015 virtual target creation/topology (task-1787407706-cd8d)

Ready task is BUG-0015 (cd8d), marked in_progress. BUG-0018 (bacd) is in_progress from a prior iteration whose factory.implement event was never published; it is NOT ready this cycle. BUG-0015's runtime-task description frames it as software-addressable: diagnose/fix the production CreateTargetDevice/SetTargetDevices topology reconciliation (src/dbus/ip_manager.c, ip_composite.c, src/manager/controllers_tab.c), add production-path semantic coverage through the DBus backend abstraction, and keep external system-bus evidence (FACT-002/003) honestly open. Plan Task 6 (full four-target routing) stays externally blocked; this is distinct software work. Appended remediation numbering continues past Task 13 (next unique = Task 14).

## Iteration: dispatch BUG-0015 (cd8d) via factory.implement (recovery)

Recovery: previous iteration decided BUG-0015 (cd8d) is the ready software-addressable task (marked in_progress) but never published factory.implement. HEAD 2703586, plan active/fresh. Both BUG-0015 (cd8d) and BUG-0018 (bacd) are P1 in_progress; BUG-0018's event also was never published, but cd80 is the most recent decision and its runtime-task description is explicitly software-addressable. Dispatching cd8d now:
1. Append uniquely-numbered remediation task (next = Task 14) to plan, keep active.
2. Diagnose/fix production CreateTargetDevice/SetTargetDevices topology reconciliation (src/dbus/ip_manager.c, ip_composite.c, src/manager/controllers_tab.c).
3. Add production-path semantic coverage through the DBus backend abstraction.
4. Keep external system-bus evidence (FACT-002/003) honestly open; Task 6 (full four-target routing) stays externally blocked.
Next: after cd8d, evaluate BUG-0018 (bacd) software work.
