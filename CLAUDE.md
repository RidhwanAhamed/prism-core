# CLAUDE.md — Prism Core (Platform Engine)

> Read this fully before writing code. This is the constitution for this repo.
> Ambiguity in the specs is not yours to resolve by guessing — see "When to stop and ask."

## What this repo is

This is the **production Prism Engine core** — the PCE (state inference) and PGAE (generative
audio) as a portable C++ library behind a stable C ABI. It is **not** a probe. The Slice 01
probe already validated the thesis (adaptive audio driven by an inferred state vector feels
right); this repo is the platform that ships inside Aqademiq (Flutter via `dart:ffi`), Prism
Venues (embedded Linux host), and later Automotive.

Code here is long-lived. Correctness, tests, ABI stability, and real-time safety are the
priorities — not speed of scaffolding.

## The firewall — now structural

The state-inference layer emits **only** a Prism State Vector (PSV). It never emits audio
parameters. In this repo that rule is enforced by module structure, not convention:

- `psv/`  — the contract: PSV types, validation, serialization. Depends on nothing.
- `pce/`  — state inference. Depends on `psv` only.
- `pgae/` — audio engine. Depends on `psv` only. **Must never include or link `pce`.**

CMake target dependencies must encode this. If a change requires `pgae` to see anything from
`pce`, stop — the design is wrong.

## Where the logic lives

**All state-inference math lives in `pce/`, in portable C++.** That includes the deadline
proximity decay, behavioral windowing (app-switch frequency, idle gaps), circadian phase,
confidence computation, and fusion. Platform shells and bindings only **capture raw events
and forward them** through the ingress API — they contain no inference logic.

Rationale: identical behavior on every platform, one artifact embodying the method (this is
also the IP posture), thin shells.

## Stack (locked — ask before adding anything)

- **C++17**, CMake ≥ 3.24 with presets.
- **miniaudio** (vendored single-file) for audio device I/O and WAV/FLAC decode on all
  platforms — desktop, Android (AAudio), iOS (CoreAudio), embedded Linux. No JUCE: licensing
  weight and binary size we don't need.
- **GoogleTest** via FetchContent for unit tests.
- **PCE math in `double`** (matches the TS reference implementation for parity), audio path
  in `float`.
- No other third-party dependencies without asking. No networking library exists in this
  repo by design.

## Real-time rules (unbreakable)

1. The audio render path **never** allocates, locks, logs, blocks, or performs I/O.
   Everything it touches is pre-allocated at scene load.
2. The PSV crosses from the inference thread to the audio thread via a **pre-allocated
   atomic double buffer** (single writer, single reader). No mutex on that path.
3. **Every** gain or routing change is ramped. Nothing steps. Crossfades are equal-power.
4. Stems are **preloaded and decoded at scene load** (loops are short by design — coprime
   loop lengths, tens of seconds — so the memory budget holds). No disk streaming in v1.
5. Loop-boundary scheduling is **sample-accurate**.
6. The master limiter is the last stage and cannot be bypassed by anything upstream.

## Parity with the probe (the acceptance bar)

The probe is the **reference implementation**. Port its heuristic constants and mapping
curves **verbatim** — they encode the validated feel. Do not "improve" them in this repo.

- `tests/golden/` holds recorded input traces and PSV trajectories from probe session logs.
- **PCE parity test:** feeding a golden input trace must reproduce the probe's PSV
  trajectory within 1e-6 per dimension per tick. Divergence is a bug or a documented,
  reviewed decision — never silent.
- **PGAE render tests (offline):** render to a buffer and assert — limiter ceiling never
  exceeded, control parameters never step, crossfades hold the equal-power property.

## Contracts

- `docs/prism-state-vector-spec.md` — the producer contract. Emitted PSVs must validate
  against its schema (§4–5); honor cold-start (§6) and confidence semantics (§4.2).
- `docs/prism-pgae-consumption-spec.md` — the consumer contract. Slice 02 implements its
  §11 minimum-viable scope against the §5 mapping and §8 rails.
- `docs/slice-02-build-plan.md` — the work, task-by-task with acceptance criteria.

## Privacy invariants

1. **No network calls anywhere in the core.** Not for telemetry, not for assets, not ever.
2. Raw input events are processed **in memory** and never written to disk by the core.
3. The optional session log (for tuning) contains **derived values only** — PSV trajectory,
   scene, parameters — and is written only when explicitly enabled by the host.

## The C ABI is the product surface

- `include/prism/prism_core.h` is the **only** public header. Opaque handle,
  create/destroy, start/stop, event ingress (`prism_report_*`), PSV read-out for UI, and a
  pull-model render callback plus an optional built-in device.
- Everything else is internal. The ABI is semver'd; breaking it is a major version.
- The desktop harness must consume **only the C ABI** — it keeps the boundary honest and
  doubles as the binding reference for `dart:ffi` (ffigen), a future Capacitor plugin, or
  an OEM SDK.

## Conventions

- Small, single-purpose commits. One task per review. A human (Aswath) reviews every task.
- Tests are part of "done" for every task — no exceptions.
- Comment the *why* on every heuristic constant: they are dogfooding findings, not magic
  numbers.
- This repo is **private**. Nothing here is published or disclosed until the provisional
  patent is filed.

## When to stop and ask

- Spec ambiguity or an Open Item in either contract — flag it and wait.
- Any new dependency.
- Anything that would touch the firewall, the ABI shape, or the real-time rules.
- Anything requiring network access.

## Definition of done (Slice 02)

The core, consumed only through its C ABI, reproduces the probe's behavior on desktop
(golden-trace parity + audible parity), runs on one physical Android device and one physical
iPhone via a minimal Flutter smoke app, and has its performance numbers (CPU, memory,
battery estimate) measured and recorded against the budgets in the build plan.

## Commands

- Configure: `cmake --preset debug` (also `release`, `asan`)
- Build: `cmake --build --preset debug`
- Test: `ctest --preset debug` (includes the `firewall_includes` check)
- Harness: `./build/debug/harness/prism_harness assets/stems/bed.wav` from the repo
  root (`--seconds N` to auto-exit; omit to play until Enter)
- Format: `find psv pce pgae harness tests/unit -name "*.cpp" -o -name "*.h" | xargs clang-format -i`
