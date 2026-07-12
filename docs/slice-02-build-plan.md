# Slice 02 — Build Plan (Prism Core: Platform Port)

**For:** Claude Code (driven task-by-task, human review per task)
**Companion docs:** `CLAUDE.md` (rules), `docs/prism-state-vector-spec.md` (producer contract),
`docs/prism-pgae-consumption-spec.md` (consumer contract)

---

## Goal

Port the **validated** Slice 01 engine to the production platform:

```
raw events ─► C++ PCE (ported heuristic) ─► PSV ─► C++ PGAE ─► audio out
                        │
                        └─ identical behavior to the probe (golden-trace parity)
```

Deliverable: a portable C++ core behind a C ABI, proven on desktop and on physical
Android + iOS devices via a minimal Flutter FFI smoke app. **No new features** — parity
first, expansion in the next slice. Timebox: ~4–6 weeks with per-task review gates.

## Gate 0 — before the first commit

- Signed IP Assignment + Confidentiality agreements from every contributor to this repo.
  Production core code is exactly the code those agreements exist for. No commits before
  signatures.
- Repo is private.

## Inputs to this slice

- The probe repo: heuristic constants, mapping curves, and session logs.
- Probe session logs exported as golden traces into `tests/golden/` (input trace + expected
  PSV trajectory per session).
- Probe stems (or the same placeholders) + a `scenes.json` manifest.

## Performance budgets (measured, not assumed)

- PCE inference: trivial for the heuristic; must be < 1 ms per tick regardless.
- Audio render: < 10% of one core on a mid-range 2022 Android device.
- Resident memory: < 50 MB including preloaded stems.
- Battery: < 3%/hour active, estimated from a 30-minute on-device smoke run.

---

## Tasks (in order — stop at each acceptance criteria)

### Task 0 — Scaffold
CMake project with presets; module layout `psv/ pce/ pgae/ bindings/ harness/ tests/`;
GoogleTest wired; miniaudio vendored; clang-format config; CI workflow (build + ctest) if
the remote is GitHub. Fill the Commands section of `CLAUDE.md`.
**Done when:** `ctest` runs green on an empty test, and the harness plays one static stem
loop on desktop through miniaudio.

### Task 1 — PSV module
Implement PSV types exactly per spec §4–5: four dimensions as `{value, confidence}`,
metadata, neutral/cold-start vector (§6), JSON serialization + schema-shape validation.
**Done when:** unit tests cover round-trip serialization, bounds, and cold start; output
validates against the spec's JSON Schema.

### Task 2 — PCE port (the heuristic, verbatim)
Event ingress API (`report_task_deadline`, `report_app_switch`, `report_idle`, …); rolling
in-memory behavioral window; deadline decay; circadian phase if the probe had it; confidence
from availability/freshness; fusion constants copied **verbatim** from the probe; emit at
the spec cadence + on significant change. PCE math in `double`.
**Done when:** golden-trace parity — every trace in `tests/golden/` reproduces the probe's
PSV trajectory within 1e-6 per dimension per tick.

### Task 3 — PGAE port (minimum-viable scope)
Per consumption spec §11: stem player with sample-accurate coprime loops from
`scenes.json`; one scene (Deep Work) + a second if cheap; confidence weighting (§3); delta
mapping to gain / LP cutoff / density (§5); smoothing + slew caps (§7); equal-power
crossfades at loop boundaries (§6.2); master hard limiter (§8). Stems preloaded/decoded at
scene load.
**Done when:** offline render tests pass (ceiling held, no parameter steps, equal-power
property) and desktop playback is audibly at parity with the probe.

### Task 4 — Real-time hardening
Three-thread model (ingress / inference / audio); atomic double-buffer PSV handoff; verify
the render path never allocates or locks (instrumented allocator + assertions in debug);
record CPU% and resident memory on desktop.
**Done when:** instrumentation shows zero allocations/locks in the render path under a
10-minute run; numbers recorded in the repo.

### Task 5 — C ABI
`include/prism/prism_core.h`: opaque handle, lifecycle, event ingress, PSV read-out, pull
render callback + optional built-in device, error codes. Semver'd, documented per function.
Rebuild the harness **on top of the C ABI only**.
**Done when:** the harness links against the public header alone and behaves identically;
ABI doc reviewed.

### Task 6 — Flutter FFI smoke (Android first)
`bindings/dart/` via ffigen against the public header; minimal Flutter app in
`examples/flutter_smoke/`: start/stop, live PSV readout, audio playing. Android first for
iteration speed.
**Done when:** runs on a physical Android device; 30-minute run with CPU/battery snapshot
recorded against budgets.

### Task 7 — iOS target
xcframework build of the core; same smoke app on a physical iPhone.
**Done when:** runs on device; numbers recorded.

### Task 8 — Parity dogfood (not a coding task)
Daily sessions through the desktop harness / smoke app for ~1 week. Confirm the feel
survived the port. Go → next slice (product integration: Flutter app proper, remaining v1
adapters, Venues host).

---

## Out of scope (do not build)

- Any ML model.
- New inputs (weather, HR/HRV) — next slice, after parity.
- Venues host, Automotive, Capacitor plugin (the C ABI keeps these open; do not build them).
- Any networking, telemetry, or cloud integration.
- Product UI beyond the smoke app.
- "Improving" the heuristic or the mapping — parity is the point of this slice.
