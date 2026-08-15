# Task 7: Overlay dynamic columns hotplug + visual skip hardening

## Outcome
Task 7 complete (commit fb62cc6). All acceptance criteria met.

## Deliverables

### 1. Production-dispatch hotplug test (OV-14)
`test_hotplug_target_add_remove_through_dispatch` in `test_overlay_interaction.c`:
- Full signal injection path: `inject_signal(IP_IFACE_OBJECT_MANAGER, "InterfacesAdded/Removed")` → `hotplug_signal_cb` subscription callback → `ip_hotplug_handle_added/removed` (with sender verification + path validation) → `model_changed` → `cbx_overlay_service_step` Phase 4 → `cbx_overlay_reconcile_hotplug`
- Phase 1 (ADD): target_count 4→5, grid col_count 5→6 (rebuild via `cbx_dynamic_columns_rebuild`)
- Phase 2 (REMOVE): target_count 5→4, grid col_count 6→5, row 0 clamped from col 5 to col 0 (Unassigned) via `cbx_dynamic_columns_clamp_positions`
- `ip_hotplug_init` + `ip_hotplug_subscribe` in test; `SDL_INIT_TIMER` added to interaction fixture for poll re-arm during reconcile

### 2. Visual skip hardening (OV-21, OV-22, §11.2.5)
`test_overlay_visual.c`: silent `skip()` replaced with `fail_msg()` in:
- `test_model_profile_text`: documents DejaVuSans.ttf font requirement
- `test_virtual_device_icons`: documents controller-icons.yaml + SVG asset requirement
- In declared nix-shell environment, both tests pass (font/icons available); outside it, they fail with clear message (no unexplained skip)

## Key discovery
`ip_dbus_mock_reset` clears ALL subscriptions (`sub_count = 0`), not just expectations. After calling `expect_reconcile` (which resets the mock), hotplug signal subscriptions are gone. Fix: `expect_reconcile` re-subscribes via `ip_hotplug_subscribe(hp)` after reset.

## Verification
- `ctest -R 'test_overlay_interaction|test_overlay_integration|test_overlay_visual|test_overlay_reconcile'` → 4/4 pass
- Full gate: 90/91 pass (1 pre-existing `test_pi2_ollama_wrapper` failure, needs ollama)

## Next task
Task 8 (Flatpak experimental status, documentation defects, clean-install default) — no deps.