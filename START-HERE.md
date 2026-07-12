# Start Here — Prism Core (Platform Engine)

This repo is the **production Prism Engine core**: PCE + PGAE as a portable C++ library
behind a stable C ABI, built with **Claude Code**. The Slice 01 probe validated the thesis;
this slice ports the validated engine to the platform stack with **parity** as the
acceptance bar.

## Files

- `CLAUDE.md` — the constitution. Claude Code reads this automatically every session:
  the structural firewall, real-time rules, stack, parity requirements, privacy invariants.
- `docs/prism-state-vector-spec.md` — producer contract (what the PCE emits).
- `docs/prism-pgae-consumption-spec.md` — consumer contract (how the PGAE reads it).
- `docs/slice-02-build-plan.md` — the work: Gate 0, budgets, Tasks 0–8 with acceptance
  criteria.

## Gate 0 — do this before any code exists

1. Signed IP Assignment + Confidentiality agreements from everyone contributing here.
2. Private repo. Nothing public until the provisional patent is filed.
3. Export the probe's session logs as golden traces (input trace + expected PSV trajectory)
   — they land in `tests/golden/` and become the parity tests.

## How to run the handoff

1. `git init` this folder as a **private** repo.
2. Open it in Claude Code.
3. Paste the kickoff prompt below as your first message.
4. Drive it **one task at a time**: task → review the diff (Aswath) → commit → next.
   Never ask for the whole slice in one shot.

Useful mid-session: `/memory` shows which instruction files are loaded; `#` adds a durable
rule on the fly; `/clear` resets context between unrelated tasks.

## Kickoff prompt (paste as your first message to Claude Code)

```
You're working in the Prism Core repo — the production C++ engine. Before doing anything,
read CLAUDE.md (repo root), then docs/prism-state-vector-spec.md,
docs/prism-pgae-consumption-spec.md, and docs/slice-02-build-plan.md in full — these are
the rules, the two contracts, and the work.

Then, BEFORE writing any code, reply with:
  (a) a one-paragraph restatement of what this repo is (and how it differs from the probe),
  (b) the two rule sets you must never break (the structural firewall; the real-time audio
      rules), each in one sentence,
  (c) your concrete plan for Task 0: exact directory layout, CMake preset structure, and
      the full dependency list (which should be very short).
Stop there and wait for my confirmation.

Working mode for the whole build:
- One task at a time, in build-plan order. Stop when a task's acceptance criteria pass and
  wait for my review before starting the next.
- Small, single-purpose commits. Tests are part of done for every task.
- If a spec is ambiguous or you hit an Open Item, flag it and wait. Do not invent a
  resolution.

Non-negotiables (also in CLAUDE.md):
- pgae never includes or links pce. The PSV is the only thing that crosses. CMake must
  enforce this.
- The audio render path never allocates, locks, logs, or does I/O. All parameter changes
  are ramped. PSV handoff is a pre-allocated atomic double buffer.
- Port the probe's heuristic constants verbatim — golden-trace parity within 1e-6 is the
  acceptance bar. Do not improve the heuristic in this slice.
- Fully local: no networking exists in this repo. Raw input events are never persisted.
- C++17, CMake, miniaudio, GoogleTest. Nothing else without asking.

Start by reading the files and giving me your Task 0 plan.
```
