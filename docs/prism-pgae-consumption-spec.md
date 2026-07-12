# Prism Generative Audio Engine — Consumption Specification

**Document ID:** PRISM-SPEC-PGAE-001
**Version:** 1.0.0 — *Draft for review*
**Status:** Draft — pending sign-off (engineering)
**Owner:** Ridhwan (architecture) · Mohammed Aswath (implementation)
**Date:** 16 June 2026
**Audience:** Prism engineering
**Companion specs:** `PRISM-SPEC-PSV-001` (state vector / producer contract); Stem Production Specification (musical material / source of truth for content)

---

## 1. Purpose & Scope

Defines how the Prism Generative Audio Engine (PGAE) consumes a Prism State Vector (PSV) and produces continuous, adaptive audio — entirely on-device.

This document governs the **mapping and runtime behaviour**: how PSV values become audio control parameters, how transitions are scheduled, and what guardrails bound the output. It does **not** define the musical material — stem composition, instrumentation, keys, and loop lengths are owned by the **Stem Production Specification**, which this document treats as the source of truth for content.

In scope: the read contract, the processing pipeline, the PSV→parameter mapping, transition and smoothing behaviour, master-stage guardrails, lifecycle, and the reduced subset required for the Slice 01 probe.

Out of scope: PCE internals, authoring/DSP of the stems themselves, and any safety-critical signalling (automotive — handled out of band; see PSV spec §7.4).

---

## 2. Architectural position (the firewall, from the consumer side)

```
PCE ──► [ PSV ] ──► PGAE ──► audio out
```

PGAE is the **only** component that turns a PSV into sound. It receives **only** a PSV (per `PRISM-SPEC-PSV-001`) and never reads PCE inputs, models, or internal state, and never calls back into the PCE. This is the same firewall the PSV spec defines from the producer side, and it is equally inviolable here: all audio knowledge lives below this line, all state inference lives above it.

---

## 3. The read contract

PGAE consumes the PSV exactly as the PSV spec defines. The operative obligations (see PSV spec §8 for the authoritative list):

- **Respect confidence.** Scale every dimension's influence by its confidence:
  `effective = 0.5 + (value − 0.5) × confidence`. At confidence 0 a dimension is inert and the engine behaves as if it were neutral (0.5).
- **Modulate on deltas, not absolutes.** Drive parameter *changes* from changes in the (confidence-weighted) vector, so context shifts ease in rather than snapping.
- **`mode_hint` selects the scene; the continuous vector shapes it.** Discrete choices (which stem set / musical scene) follow `mode_hint`; fine modulation follows the continuous dimensions. `mode_hint` may be `null` — never require it.
- **Tolerate jumps; PGAE owns smoothing.** The PCE makes no rate-of-change guarantee. All temporal smoothing is the engine's responsibility (§7).
- **Guardrails always win.** Nothing derived from the PSV may push output past the master-stage rails (§8).
- **Cold start / staleness.** On the neutral vector (PSV §6) render a calm default scene. On staleness (PSV §7.3) hold, decay effective confidence toward 0, and fall back through `mode_hint` to the neutral default.

---

## 4. Pipeline

```
PSV ─► [1] Scene select ─► [2] Param map ─► [3] Smooth/slew ─► [4] DSP graph ─► [5] Master ─► out
            (mode_hint)      (continuous     (audio-domain      (gain, filter,    (LUFS norm,
                              dims → params)  easing)            reverb, width,    limiter,
                                                                 density)          band rails)
        [6] Transition scheduler coordinates scene changes at loop boundaries (crosscuts 1 & 4)
```

1. **Scene selection** — `mode_hint` → musical scene (stem set + key, per the Stem spec). See §6.
2. **Parameter mapping** — confidence-weighted continuous dimensions → audio control parameters. See §5.
3. **Smoothing / slew** — easing and rate limits applied in the audio domain. See §7.
4. **DSP graph** — applies parameters to the active stems: per-stem gain, filtering, reverb send, stereo width, layer density.
5. **Master stage** — loudness normalisation, hard limiter, prohibited-band enforcement, click/pop prevention. See §8.
6. **Transition scheduler** — schedules scene/stem changes at coprime loop boundaries with crossfades. See §6.2.

