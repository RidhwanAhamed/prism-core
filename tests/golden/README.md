# Golden traces

Exported from the probe (`prism-engine-probe`, dogfood sessions of 2026-07-12).

## Status: INCOMPLETE — PSV trajectories only

Gate 0 requires **input trace + expected PSV trajectory** per session. These files
contain only the PSV half. The probe's session logger records `session_start`, `psv`,
and `checkpoint` events — it never persisted raw input events (by design, mirroring
the core's privacy invariant), so the input half of each trace does not exist yet.

Consequence: the Task 2 parity test (replay input trace → reproduce PSV trajectory
within 1e-6 per dimension per tick) **cannot run against these files alone**.

To complete the fixtures, the probe needs a deliberate fixture-capture mode that logs
the same events the PCE ingress API accepts (`report_task_deadline`,
`report_app_switch`, `report_idle`, …) alongside the PSV output, followed by a re-run
dogfood session. That capture mode is a conscious, host-enabled exception to the
"raw inputs never hit disk" rule, used only to generate test fixtures.

## Files

- `session-2026-07-12T14-13-21-161Z.jsonl` — 46 lines: 1 session_start, 40 psv,
  5 checkpoint. The main session.
- `session-2026-07-12T14-03-25-352Z.jsonl` — 13 lines. Short false-start session,
  kept for reference.