---

## 5. PSV → audio parameter mapping (v1 default)

These are the **default mappings for v1, to be tuned during dogfooding** (Slice 01, Task 7). They are starting points, not fixed constants. All inputs are the confidence-weighted (`effective`) values from §3.

| Dimension | Direction | Primary audio parameters | Rationale |
|---|---|---|---|
| `arousal` | higher → more energy | layer/stem density ↑, rhythmic-element gain ↑, spectral brightness (LP cutoff) ↑, reverb tail ↓, tempo micro-adjust ↑ (±2–3% max) | match the soundscape's energy to the listener's activation, without large tempo swings |
| `cognitive_load` | higher → **recede** | melodic/foreground stem gain ↓, transient & novelty density ↓, stereo movement ↓, steady-bed gain ↑, modulation rate ↓ | under high load the audio must get *less* attention-grabbing, not more — the core focus-protection principle |
| `readiness` | lower → gentler & grounding | overall energy envelope ↓, low-frequency grounding bed ↑, harshness ↓; higher → fuller, more sustained focus bed | support a fatigued listener gently; give a fresh one room for deeper material |
| `valence` | higher → warmer/brighter harmony | harmonic warmth / consonant-layer balance (subtle) | affective colouring only; **near-inert in v1** because valence ships at low confidence (PSV §4.1), so the confidence weighting keeps its influence small |

**Combinations matter — read the vector as a whole.** The most important combined case for a focus product: **high `cognitive_load` + low `readiness`** (loaded *and* fatigued) → recede maximally *and* ground (minimal foreground, warm steady bed, no novelty). High `arousal` + high `cognitive_load` → keep energy present but strip anything attention-grabbing (drive density through non-melodic, non-transient layers). Apply the table jointly, not as independent knobs.

---

## 6. Scene model

### 6.1 `mode_hint` → scene

| `mode_hint` | Scene | Musical target (per Stem spec) | Intent |
|---|---|---|---|
| `deep_work` | Deep Work | C Dorian | maximally non-distracting sustained focus |
| `review` | Review | G Major | lighter, slightly more present |
| `wind_down` | Wind-down | A Natural Minor | lowest energy, recovery |
| `creative` | *(v1)* Deep Work base | — | reuse nearest defined scene until a dedicated one exists |
| `admin` | *(v1)* Review base | — | reuse nearest defined scene until a dedicated one exists |
| `null` | default (Deep Work) | C Dorian | no hint → calm default, driven purely by the continuous vector |

The Stem Production Specification is the source of truth for what each scene contains (the six stem roles, their material, keys, loop lengths). `creative` and `admin` reuse the nearest defined scene in v1 because the Stem spec defines three mode-level acoustic targets; dedicated scenes for them are a future-version item.

### 6.2 Transitions

- **Continuous modulation** (parameter changes within a scene) is applied continuously via the smoothing layer (§7) — no scheduling required.
- **Scene / stem-set changes** (a `mode_hint` change) are **scheduled at the next coprime loop boundary**, not applied immediately, then crossfaded. The coprime loop-length architecture (Stem spec) is what makes long, non-repeating soundscapes possible and what hides the seams.
- Crossfades are **equal-power**. Default scene-change crossfade ≈ one bar to a few seconds (tune in dogfooding); within-scene stem swaps use shorter equal-power fades.
- The property we are designing for: the listener's state shifts before they consciously notice the audio changed.

---

## 7. Smoothing & rate limiting (audio domain)

Because the PSV makes no rate-of-change promise, the engine owns all smoothing:

- Each mapped control parameter is smoothed (e.g. one-pole / EMA) before it reaches the DSP graph.
- Per-parameter **maximum slew rates** cap how fast a parameter may move, so even a large PSV jump produces an audible glide rather than a step. Gain especially must never step.
- Gain changes are **perceptual / equal-power**, not linear-amplitude.
- Time constants are per-parameter and tunable — slower for timbral/spectral moves, faster for fine gain.

---

## 8. Master stage — guardrails (non-negotiable)

The master stage enforces the loudness and spectral rails defined in the **Stem Production Specification**. The PSV→parameter mapping operates strictly *inside* these rails and can never override them.

- **Loudness ceiling + hard limiter** on the final bus. True-peak safe.
- **Per-mode loudness normalisation** (LUFS target per scene) — exact targets per the Stem spec.
- **Prohibited frequency bands per scene** — e.g. constraints on sub-bass or high-shelf content in specific modes; exact bands per the Stem spec. The §5 mapping may not raise a parameter such that output enters a prohibited band.
- **Click / pop prevention** — all gain and routing changes ramped; no discontinuities.

If a mapped parameter would violate any rail, the rail clamps it. Guardrails are evaluated last and win unconditionally.

---

## 9. Lifecycle

| State | Behaviour |
|---|---|
| Cold start | PSV is the neutral vector (all 0.5, confidence 0) → render the default scene (Deep Work) at a calm baseline; no strong modulation until confidence rises. |
| Running | Consume PSV at the producer's cadence; modulate continuously; schedule scene changes at loop boundaries. |
| Stale input | (now − `update_timestamp`) > 3× nominal cadence → hold last scene, decay effective confidence toward 0 (parameters drift back to neutral), fall back through `mode_hint` to default. |
| Shutdown | Equal-power fade to silence over a short fixed interval; never a hard cut. |

---

## 10. Telemetry & privacy

PGAE runs fully on-device and makes no network calls. It may write a **derived** session log (active scene, parameter trajectory, the PSV it consumed) for tuning — local, opt-in, carrying no raw inputs (those never reach PGAE anyway). Aligns with PSV spec §11.

---

## 11. Minimum viable PGAE (Slice 01)

For the probe, implement only this subset; defer the rest.

**Implement**
- One scene is enough to start (Deep Work / C Dorian); a second (Review) if cheap.
- 4–6 stems loaded from Stem spec material (or temporary placeholders).
- Map **three** parameters from the confidence-weighted vector: per-stem **gain**, a low-pass **filter cutoff** (brightness), and **layer density** (how many stems are active).
- Apply the confidence-weighting formula (§3) and modulate on **deltas** (§5).
- **Equal-power crossfade** on stem changes, scheduled at loop boundaries (§6.2).
- **Hard limiter** on the output bus (§8).
- Smoothing/slew on gain so nothing steps (§7).

**Defer**
- Full six-stems-per-scene across all three keys.
- Reverb modelling, stereo-width modulation, tempo micro-adjustment.
- `valence`-driven harmony (inert in v1 anyway).
- `creative` / `admin` dedicated scenes.
- Per-mode LUFS targets beyond a single safe ceiling.

This is exactly the surface Slice 01 Task 5 builds against.

---

## 12. Open items / to tune

1. **Mapping coefficients** (§5) are placeholders — tune during dogfooding (Task 7).
2. **Exact LUFS targets and prohibited bands** (§8) — source from the Stem Production Specification.
3. **`creative` / `admin` scenes** (§6.1) — confirm whether to author dedicated scenes later or keep the reuse mapping.
4. **`valence` handling** — revisit §5 once valence confidence improves (tracks PSV spec open items).
5. **Crossfade and slew timings** (§6.2, §7) — confirm by ear in dogfooding.

---

## Changelog

| Version | Date | Change |
|---|---|---|
| 1.0.0 | 16 Jun 2026 | Initial draft for engineering review. Consumer-side companion to `PRISM-SPEC-PSV-001`. |
