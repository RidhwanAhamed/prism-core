# Prism Generative Haptics Engine — Consumption Specification

**Document ID:** PRISM-SPEC-PGHE-001
**Version:** 0.1.0 — *Draft for review*
**Status:** **Draft — proposal only. NOT IMPLEMENTED.** No haptic code, reference implementation, probe, or validated constant exists anywhere in prism-core as of this draft (repo state: commit `acbfd50`; the only mentions of haptics are PSV spec §2/§3 listing haptics as a permitted consumer). Every numeric constant in this document — baselines, authority coefficients, gate thresholds, time constants, rails, frame rate, cycle lengths — is a **design choice pending dogfooding**. None has been validated, measured, or felt.
**Owner:** Ridhwan (architecture) · Mohammed Aswath (implementation)
**Date:** 13 September 2026
**Audience:** Prism engineering, IP counsel
**Companion specs:** `PRISM-SPEC-PSV-001` (state vector / producer contract, `docs/prism-state-vector-spec.md`); `PRISM-SPEC-PGAE-001` (audio consumer contract, `docs/prism-pgae-consumption-spec.md`) — this document is its structural twin for the haptic modality.

---

## 0. What this document is, and is not

This is the consumer-side contract for a **Prism Generative Haptics Engine (PGHE)**: how it would consume a Prism State Vector (PSV) and drive a vibrotactile actuator, entirely on-device. It exists so that (a) engineering can build PGHE to the same standard as the audio engine — real implementation, tests as part of done, measured data behind every statement — and (b) the complete patent specification (due within twelve months of the provisional filed on 29 August 2026) can describe PGHE from a document whose every mechanism is implementable, testable and measurable by then.

It is written to be honest about its own status. Unlike PGAE, **PGHE has no probe to reach parity with**; its acceptance bar is conformance to this specification plus the measurements in §14, not parity. The word "parity" must not leak into PGHE tasks from the audio work.

Three rules apply to every reader:

1. **Nothing here is implemented.** Sentences use "is to", "will", or "proposed". If a sentence reads as a description of existing behaviour, that is a drafting error — flag it.
2. **Nothing here is validated.** The audio coefficients were ported verbatim from a validated probe. The haptic coefficients were not ported from anything; where a *shape* is borrowed from `pgae/src/mapping.cpp` it is labelled a borrowed shape, still a haptic design choice.
3. **PGHE output is aesthetic/comfort haptics only.** It is never an alert, notification, warning, or safety signal; it must never be presented, interpreted, or derived into one (PSV spec §7.4). §8.9 makes this a rail, not a hope.

---

## 1. Purpose & Scope

Defines how PGHE consumes a PSV and produces slow, soft, continuous-feeling adaptive haptics on a single-axis vibrotactile actuator — a phone or tablet LRA/ERM in v1; wearable, seat and steering-wheel actuators are reserved and out of build scope.

This document governs the **mapping and runtime behaviour**: how PSV values become haptic control parameters, how pulses are sequenced and transitions scheduled, what rails bound the output, what the host must do to drive an actuator, and how the engine is to be measured. Pattern material (the pulse shapes and cycle lengths of each pattern set) is defined here in v1 as compiled-in tables (§6); a separate pattern-authoring specification, the haptic analogue of the Stem Production Specification, does not exist yet (§15).

In scope: the read contract, the pipeline, the PSV→parameter mapping, the pattern model, smoothing, rails, the host transport and proposed C ABI, lifecycle, the minimum viable PGHE, tests, and the measurement plan.

Out of scope: PCE internals; the audio engine; any safety-critical signalling (automotive — out of band, PSV spec §7.4); perceptual studies (dogfooding produces tuning input, not claims).

---

## 2. Architectural position (the firewall, from the haptic side)

```
PCE ──► [ PSV ] ──┬──► PGAE ──► audio out
                  └──► PGHE ──► haptic frames ──► (host shell) ──► actuator
```

PGHE is a **second, independent consumer** of the PSV beside PGAE. It receives **only** a PSV (per `PRISM-SPEC-PSV-001`), never reads PCE inputs, models or internal state, never calls back into the PCE — and never shares state with PGAE. The two engines have independent clocks, independent smoothing and independent output paths; the only thing they have in common is the vector they both read.

Structurally, in this repository:

- A new module **`pghe/`** (`pghe/include/pghe/…`, `pghe/src/…`) with CMake target `prism::pghe` that links **`prism::psv` and nothing else** from this repo (no `miniaudio` either — PGHE has no device backend and decodes no assets).
- `pghe/` must never include or link `pce/` **or `pgae/`**. `cmake/CheckFirewall.cmake` gains the rules `pghe:pce`, `pghe:pgae`, `pgae:pghe`, `pce:pghe`, `psv:pghe`, and `pghe` joins its `modules` list. The same reasoning as the audio firewall applies: a haptic engine that could see inference internals would collapse the layered claim, and an engine that could see the audio engine would couple two things the design keeps separate.
- `core/` remains the only target that links `pce`, `pgae` and `pghe`; even there, the PSV snapshot is the only thing that crosses.
- Shared DSP helpers (`OnePole`, fade helpers) live in `pgae/detail/` today. PGHE may not include them. v1 **duplicates** the ~30 lines under `pghe/detail/` with their own pinned tests; factoring a dependency-free `dsp_common/` module is a module addition and therefore a stop-and-ask item (§15).

Adding this module, the ABI additions in §9.2, the second exchange in §9.3, and the firewall/RT-audit extensions are **all "stop and ask" items under CLAUDE.md**. Aswath's sign-off on this document is the gate before any code.

---

## 3. The read contract

PGHE consumes the PSV exactly as the PSV spec defines. The operative obligations (PSV spec §8):

- **Respect confidence.** Every dimension's influence is scaled by its confidence through the effective value
  `effective(d) = 0.5 + (value(d) − 0.5) × confidence(d)`
  (`psv::RtStateVector::effective`, identical to PGAE §3). At confidence 0 a dimension is inert and the engine behaves as if it were neutral (0.5).
- **Modulate on deltas, not absolutes.** Every parameter change eases toward its new target through the smoothing layer (§7); a PSV jump becomes a glide over several pulses.
- **`mode_hint` selects the pattern set; the continuous vector shapes it.** `mode_hint` may be `null` — never require it.
- **Tolerate jumps; PGHE owns smoothing.** The PCE makes no rate-of-change guarantee (PSV §7.2).
- **Rails always win.** Nothing derived from the PSV may push output past the §8 rails.
- **Cold start / staleness.** On the neutral vector (PSV §6) render the default pattern set at its baseline. On staleness (PSV §7.3) hold, decay effective confidence toward 0, and fall back through `mode_hint` to the default set.

**The mechanism PGHE embodies is the audio engine's, unchanged.** For every output parameter *p*:

```
A_p = clamp01( B_p + Σ_d K_{d,p} · (effective(d) − 0.5) )
```

with *B_p* the parameter's neutral baseline, *K_{d,p}* the per-dimension, per-parameter authority coefficient, and clamp01 saturation to [0,1] the only non-linearity, applied last. Confidence enters **linearly** and **only** through `effective(d)`; there is **no confidence threshold anywhere**; at confidence 0 a dimension is inert and *A_p* rests at *B_p*; dimensions are **independent** (no cross terms). This is exactly `map_from_effectives` in `pgae/src/mapping.cpp` over a different parameter set. The density gates in §5.4 are thresholds on a *mapped parameter*, never on confidence — the spec says so explicitly so that "no thresholds" is not misread.

---

## 4. Pipeline

```
PSV ─► [1] Pattern-set select ─► [2] Param map ─► [3] Smooth / latch ─► [4] Pulse sequencer ─► [5] Rail stage ─► frames out
            (mode_hint →           (a, l, r →        (one-pole on the     (layers, raised-        (ceiling, duty,      {amplitude,
             Presence /             intensity, rate,   frame clock;         cosine envelopes,       min interval,        sharpness,
             Companion /            dwell, sharpness,  per-pulse latch      period clock, gate      burst guard, floor,  flags}
             Ground)                density; gates)    + per-pulse slew)    fades at boundaries)    SAFETY MUTE)         per 10 ms
        [6] Transition scheduler: pattern-set swaps and layer gate flips land on silent boundaries (crosscuts 1, 3, 4)
```

1. **Pattern-set selection** — `mode_hint` → one of three compiled-in pattern sets (§6.1). No manifest, no I/O in v1. A change is armed and takes effect at the next period boundary.
2. **Parameter mapping** — the §3 law on the confidence-weighted vector → `HapticParams {intensity, rate, dwell, sharpness, density, active[]}`. Pure, allocation-free, DSP-free, unit-testable: `pghe/src/mapping.cpp` is the **only** place a PSV becomes haptic intent (the haptic side of the firewall).
3. **Smoothing / latch** — `intensity` and `sharpness` are one-pole smoothed every frame; `rate`, `dwell` and the gates are period-structural and are applied only at pulse boundaries; everything a pulse needs is **latched at its onset** so a pulse in flight is never reshaped (§7).
4. **Pulse sequencer** — a period clock on the frame clock (100 Hz, §9.1) emits raised-cosine pulses per active layer, summed into one amplitude channel plus a sharpness channel (§6.2).
5. **Rail stage** — evaluated last, unconditionally, on every frame (§8). It owns the only path to the output buffer.
6. **Transition scheduler** — the analogue of loop-boundary scheduling. Because every layer is a pulse with true silence around it, every discrete change is scheduled on a **silent** boundary; there is nothing to crossfade (§6.3).

Everything stages 2–6 touch is **pre-allocated** in `prism_haptic_enable` on the control thread. The haptic thread's per-call path is one wait-free poll of PGHE's own PSV exchange → consume → render *n* frames. It never allocates, locks, logs, blocks, or performs I/O (CLAUDE.md real-time rule 1, applied to a second real-time path), and its translation units join the `rt_lock_primitives` include-closure audit.

---

## 5. PSV → haptic parameter mapping (v1 default)

These are the **v1 default mappings, to be tuned in dogfooding**. All inputs are the confidence-weighted effective values from §3; `a`, `l`, `r` are the deviations `effective(arousal) − 0.5`, `effective(cognitive_load) − 0.5`, `effective(readiness) − 0.5`; `valence` is inert (every `K_{valence,p} = 0`) because it ships at low confidence (PSV §4.1) and PGAE gives it no term either.

### 5.1 The parameter set

Five control parameters, each a scalar in [0,1] computed by the §3 law, each with a defined neutral baseline, each smoothed, the composite bounded by the §8 rails. These, together with the per-layer activation gates they drive and the frame fields the shell commands (§9.1), are the "haptic parameters of the vibrotactile actuator" this engine controls.

| Parameter | Controls | Physical transfer (applied *after* the law, monotone, fixed) | Neutral `B_p` | Why this baseline |
|---|---|---|---|---|
| `intensity` | peak amplitude of each pulse | identity in v1: commanded peak = intensity (a perceptual curve is Open Item 4; the linear-exactness sweep is defined on the mapped parameter so a future curve cannot muddy it) | **0.22** | felt-but-ignorable. Haptics carry a higher attention cost than audio, so the neutral state — also the cold-start and staleness fallback — must rest well below anything that reads as a notification. Whether it should be non-zero at all is Open Item 1. |
| `rate` | pulse repetition (period of the presence layer) | `f_hz = 0.06 · 5^rate` → 0.06–0.30 Hz (period 16.7–3.3 s); log-mapped like `brightness_to_hz` so equal deltas are equal *ratio* changes | **0.50** → 0.134 Hz, period ≈ 7.5 s | the whole band sits in the slow-breathing range; the ceiling (0.30 Hz) keeps even maximal arousal far below the ~1 Hz cadences that read as ringing. Neutral is near the ~0.1 Hz band reported for relaxation pacing (§5.6, informative). |
| `dwell` | how long each pulse is on (attack + sustain + release) | `pulse_s = 0.15 · (1.6/0.15)^dwell` → 0.15–1.6 s, then capped at `0.25 · period` | **0.50** → ≈ 0.49 s | long, slow pulses read as a breath; short ones as ticks. The cap keeps the sequencer's own worst case at 25 % on-time. |
| `sharpness` | transient character of each pulse | envelope attack `attack_ms = 250 · (40/250)^sharpness` → 250–40 ms, release = 1.5 × attack, raised-cosine; **also** passed through per frame for actuators with a sharpness/frequency axis (Core Haptics `hapticSharpness`; Android 16 `BasicEnvelopeBuilder`) | **0.25** → attack ≈ 158 ms, release ≈ 237 ms | rounded and organic by default: attention is captured by transients, so the default must be the least transient texture. The 40 ms floor (4 frames) keeps every rise ramped and comfortably above the 20–50 ms ring-down Android documents for a short drive, so no pulse is ever a click. |
| `density` | which layers are active (§5.4) | gates: `presence ≥ 0.20`, `accent-eligible ≥ 0.40`, `texture ≥ 0.62`, hysteresis ± 0.03 | **0.50** → presence on, texture off | the salience ladder of the audio density gate (0.35/0.55/0.72), re-cut so that the lowest rung is **silence**: in a focus product the correct output under saturated load is nothing. |

### 5.2 Directions per dimension

| Dimension | Direction | Parameters affected | Rationale |
|---|---|---|---|
| `arousal` | higher → modestly more energy | intensity ↑, rate ↑, dwell ↓, sharpness ↑, density ↑ | match activation as PGAE does, but with smaller authority than audio (PGAE uses 0.9 on brightness/density) because a haptic pulse is intrinsically more salient than a stem. Whether arousal should *match* or *draw down* the pulse rate (entrainment) is Open Item 2. |
| `cognitive_load` | higher → **recede on every lever, ending in silence** | intensity ↓, rate ↓ (longer periods), dwell ↑ slightly (rounder), sharpness ↓ (largest single term), density ↓ (reaches 0 at saturated load → presence gate closes → nothing is rendered) | the core focus-protection principle, stronger for touch than for audio: a vibration is an interrupt by nature. Load has the largest authority in the table, attacks transients first, then layers, then level. Sustained vibration also raises detection thresholds (adaptation, §5.6), so receding is also the honest response — never compensate by pushing harder. |
| `readiness` | lower → slower, softer-but-present, longer, rounder (grounding); higher → lighter | intensity ↑ slightly as readiness falls, rate ↓, dwell ↑, sharpness ↓, density ↑ slightly | a fatigued user gets the breathing-pacer character — long, slow, round pulses near 0.1 Hz — the haptic analogue of PGAE's grounding sub bed; a fresh user needs no grounding. Readiness never overrides load: with load saturated the density gate is closed regardless. |
| `valence` | inert in v1 | none | ships at low confidence; no perceptually unambiguous haptic lever. A future term would most plausibly touch sharpness (warmth) — Open Item 12. |

### 5.3 Coefficient table (design choices — every value is a placeholder to tune)

```
intensity = clamp01(0.22 + 0.30·a − 0.36·l − 0.10·r)   // −0.36·l outranks +0.30·a: load always wins the argument (focus protection);
                                                        //   −0.10·r: mild grounding lift for the fatigued
rate      = clamp01(0.50 + 0.40·a − 0.30·l + 0.20·r)   // arousal quickens within a bounded band; load lengthens the period;
                                                        //   fatigue slows toward the ~0.1 Hz band
dwell     = clamp01(0.50 − 0.20·a + 0.10·l − 0.40·r)   // crisper when activated; rounder under load; fatigue → long breath-like pulses
sharpness = clamp01(0.25 + 0.40·a − 0.50·l + 0.20·r)   // −0.50·l is the largest term in the table: transients are what grab attention
density   = clamp01(0.50 + 0.70·a − 1.00·l − 0.20·r)   // borrowed SHAPE from the audio density (0.9a − 1.0l) with arousal trimmed;
                                                        //   −0.20·r keeps the presence layer a little longer while fatigued and unloaded
```

Every `K_{cognitive_load,p}` for intensity, rate, sharpness and density is **≤ 0**, and for dwell **≥ 0**. §8.8 pins these signs with a test so a retune cannot silently invert the principle.

### 5.4 Layers and gates (the stem analogue)

| Layer | Shape | Gate | Notes |
|---|---|---|---|
| **presence** | one pulse per period: raised-cosine attack (from `sharpness`), sustain, raised-cosine release; peak = latched `intensity`; length = latched `pulse_s` | `density ≥ 0.20` (off below → true silence) | the "bed". Always the first layer on and the last off. |
| **texture** | a sub-pulse at 0.45 × period, peak 0.5 × intensity, length 0.5 × pulse_s, same attack | `density ≥ 0.62` | the "exhale" marker in Ground; the most notification-like element after accent, hence the high gate. Deferred from the MVP (§12). |
| **accent** | one single, slow-rising pulse (attack ≥ 150 ms, peak ≤ min(1.25 × intensity, ceiling)) emitted **once** at a pattern-set transition, at the incoming set's first onset | `density ≥ 0.40` and only in sets that allow it (never in Presence) | transitions are where haptics earn their place; never in deep work. Deferred from the MVP. |

Gate flips are scheduled at the next period boundary; a joining layer ramps its peak in over two periods, a leaving layer ramps out over two periods (§7). All layer envelopes are summed into one amplitude channel; `sharpness` is carried alongside, not summed.

### 5.5 Worked examples (hand-checked values the tests pin; confidence 1.0 unless stated)

| Case | `a, l, r` | intensity | rate → period | dwell → pulse | sharpness → attack | density → layers |
|---|---|---|---|---|---|---|
| Cold start / neutral (all 0.5, conf 0) | 0, 0, 0 | 0.22 | 0.50 → 7.45 s | 0.50 → 0.49 s | 0.25 → 158 ms | 0.50 → presence only. First pulse withheld one full period after enable (§10). |
| Saturated load | 0, +0.5, 0 | 0.04 | 0.35 → 9.5 s | 0.55 | 0.00 → 250 ms | **0.00 → silence.** The deep-work answer. |
| High arousal + high load | +0.5, +0.5, 0 | 0.19 | 0.55 → 6.9 s | 0.45 → 0.44 s | 0.20 → 174 ms | 0.35 → presence only, soft and rounded: energy acknowledged, nothing grabs. |
| Loaded + fatigued, saturated | 0, +0.5, −0.5 | 0.09 | 0.25 | 0.75 | 0.00 | **0.10 → silence** (readiness cannot reopen the gate against saturated load). |
| Loaded + fatigued, load 0.8 | 0, +0.3, −0.5 | 0.16 | 0.31 → 10.1 s | 0.73 → 0.85 s | 0.00 → 250 ms | 0.30 → presence: the grounding breath — slow, long, round, soft. |
| Rested, calm, unloaded (PSV §8.1 row 1) | −0.2, −0.3, +0.3 | 0.24 | 0.57 → 6.7 s | 0.39 → 0.38 s | 0.38 → 125 ms | 0.60 → presence; texture just below its gate. |
| Liveliest corner | +0.5, −0.5, +0.5 | 0.50 | 0.95 → 3.6 s | 0.15 → 0.21 s | 0.80 → 58 ms | 1.00 → all layers the set allows. |
| Ceiling corner | +0.5, −0.5, −0.5 | 0.60 | — | — | — | with an accent boost the composite reaches 0.75 → the §8.1 rail clamps to 0.60. This is the corner the ceiling test drives. |
| Any vector at confidence 0.5 | halves | every parameter moves **exactly half** as far from `B_p` as at confidence 1 — the linearity the sweep (§14, H-M1) measures. |

Reading the table jointly, as PGAE §5 requires: **high load + low readiness** recedes to silence at saturation and to a slow grounding breath short of it; **high arousal + high load** keeps a soft presence and strips everything else. These are consequences of the linear table, not extra rules — adding a rule here would break "no thresholds".

### 5.6 Perceptual basis (informative — not claims of this specification)

The design leans on findings reported in the literature; they inform choices, they are not asserted by this document, and none has been verified for Prism's actuators or users:

- Pacinian-channel sensitivity peaks near 250 Hz (≈ 200–400 Hz), which is why a fixed carrier in that band is the shell's default when it can choose one (Springer s13414-020-02025-y; NCBI NBK10895 — seen via search snippets).
- Vibrotactile amplitude discrimination follows Weber's law with a fraction around 20 %, and relative-change perception is location-independent even though absolute intensity is not — the basis of the per-pulse slew in §7 (Frontiers fnins.2022.958415, snippet).
- Perceived vibrotactile magnitude grows sub-linearly with amplitude (Stevens exponents < 1) — the reason a perceptual transfer is an open item (Springer BF03212732, snippet).
- Extended exposure to an adapting vibration raises detection thresholds — the reason sustained output is avoided and receding is not compensated (J Neurophysiol jn.00519.2015, snippet).
- Breathing near 0.1 Hz is associated with relaxation and maximal HRV/baroreflex resonance; shipping wearable pacers use ~6/min with vibration cues — the reason the Ground set's band is 0.08–0.15 Hz (PMC6753868; Polar Serene manual, snippets).
- Platform guidance: "less is more"; "given the choice of buzzy haptics or no haptics … choose no haptics"; start and end waveforms at zero; step changes in amplitude overshoot and ring ≥ 50 ms (developer.android.com haptics-principles / actuators / custom-haptic-effects — VERIFIED, pages fetched).

---

## 6. Pattern model

### 6.1 `mode_hint` → pattern set

Pattern sets are the scene analogue: **compiled-in constant tables**, pre-allocated at enable, no manifest, no I/O in v1 (a `haptic_patterns.json` is Open Item 13). A set fixes which layers may exist and a band on the physical rate/dwell transfers. Baselines `B_p` are the same in every set in v1 — a per-set offset is another multiplier that would muddy the evidence and is deferred.

| `mode_hint` | Pattern set | Layers eligible | Band | Intent |
|---|---|---|---|---|
| `deep_work` | **Presence** | presence only (texture never, accent never) | rate ceiling 0.16 Hz (period ≥ 6.25 s) | maximally non-distracting: a rare, very soft presence pulse — or nothing |
| `review` | **Companion** | presence, texture, accent | full band | lighter, slightly more present |
| `wind_down` | **Ground** | presence, texture (exhale marker), accent | rate clamped to 0.08–0.15 Hz (≈ 5–9/min); dwell floor 0.6 s | grounding / recovery: the breathing-pacer character |
| `creative` | *(v1)* Companion base | — | — | reuse nearest defined set, as PGAE does |
| `admin` | *(v1)* Companion base | — | — | reuse nearest defined set |
| `venue_*` | *(v1)* Presence base | — | — | venue actuators are speculative; no dedicated sets until hardware exists |
| `drive_*` | *(v1)* **not rendered** (silence) | — | — | automotive is out of v1 scope; when it lands it is comfort only, `drive_energize` is **not** an alert (PSV §9), and the automotive rail table (§8.9) needs safety-lead sign-off first |
| `null` / unknown | Presence | — | — | calm default, driven purely by the continuous vector (PSV §10 forward compatibility) |

### 6.2 The period clock and the layers

The presence period (from `rate`) is the only "loop": `boundary = frame_index % period_frames == 0`, frame-accurate (the haptic analogue of sample-accurate). Texture is phase-locked to it at 0.45 × period; accent fires once at a set transition. There is nothing coprime to align — the seam-hiding property comes from the long inter-pulse silence, not from polyrhythm. The composite is guaranteed silent at every boundary: presence occupies at most the first 25 % of the period and texture at most 0.45–0.575 of it (§8.3 additionally enforces ≥ 0.4 s of true zero before every boundary).

### 6.3 Transitions — nothing overlaps, so nothing crossfades

- **Continuous modulation** (`intensity`, `sharpness`) flows through the smoothing layer every frame and is latched per pulse — no scheduling.
- **Period-structural changes** (`rate`, `dwell`) and **layer gate flips** apply at the next boundary, never mid-pulse; a gate flip ramps the layer's peak over two periods.
- **Pattern-set changes** (a `mode_hint` change) apply at the outgoing set's next boundary. Both sets are silent there, and the incoming set's targets are approached from the **current** smoothed values (never restarted), so a set change can never step the peak. No two-deck crossfade machinery is needed — a deliberate simplification relative to PGAE, and the one place the haptic re-interpretation of real-time rule 3 ("every change is ramped, crossfades are equal-power") differs in form: the property the rule protects — no energy discontinuity at a transition — holds by construction because transitions happen in silence. The audio engine's equal-power law has no haptic analogue in this design; a haptic "equal-power" summation for overlapping layers is undefined for a single-axis actuator and is not needed.
- The property we design for, restated for touch: the user should notice they feel calmer before they notice the pulse changed — and in deep work they should not notice the pulse at all.

---

## 7. Smoothing & rate limiting (haptic domain)

Because the PSV makes no rate-of-change promise, the engine owns all smoothing. All constants are design choices.

- **Frame-clock one-pole** on `intensity` (τ = 0.5 s) and `sharpness` (τ = 1.0 s) — `value += (target − value) · (1 − e^{−1/(τ·f_frame)})`, the same primitive as `pgae::detail::OnePole`, re-implemented under `pghe/detail/` (§2). Slower than PGAE's 0.25 s / 0.6 s because a level change in a single-axis vibration is more noticeable than the same fractional change in a mixed soundscape (UNVERIFIED perceptual assumption). The one-poles run continuously, including through silence, so values converge between pulses. Slew cap 0.5/s on both (a full-range move takes ≥ 2 s).
- **Per-pulse latch.** At each presence onset the smoothed `intensity`/`sharpness` and the boundary-scheduled `rate`/`dwell`/gates are latched for the whole pulse. A pulse in flight is never reshaped. This makes **render-ahead legal**: a frame, once rendered, is final — nothing that arrives later (a PSV snapshot, a set change, a mood override) can reshape it or the pulse it belongs to — so a shell may pull up to one pulse plus gap (≤ 2.5 s) ahead and dispatch each pulse as one platform effect. The cost of rendering ahead is take-up latency, not correctness: the exchange is polled once per render call (§9.2), so a snapshot published while the shell is ahead is consumed at the next call and its first effect is delayed by at most the lookahead on top of the per-pulse latch. H-M3(a) measures that latency as a function of window size. The one input that must not wait is the safety mute, which the host applies to the actuator directly (§8.9).
- **Per-pulse peak slew (asymmetric).** The latched peak of pulse *n+1* may exceed pulse *n*'s by at most `max(0.25 × previous, 0.05)` — roughly one ~20 % Weber step per pulse, with a floor so near-silent pulses can still grow; **decreases are limited only by the one-pole**, so recession is fast and growth is slow. This is the attention rail (§8.5): nothing can "pop" into awareness under any PSV trajectory.
- **Period-structural slew.** `rate`/`dwell` may change the period by ≤ 15 % per period (period JND UNVERIFIED; chosen below the amplitude Weber fraction), so a PSV jump becomes a glide over several pulses.
- **Envelopes.** Raised-cosine attack (40–250 ms from `sharpness`) and release (1.5 × attack); if attack + release exceed `pulse_s` both scale down so the pulse is a pure bump. Per-frame amplitude delta is therefore bounded by the fastest attack at the ceiling: `0.60 · π / (2 · 4 frames) ≈ 0.24` — the bound `PgheRender.NoAmplitudeSteps` asserts. (An audio-style 1e-3 bound is meaningless at 100 Hz; the haptic statement of "nothing steps" is: every rise is a raised cosine of ≥ 4 frames, every fall ≥ 6 frames, and no frame-to-frame change exceeds that slope.)
- **Mute release ≤ 30 ms** (3 frames, monotone) — the one deliberately fast ramp in the engine, still a ramp (§8.9). Un-mute resumes at the next boundary through the normal fade-in.
- **Start:** no pulse for one full period after enable, then the first pulse fades in over two periods — start-up never announces itself. **Stop:** the current pulse completes its release (≤ 2 s), then zeros; never a cut.
- **Staleness** (PSV §7.3): age is measured on the engine's **frame counter since the last fresh snapshot** (the render path reads no wall clock, which also makes staleness deterministic offline). After 3 × the vertical's nominal cadence (90 s aqademiq, 180 s venues, 15–30 s automotive) every confidence is multiplied by `e^{−(age − 3T)/60 s}` before the map, so parameters glide to `B_p` over about a minute; then `mode_hint` falls back to Presence. Note: PGAE does not implement its §9 staleness decay today either — implement in both engines together, from one formulation (Open Item 9).

---

## 8. Rails — guardrails (non-negotiable)

The rail stage is evaluated **last**, on **every frame**, and wins unconditionally; it is the last code a frame passes through before the output buffer and it is part of the audited real-time closure. If a mapped or rendered value would violate a rail, the rail clamps it. A violation is a bug; every rail has a conformance test (§13) and a measured number (§14, H-M7).

1. **Intensity ceiling** — commanded amplitude ≤ 0.60 of actuator full scale (default; host may only *lower* it via `prism_haptic_config.intensity_ceiling`; a value above the built-in ceiling is INVALID_ARGUMENT). Hard clamp, not a soft knee, so a test can assert `max ≤ ceiling + 1e-9` over any hostile drive. Why 0.60: aesthetic haptics must be unmistakably weaker than any alert the host itself produces; it also keeps an LRA out of its saturation plateau. The analogue of the limiter.
2. **Duty / thermal governor** — actuator on-fraction over any rolling 60 s window ≤ 0.30 (default `duty_cap`); no single on-run > 2.0 s. The sequencer's own structure (pulse ≤ 0.25 × period, texture ≤ 0.125 × period) makes 0.375 its worst case, so the governor catches transients (accents, set transitions) — when exceeded it scales the *next* pulses toward zero through a one-pole until the window recovers (pulse-granular: never cuts a pulse mid-way). Why: LRAs/ERMs heat and are duty-rated (figures UNVERIFIED — datasheets not fetched; a conservative choice), detection thresholds rise under sustained exposure, and long buzzes are what users disable haptics over.
3. **Minimum silent gap ≥ 0.4 s** of exact zero between the end of any pulse's release and the next onset, and before every period boundary. Why: far above the documented 20–50 ms ring-down, so the shell's per-window resubmission (§9.4) always lands in true silence, and no rate/dwell combination can become a drone. Enforced by capping the latched pulse length at onset.
4. **Minimum onset interval ≥ 1.5 s** between any two onsets across all layers, and **burst guard**: never ≥ 3 onsets in any 2 s window. The mapping's own fastest period is 3.3 s, so these are backstops against bugs — and the signature that PGHE is not an alert: bursts are the vocabulary of notifications and alarms.
5. **Per-pulse peak rise ≤ `max(0.25 × previous, 0.05)`** (the attention rail, §7); decreases uncapped.
6. **Sub-threshold floor with hysteresis** — commanded amplitude < 0.03 renders as exactly 0 (re-enable above 0.05). Why: driving an actuator below what can be felt heats it and drains battery for nothing, a faint LRA drive is precisely the "buzzy" case the platform guidance says to replace with nothing, and an exact zero is what makes every window start and end at zero (§9.4). This is the sole permitted discontinuity and it is bounded at 0.03.
7. **Zero endpoints** — the first and last frame of every window the host renders (§9.1) are forced to 0 regardless of upstream state.
8. **Focus protection by coefficient sign** — `K_{load,p} ≤ 0` for intensity, rate, sharpness, density and `≥ 0` for dwell; asserted by a test on the table (`PgheMapping.LoadCoefficientSignsProtectFocus`). Implemented as a property of the linear law, not a threshold rule.
9. **Safety-channel arbitration (PSV §7.4 — non-negotiable).** PGHE haptics are aesthetic/comfort only. They are **never** an alert, a drowsiness/lane/collision cue, or any safety signal; no safety state ever enters the core; nothing derived from the PSV may be used as one. The host owns the actuator and any safety haptic channel. When that channel needs the actuator the host **cancels the actuator on its own channel first**, then calls `prism_haptic_set_mute(core, PRISM_HAPTIC_MUTE_SAFETY_CHANNEL)`: the rail stage ramps to zero within 30 ms and holds an exact zero — no PSV, mood override, pattern set or config can raise it — for the alert's duration plus a hold-off (proposed 3 s) so no aesthetic pulse can be read as part of an alert. PGHE never learns *why* it was muted. In automotive the safety lead owns the actuator mux; PGHE must never be enabled on a seat/wheel actuator without that arbitration in place, and the proposed automotive rail table — ceiling 0.50, duty 0.10, onset interval 3 s, presence only — needs the safety lead's sign-off (tracks PSV spec open item 4).
10. **No alert semantics by construction** — no pattern set contains a double-tap, a rising-urgency ramp, or a repeat-until-acknowledged shape; accents are single and sparse. The spec removes the vocabulary rather than relying on intent.
11. **Capability rail** — an actuator without amplitude control (`has_amplitude_control = 0`) gets exact silence from the core, never on/off emulation: bang-bang vibration at any duty is more salient than nothing. A shell that observes the user has disabled vibration, or that the OS/system haptics have pre-empted the actuator, reports a mute; PGHE never sets `FLAG_BYPASS_INTERRUPTION_POLICY` or any equivalent and always yields to Do-Not-Disturb, user vibration settings and system haptics.
12. **User off-switch** — the host must expose a user-facing disable that results in `prism_haptic_set_mute(…, HOST)` or in never enabling haptics. Accessibility and sensory-sensitivity users must be able to opt out, and PGHE must never mask the system haptics they rely on.

---

## 9. Transport & host contract

### 9.1 What the core emits

**Pull model, mirroring `prism_render`.** The host owns the actuator and exactly one haptic thread; that thread calls `prism_haptic_render(core, frames, n)` and receives *n* frames of `{amplitude, sharpness, flags}` at a fixed **100 Hz** frame rate (10 ms frames; design choice, configurable 50–400 Hz at enable). The envelope is the haptic "PCM":

- the offline harness renders it to a file with no actuator — which is what every device-independent measurement in §14 needs;
- every shell stays thin: it converts amplitude frames into its platform's waveform or parameter curve with no knowledge of periods, layers or rails;
- it is wait-free with the same single-reader contract the audio path already uses.

Why not a parameter-frame poll where the shell expands pulses: that would put pattern rendering — where gates, latching, duty and interval rails live — into every shell, violating the thin-shell rule and making "nothing steps" unprovable per platform. Why not an event stream: it keeps the continuous path a shell concern and turns the offline evidence into an event log rather than a sampled signal. Why 100 Hz: reported vibrotactile temporal resolution is ~10–20 ms (§5.6), Android's waveform timings are integer milliseconds and its envelope path wants ≥ 20 ms between control points (VERIFIED-snippet), and 10 ms frames put four frames under the shortest attack while costing on the order of 1e-4 of a core. No built-in actuator exists in the core: miniaudio has no haptics, and adding a backend would be a new dependency (stop-and-ask). Unlike audio there is only the pull model.

**Frame flags** let a shell segment the stream at silent instants without knowing what a pulse is: `PULSE_START` on the first non-zero frame of a pulse, `PULSE_END` on its last, `BOUNDARY` on the first frame of each period (always amplitude 0). A shell may pull ahead by up to one pulse plus gap (≤ 2.5 s) and dispatch each pulse as one platform effect at its onset, or render window-per-period (§9.4); both are legal because of the latch, and they differ only in how much PSV take-up latency the lookahead adds (§7, H-M3(a)). The only input that must be honoured faster than that is the safety mute, which the host applies to the actuator directly and additionally reports to the core.

### 9.2 Proposed C ABI (additive minor bump 0.3.0 → 0.4.0) — STOP-AND-ASK

No existing declaration changes; `prism_core` stays one handle = one PCE + one PGAE + one PGHE + their PSV exchanges. `tests/core/c_compat.c` must consume every new declaration; the ABI tripwire moves to 0.4.0; the Dart bindings are regenerated with ffigen. Style follows `include/prism/prism_core.h`.

```c
/* --- Haptics out (Prism Generative Haptics Engine) -----------------------------------
 * A SECOND consumer of the PSV beside the audio engine. It sees nothing but the PSV,
 * which reaches it through its OWN wait-free single-writer single-reader exchange fed at
 * the same publish point as the audio one, and it renders an amplitude / sharpness
 * ENVELOPE — never actuator commands. There is no built-in actuator: hosts always pull
 * and a thin shell turns frames into platform commands (unit conversion only).
 *
 * AESTHETIC / COMFORT ONLY. PGHE output is never an alert, warning, notification or
 * safety signal and must never be presented or interpreted as one. Any safety-critical
 * haptic channel is out of band (PSV spec §7.4), owned by the host, and takes precedence:
 * the host cancels the actuator on its own channel FIRST, then mutes PGHE.
 *
 * Threading (extends the contract above):
 *   - prism_haptic_enable: control thread, before prism_start (pre-allocates).
 *   - prism_haptic_render: exactly ONE haptic thread (single-reader contract on the
 *     haptic exchange), independent of the audio thread — the two share no state.
 *     Wait-free, allocation-free, lock-free, no I/O, no logging.
 *   - prism_haptic_set_mute / prism_haptic_muted / prism_haptic_last_sequence: any
 *     thread; one atomic each.
 *   - QUIESCENCE BEFORE DESTROY applies to the haptic thread exactly as to the audio one. */

typedef enum prism_haptic_mute_reason {
  PRISM_HAPTIC_MUTE_NONE = 0,           /* not muted */
  PRISM_HAPTIC_MUTE_HOST = 1,           /* user setting / app policy / background */
  PRISM_HAPTIC_MUTE_SAFETY_CHANNEL = 2, /* a safety haptic is active or in hold-off */
  PRISM_HAPTIC_MUTE_PLACEMENT = 3,      /* device on a surface / not on the body */
  PRISM_HAPTIC_MUTE_THERMAL = 4         /* the actuator driver reported a thermal limit */
} prism_haptic_mute_reason;

typedef struct prism_haptic_config {
  /* Frames per second of the rendered envelope; 0 = default (100). Clamped to [50, 400].
   * Fixed for the handle's life once enabled: every time constant and cycle length is
   * derived from it at enable time so the render path never divides by a moving number. */
  uint32_t frame_rate_hz;
  /* 0 = the actuator is on/off only (Android Vibrator.hasAmplitudeControl() false). The
   * core then renders exact silence: a binary buzz is the "buzzy" haptic the platform
   * guidance says to replace with nothing. Non-zero = amplitude control available. */
  int32_t has_amplitude_control;
  /* Rail: hard ceiling on commanded amplitude, (0, 1] of actuator full scale; 0 = default
   * (0.60). A host may only LOWER it (automotive: 0.50 proposed); above the built-in
   * ceiling is INVALID_ARGUMENT. It is a rail, not a volume control. */
  double intensity_ceiling;
  /* Rail: maximum actuator on-fraction over a rolling 60 s window, (0, 1]; 0 = default
   * (0.30). Protects the actuator's thermal limits and the user's tolerance alike. */
  double duty_cap;
} prism_haptic_config;

/* All defaults (100 Hz, amplitude control assumed, ceiling 0.60, duty 0.30). */
PRISM_API prism_haptic_config prism_haptic_config_default(void);

/* Enable the haptic path: allocates and configures EVERYTHING the haptic render path
 * touches — pattern tables, sequencer state, smoothing coefficients, the duty history,
 * the second PSV exchange — here, on the control thread, never at render time. `config`
 * may be NULL for defaults. Call before prism_start (valid again between prism_stop and a
 * restart, like prism_load_scene's window). INVALID_STATE while the engine is running or if
 * already enabled; INVALID_ARGUMENT for an out-of-range field. Independent of
 * prism_load_scene: an audio-only host never calls this, and a haptics-only host needs no
 * audio scene — see the prism_start note below. */
PRISM_API prism_result prism_haptic_enable(prism_core* core, const prism_haptic_config* config);

/* Frame rate of the enabled haptic path (0 before prism_haptic_enable). */
PRISM_API uint32_t prism_haptic_frame_rate(const prism_core* core);

typedef enum prism_haptic_frame_flags {
  PRISM_HAPTIC_FRAME_PULSE_START = 1, /* first non-zero frame of a pulse */
  PRISM_HAPTIC_FRAME_PULSE_END = 2,   /* last non-zero frame of a pulse */
  PRISM_HAPTIC_FRAME_BOUNDARY = 4     /* first frame of a period; always amplitude 0 */
} prism_haptic_frame_flags;

/* One 10 ms frame, post-rail: a shell commands it as-is after unit conversion (Android
 * amplitude 1..255, Core Haptics intensity 0..1, a driver register) and adds nothing. */
typedef struct prism_haptic_frame {
  float amplitude;  /* [0, intensity_ceiling]; 0 = actuator off */
  float sharpness;  /* [0, 1]: 0 rounded / organic .. 1 crisp. Hosts whose platform has a
                     * sharpness or frequency axis pass it through; others ignore it — the
                     * envelope already carries the attack shape. */
  uint32_t flags;   /* prism_haptic_frame_flags */
} prism_haptic_frame;

/* Render `frame_count` frames into out_frames. Real-time safe: wait-free, no allocation,
 * no locks, no I/O, no logging. Single haptic thread only. Before prism_haptic_enable,
 * fills zeros and returns INVALID_STATE. Polls the haptic PSV exchange once per call,
 * exactly as prism_render polls the audio one. Rendering ahead of wall time by up to one
 * pulse plus gap is legal: rendered frames are final (pulse-latched semantics), and a
 * snapshot published meanwhile is consumed on the next call, so lookahead adds up to that
 * much take-up latency. After prism_stop, renders the current pulse's release and then
 * zeros — never a hard cut. */
PRISM_API prism_result prism_haptic_render(prism_core* core, prism_haptic_frame* out_frames,
                                           uint32_t frame_count);

/* Mute (reason != 0) or un-mute (0) the haptic path. Any thread; one atomic store. Muted
 * output ramps to exactly zero within 30 ms (never a hard cut) and stays zero; un-mute
 * resumes at the next period boundary through the normal fade-in. This is the
 * HIGHEST-PRIORITY input: nothing in the PSV, a mood override, a pattern set or the config
 * can override it. It is the LOGICAL mute — a host arbiter must also cancel the actuator
 * directly for immediacy, since PGHE has no path to the hardware. Calling again replaces
 * the reason. Idempotent. */
PRISM_API prism_result prism_haptic_set_mute(prism_core* core, int32_t reason);

/* Non-zero (the reason) while muted. Any thread; one atomic load. */
PRISM_API int32_t prism_haptic_muted(const prism_core* core);

/* Sequence number of the PSV the haptic render path most recently consumed (-1 before
 * any). Any thread; one atomic load. Exists for the measurement plan: a harness reads it
 * after each render call to find the exact frame at which a publish took effect. */
PRISM_API int64_t prism_haptic_last_sequence(const prism_core* core);

typedef struct prism_haptic_params {
  /* Mapped targets, [0, 1], BEFORE smoothing and rails (spec §5). */
  double intensity, rate, dwell, sharpness, density;
  /* Their physical form: presence period and pulse length in seconds, attack in ms. */
  double period_s, pulse_s, attack_ms;
  int32_t active_layers;  /* bitmask: 1 presence, 2 texture, 4 accent-eligible */
  char pattern_set[24];   /* NUL-terminated: "presence" | "companion" | "ground" */
  int32_t muted;          /* prism_haptic_mute_reason */
  int64_t psv_sequence;   /* the PSV these were mapped from (prism_psv.sequence) */
} prism_haptic_params;

/* Recompute the mapped targets from the most recently published PSV — a pure function of
 * that PSV and the enabled config, computed on the caller's thread; NOT a read-back from
 * the haptic thread, so it needs no lock on the render path. For UI and the opt-in
 * derived session log. Any thread. INVALID_STATE before enable or before start. */
PRISM_API prism_result prism_haptic_get_params(prism_core* core, prism_haptic_params* out);
```

No new `prism_result` values are needed. Existing functions keep their signatures; two keep their documented behaviour and one has its precondition **relaxed**, which is part of the stop-and-ask:

- `prism_start` today requires a loaded audio scene (`core->scene_loaded`, `core/src/prism_core.cpp`; header: "Requires a loaded scene"). The proposal relaxes this to "a loaded scene **or** an enabled haptic path", so a haptics-only host can start. Existing callers are unaffected (a relaxed precondition breaks nobody), but the header text changes and `PrismAbi.HapticsOnlyHostNeedsNoAudioScene` pins the new behaviour. `prism_start` also arms the haptic path (neutral PSV → Presence set, first pulse withheld one period).
- `prism_stop` lets the haptic path finish its pulse and then render zeros (§10); the haptic path stays enabled across a stop/restart, as the audio scene stays loaded.
- `prism_destroy` requires haptic-thread quiescence exactly like the audio thread.

### 9.3 Threading and the composition root

Writer side unchanged: `publish_psv` (`core/src/prism_core.cpp`) runs under `pce_mutex` on the inference thread (or a host thread for `prism_set_mood_override`) and publishes the **same** `RtStateVector` into **two** `RtExchange` instances back to back — `rt.exchange` (audio) and `rt.haptic_exchange` (haptics). Each keeps its single-writer/single-reader contract. The second instance is required because `RtExchange` is single-reader by construction (the reader clears the fresh bit and owns `read_idx_`, `psv/include/psv/exchange.h`); sharing one instance between the audio and haptic threads would race. Cost: 3 × `sizeof(RtStateVector)` (336 bytes) and one extra wait-free publish. A mood override therefore reaches haptics by the identical path it reaches audio.

`PrismRt` (`core/src/rt_part.h`) gains `prism::pghe::Pghe pghe; prism::psv::RtExchange haptic_exchange; std::atomic<bool> haptic_ready{false}; std::atomic<int32_t> haptic_mute{0}; std::atomic<int64_t> haptic_last_sequence{-1};`. A new TU `core/src/haptic_render.cpp` holds `prism_haptic_render` (one wait-free poll → `pghe.consume_psv(snapshot)` → `pghe.render(out, n)` → store last_sequence) and, with `pghe/src/engine.cpp` and `pghe/src/mapping.cpp`, joins the `rt_lock_primitives` closure audit (`cmake/CheckRtPrimitives.cmake` gains `-I${ROOT}/pghe/include` and the three TUs; its "suspiciously small closure" floor is raised accordingly). `PgheRtSafety.TenMinuteRenderRunAllocatesNothing` is the dynamic half.

The core **never creates the haptic thread**: no OS hands a shell a real-time haptic callback the way audio devices do, a library cannot stop a thread it does not own (the quiescence rule), and a shell may not be able to give it real-time priority. Host haptic thread per platform: Android — a dedicated `HandlerThread` (or, for the Flutter smoke app, a Dart `Timer` on the root isolate calling the C ABI synchronously through `dart:ffi`, as the existing bindings already do for `prism_get_psv`); iOS — a `DispatchSourceTimer` on a serial utility-QoS queue; embedded Linux — a `SCHED_FIFO` thread with `clock_nanosleep`; desktop harness — no thread, the offline capture loop is the clock. Timer jitter on the Flutter path is UNVERIFIED and is what H-M4 counts; a native thread is the fallback.

### 9.4 Platform translation (thin shells; facts marked VERIFIED / UNVERIFIED)

The shell owns the timer, calls `prism_haptic_render`, converts frames to actuator commands, and calls `prism_haptic_set_mute` from lifecycle and safety events. **No mapping, smoothing or pattern logic on the host** — the frame envelope exists precisely so shells never need to know what a pulse is. Recommended dispatch on both mobile platforms: **window-per-period** — pull the frames up to the next `BOUNDARY` flag, submit them as one effect, re-arm for the boundary minus ~30 ms (the tail of every window is silent by rail 8.3, so timer jitter lands in silence). Whole-pulse dispatch with lookahead is the equivalent alternative.

**Android** (Kotlin, via a Flutter platform channel or a JNI shim beside the `dart:ffi` core; ~60 lines).
- VERIFIED (developer.android.com custom-haptic-effects / reference): `VibrationEffect.createWaveform(long[] timings, int[] amplitudes, int repeat)` takes millisecond timings and amplitudes 0–255 (0 = motor off), `repeat = −1` plays once; `Vibrator.hasAmplitudeControl()` (API 26) — without it "the device vibrates at the maximum amplitude for each positive entry", hence `has_amplitude_control` and the capability rail; "start and end a waveform at zero amplitude whenever possible … some drivers apply active braking"; step amplitude changes overshoot and ring ≥ 50 ms; `VibrationAttributes` usage classes exist and `FLAG_BYPASS_INTERRUPTION_POLICY` must never be set; the `VIBRATE` permission is required.
- VERIFIED (same pages): Android 16 / API 36 `VibrationEffect.BasicEnvelopeBuilder` (intensity, sharpness, duration per control point; must end at zero intensity), `WaveformEnvelopeBuilder` (amplitude, frequency Hz), `Vibrator.areEnvelopeEffectsSupported()` / `getEnvelopeEffectInfo()` (max control points, max duration, min control-point duration). Where available this is the only path that makes the `sharpness` axis physical on Android; the shell thins the dense frames to the device's control-point limit (a pure sampling operation) and falls back to `createWaveform` otherwise.
- VERIFIED: `Composition` primitives (`PRIMITIVE_CLICK/TICK/LOW_TICK/…`, `areAllPrimitivesSupported`) exist; PGHE does **not** use them in v1 — they are discrete click-like events and cannot express a continuous envelope.
- Translation: `timings = {10, 10, …}` (or run-length-coalesced equal amplitudes), `amplitudes = round(1 + 254 × amplitude)` for amplitude > 0 else 0, one `vibrate(effect, VibrationAttributes.createForUsage(USAGE_MEDIA))` per window; `onStop` → `set_mute(HOST)` and `Vibrator.cancel()`; `onStart` → un-mute.
- UNVERIFIED: which `USAGE_*` class is right and how OEM/system intensity scaling under it distorts the transfer (H-M4 measures the actuator, not the frame); whether a new `vibrate()` replaces the playing effect immediately (widely reported; the window-per-period design lands every resubmission in silence so the assumption is harmless either way); background/screen-off vibration policy per OEM; `VibratorManager` / multi-vibrator devices (out of scope).

**iOS** (Swift; ~120 lines).
- VERIFIED (developer.apple.com Core Haptics pages, fetched): `CHHapticEngine` (iOS 13+) with `start()`, `stop(completionHandler:)`, `stoppedHandler` (reasons include `audioSessionInterrupt`, `applicationSuspended`, `idleTimeout`, `systemError`), `resetHandler` ("restart the engine and recreate all players"), `capabilitiesForHardware().supportsHaptics` ("some devices don't support haptic feedback, including iPad"); "the maximum duration of a continuous haptic event is 30 seconds" → PGHE windows are ≤ 16.7 s by construction; `CHHapticParameterCurve` interpolates linearly between control points and applies to all events in a pattern; `hapticIntensityControl` **multiplies** the event intensity, `hapticSharpnessControl` **adds**; `CHHapticAdvancedPatternPlayer` supports `scheduleParameterCurve`, `sendParameters`, `loopEnabled`, `isMuted`.
- Translation: per window, one `CHHapticPattern` = one `hapticContinuous` event (`hapticIntensity` 1.0, `hapticSharpness` 0.0, duration = window) + a `hapticIntensityControl` curve whose control points are `(t, amplitude)` from the frames (thinned; because the control multiplies, the curve value is the frame amplitude directly) + a `hapticSharpnessControl` curve from `(t, sharpness)`; play on an advanced player at `engine.currentTime + lead`; `stoppedHandler` → `set_mute(HOST)`; `resetHandler` → restart engine, recreate players, un-mute at the next boundary.
- UNVERIFIED: any limit on control points per curve and the cost of one player per window (fallback: one event per pulse with attack/release times); the exact `[0, 1]` ranges of `hapticIntensity`/`hapticSharpness` (VERIFIED-weak — confirm from `CHHapticDeviceCapability.attributes(forEventParameter:eventType:)` at runtime); background playback (third-party sources say none by default; `applicationSuspended` is VERIFIED, so the shell reports a mute and the design never depends on background playback).

**Flutter.** VERIFIED by repo: the existing bindings call the C ABI synchronously through `dart:ffi`, so `prism_haptic_render` can be called the same way from a `Timer` (a `renderHaptics` sibling of `PrismCore.render` in `bindings/dart/prism_core_bindings/lib/prism_core_bindings.dart`). The last hop into the actuator API is Java/Swift-only either way, so a small `MethodChannel` carries the window into Kotlin/Swift. `package:flutter/services` `HapticFeedback` is understood to expose preset impacts only (UNVERIFIED — api.flutter.dev unreachable). Existing pub.dev haptic packages are click-oriented, were not evaluated, and adding one is a new dependency (ask first).

**Embedded Linux** (venues; speculative — no actuator exists). VERIFIED (kernel `Documentation/input/ff.rst`, `include/uapi/linux/input.h`, fetched): evdev force feedback uploads effects with `EVIOCSFF`, plays them by writing an `EV_FF` event, scales with `FF_GAIN`, prefers `FF_PERIODIC` over `FF_RUMBLE`; `ff_rumble_effect` magnitudes are `__u16`; `ff_envelope` has attack/fade length and level in ms. VERIFIED via TI product text (snippet; ti.com egress-blocked): the DRV2605 driver IC offers a real-time playback (RTP) mode in which the host streams amplitude over I2C, bypassing the library engine. Translation: an `FF_PERIODIC` sine at the actuator's resonance (LRAs are driven at ~175–235 Hz — VERIFIED via vendor snippets) with the magnitude re-uploaded per frame, or one RTP register write per 10 ms frame; `sharpness` has no channel there. Nothing to build until an actuator is chosen.

**Automotive** (seat / steering wheel). No public API; OEM SDK or CAN gateway — UNVERIFIED and out of v1. Committed regardless of API: comfort only, the safety lead owns the actuator mux, the automotive rail table (§8.9), and the mute contract.

**Desktop harness.** No actuator. `prism_harness --haptic-render out.jsonl --seconds N` captures the frame stream offline through the C ABI (the analogue of the existing `--render out.wav`), with a header carrying commit hash, dirty flag, frame rate, config and pattern set — the reproducible substrate for every device-independent measurement in §14.

---

## 10. Lifecycle

| State | Behaviour |
|---|---|
| Not enabled | `prism_haptic_render` fills zeros and returns INVALID_STATE; the audio path is unaffected (haptics are optional per handle). |
| Cold start | `prism_start` publishes the neutral PSV (all 0.5, confidence 0, `mode_hint` null) → Presence set at baseline (intensity 0.22, period ≈ 7.5 s, presence only). The first pulse is withheld for one full period, then fades in over two periods — start-up never announces itself. No strong modulation until confidence rises. |
| Running | Poll the haptic exchange once per render call; retarget on every fresh snapshot; `intensity`/`sharpness` through smoothing + per-pulse latch; `rate`/`dwell`/gates/sets at boundaries. Shell pulls one window (or one pulse) ahead, which adds up to that much PSV take-up latency (§7, H-M3(a)). |
| Stale input | age > 3 × nominal cadence, measured on the frame counter since the last fresh poll → hold the current set, decay effective confidence (τ = 60 s) so every parameter glides to `B_p`, then fall back through `mode_hint` to Presence (§7). |
| Muted (any reason) | Ramp to zero within 30 ms, hold exact zero; the sequencer phase keeps advancing so un-mute lands on a boundary; on un-mute, the normal fade-in, no accent. |
| App background / screen off | **Shell policy, stated as a MUST:** on background, stop pulling frames, cancel the actuator, `set_mute(HOST)` — a vibration with the screen off reads as a notification. iOS stops the engine on `applicationSuspended` (VERIFIED); Android background vibration policy is OEM-dependent (UNVERIFIED) and irrelevant to the rule. On foreground: un-mute; the engine resumes at the next boundary, never mid-pulse. |
| Actuator unavailable | No vibrator, no amplitude control, `supportsHaptics == false`, engine stopped/reset, user disabled vibration: the shell never enables PGHE or reports a mute. The core keeps rendering when asked (this is how offline evidence is produced); the desktop harness is permanently in this state. Never an error in the core. |
| Thermal limit reported by the driver | Shell asserts `MUTE_THERMAL`; the duty governor is the core-side backstop, not a substitute for the driver's own protection. |
| Safety channel active | Rail 8.9: host cancels the actuator, mutes PGHE for the alert plus hold-off; PGHE output is zero throughout and never learns what happened. |
| `prism_stop` | Inference stops; the haptic path finishes the current pulse's release (≤ 2 s) and then renders zeros; the shell keeps pulling until it observes a `BOUNDARY` frame, then stops its thread. Never a hard cut (the shell's direct actuator cancel is always allowed). |
| `prism_destroy` | Requires haptic-thread quiescence exactly like the audio thread: the host stops its haptic thread first. |

---

## 11. Telemetry & privacy

PGHE runs fully on-device and makes no network calls (privacy invariant 1). It may write a **derived** session log — pattern set, mapped targets, rail engagement counts, the PSV it consumed — for tuning: local, opt-in, carrying no raw inputs (those never reach PGHE anyway). Aligns with PSV spec §11 and CLAUDE.md privacy invariant 3. Nothing about the actuator, placement or the user's body enters the core; the placement mute reason is a host-asserted fact, not a sensor path.

---

## 12. Minimum viable PGHE (Slice 03-H) — the claim-support floor

The smallest surface that lets every sentence of the complete specification's PGHE section be backed by real implementation, tests and measured data. Tasks in build-plan style, one task per review, tests part of done, Aswath reviews every task; stop-and-ask gates marked.

| Task | Scope | Done when |
|---|---|---|
| **H0 — Spec sign-off** [ASK: new module, ABI minor bump, second exchange, firewall/RT-audit rules] | this document; the §9.2 shape; the `CheckFirewall.cmake` rule list | Ridhwan and Aswath sign the draft; open items 1, 3 and 5 decided or explicitly deferred |
| **H1 — Module scaffold** | `pghe/` target linking `prism::psv` only; `pghe/include/pghe/{mapping.h, engine.h, patterns.h, detail/dsp.h}`; firewall rules + a negative self-test that a planted `#include "pgae/…"` in `pghe/` fails the ctest | `firewall_includes` covers `pghe`; the negative test is proven live once and removed |
| **H2 — Mapping** | `HapticParams`; `psv_to_haptic_params` for `StateVector` and `RtStateVector`; the §5.3 table as named `constexpr` constants each with a `// DESIGN CHOICE (unvalidated): why …` comment; the transfers; the gates | `PgheMapping.*` tests (§13) pass, including the worked examples of §5.5 |
| **H3 — Sequencer, smoothing, rails** | one-pole + latch + slew; period clock; presence layer; Presence and Ground pattern sets (Companion if cheap); the full §8 rail block; staleness on the frame counter; fade-in/out | `PgheRender.*` rail-conformance and no-step tests pass offline |
| **H4 — Real-time hardening** | `core/src/haptic_render.cpp`; second `RtExchange` published from `publish_psv`; `CheckRtPrimitives` closure extended; `PgheRtSafety` 10-minute instrumented run | zero allocations/locks; numbers recorded in `docs/perf-desktop.md` |
| **H5 — C ABI 0.4.0** (additive) | the §9.2 declarations; `abi_test` additions; tripwire → 0.4.0; `c_compat.c`; harness `--haptic-render`; Dart bindings regenerated | `c_abi_smoke` passes with the new symbols; the harness renders a 10-minute trajectory offline with a commit-hash header |
| **H6 — Android shell** | Kotlin `MethodChannel` + `createWaveform` window-per-period + `USAGE_MEDIA` + `hasAmplitudeControl` gate + background → mute; smoke-app off-switch | 30-minute on-device run; seam timing, CPU, battery delta recorded (§14) |
| **H7 — iOS shell** | Swift: `CHHapticEngine` + capability check + advanced player per window + parameter curves + stopped/reset handlers | runs on a physical iPhone; control-point limits and player churn measured; numbers in `docs/perf-ios.md` |
| **H8 — Measurement annexure** | §14 H-M1…H-M9 with the record format | `docs/perf-haptics.md` carries the numbers, reproduction commands and commit hashes |
| **H9 — Dogfood** (not a coding task) | ~1 week through the smoke app; tune coefficients; re-run H-M1–H-M4 after every coefficient change | every change logged with its why; the measured tables match the shipped constants |

**Defer:** texture and accent layers; the Companion set beyond a table; any perceptual transfer (γ); Android frequency control beyond pass-through; placement detection and its ABI; venue and automotive shells and sets; the safety-arbitration protocol beyond the mute contract (needs the safety lead); a pattern manifest and load-time I/O; valence; a pulse-descriptor event accessor; a shared `dsp_common/` module (duplicate ~30 lines instead); per-user strength preference in the core (shell-side attenuation only); any dogfood-driven retune before the measurements exist.

**Claim-support floor, stated plainly.** With H1–H8 merged, tested and measured, the complete specification may state — each sentence cited by test name and by the commit hash of the measurement record — that: PGHE controls the haptic parameters *intensity, pulse rate, dwell, sharpness and layer density* of the vibrotactile actuator (and, through density, the per-layer activation gates); each is computed as `A_p = clamp01(B_p + Σ_d K_{d,p}·(effective(d) − 0.5))` with `effective(d) = 0.5 + (value(d) − 0.5)·confidence(d)` [`PgheMapping.ConfidenceScalesLinearly`, `PgheMapping.DimensionsAreIndependent`, H-M1]; at confidence 0 every parameter rests at its baseline [`PgheMapping.ZeroConfidenceIsInert`]; parameters are smoothed and latched so no commanded value steps [`PgheRender.NoAmplitudeSteps`]; a last-stage rail bounds the commanded amplitude unconditionally [`PgheRender.CeilingIsNeverExceeded`, H-M7]; the haptic render path performs no allocation, locking, logging or I/O [`PgheRtSafety.*`, `rt_lock_primitives`]; the PSV is the only input [`firewall_includes`]; confidence weighting changes overshoot, oscillation and variance relative to unweighted and thresholded actuation on recorded state trajectories **by the amounts recorded in H-M2, whatever they are**; latency, tick misses and cold-start time on the named devices are as recorded in H-M3/H-M4/H-M5. **Nothing about feel, nothing about validation, nothing about safety function.** Until the Implement list ships, every sentence about PGHE in filings stays at "configured to control one or more haptic parameters" — the founder's own constraint to counsel.

---

## 13. Tests (part of "done")

Mirrors the audio suite's construction (`tests/pgae/*`, `tests/core/abi_test.cpp`, `tests/psv/exchange_test.cpp`); names are the contract.

**`tests/pghe/mapping_test.cpp`**
- `PgheMapping.NeutralPsvGivesBaselineParams` — neutral vector → every `A_p == B_p` (`EXPECT_DOUBLE_EQ` against the table), presence on, texture off.
- `PgheMapping.ZeroConfidenceIsInert` — extreme values at confidence 0 produce parameters identical to the neutral vector, all fields.
- `PgheMapping.ConfidenceScalesLinearly` — per dimension and parameter, sweep confidence 0→1 in 0.01 at value 1.0 (others neutral, c = 0): `A_p == B_p + K_{d,p}·0.5·c` to 1e-12 wherever unsaturated; no two adjacent steps differ by more than `K·0.005 + 1e-12` (kills any hidden threshold).
- `PgheMapping.DimensionsAreIndependent` — on random `(a, l, r)` grids, `A(a,l,r) − A(a,0,0) − A(0,l,0) − A(0,0,r) + 2·A(0,0,0) == 0` to 1e-12 in the unsaturated region (no cross terms); superposition of single-dimension deviations equals the joint result.
- `PgheMapping.ValenceIsInert` — sweeping valence value and confidence changes nothing.
- `PgheMapping.LoadCoefficientSignsProtectFocus` — asserts the signs of §5.3 (rail 8.8).
- `PgheMapping.HighLoadRecedesToSilence`, `PgheMapping.LoadedAndFatiguedGrounds`, `PgheMapping.HighArousalHighLoadStaysPresentNotGrabbing`, `PgheMapping.RestedCalmUnloadedIsALightCompanion` — the §5.5 rows, hand-checked.
- `PgheMapping.DensityGateActivatesLayersInSalienceOrder` — positive-activation coverage at 0.19/0.21/0.41/0.63 with hysteresis both ways (the PGAE mutation-testing lesson).
- `PgheMapping.TransfersAreMonotoneClampedAndLogScaled` — `rate_to_hz(0) = 0.06`, `(1) = 0.30`, `(0.5) = 0.06·√5`; `dwell_to_s` endpoints; `attack_ms(0) = 250`, `(1) = 40`; clamped outside [0,1].

**`tests/pghe/render_test.cpp`** (rendered fully offline at 100 Hz)
- `PgheRender.StartsSilentForOnePeriodThenFadesIn` — no non-zero frame before the first boundary; first energy strictly after it; the first two pulses rise monotonically.
- `PgheRender.CeilingIsNeverExceeded` — hostile drive (`a=1, l=0, r=0` at confidence 1, Companion, accent boost, set change mid-run) for 60 s: `max ≤ 0.60 + 1e-9` and `> 0.30` (the rail works and is not just silence); repeated with a host ceiling of 0.50.
- `PgheRender.NoAmplitudeSteps` — big retarget, load slam, set change: per-frame `|Δamplitude| ≤ 0.24 + 1e-9`, every rise ≥ 4 frames and every fall ≥ 6 frames of raised cosine, `|Δsharpness| ≤ 0.02` per frame.
- `PgheRender.PulseIsLatchedAtOnset` — a PSV slam mid-pulse changes nothing until the next onset (frames identical to a run without the slam up to the boundary).
- `PgheRender.RenderedFramesAreFinal` — the same frame-indexed publish schedule rendered in 10 ms slices and in 2.5 s slices: no frame rendered before a publish is consumed ever differs between the runs (rendering ahead never rewrites the past), and where every publish lands on a slice boundary in both runs the streams are bit-identical.
- `PgheRender.RenderAheadOnlyDelaysPsvTakeUp` — for a publish at frame *k* and slice sizes {1, 25, 250} frames: `prism_haptic_last_sequence` shows the sequence consumed within one slice of *k*; the first latched pulse reflecting it occurs no later than one period plus one slice after *k*; once both runs have consumed the same sequence and passed the next onset with converged one-poles, their frames agree — the only divergence is bounded take-up delay (the lookahead contract).
- `PgheRender.PeakRiseIsCappedPerPulse` — neutral → `a=1` slam: consecutive latched peaks rise by ≤ `max(0.25×prev, 0.05) + 1e-9`; the decrease direction is uncapped.
- `PgheRender.PeriodChangesOnlyAtBoundaryAndSlews` — rate slam: onset times change by ≤ 15 % per period until converged; the in-flight period is unchanged.
- `PgheRender.PatternSetChangeWaitsForTheBoundary` — `deep_work → wind_down` mid-period: the period completes, the next carries the new set; boundary index exact (mutation-tested like `PgaeRender.CrossfadeWaitsForTheExactLoopBoundary`).
- `PgheRender.EveryWindowStartsAndEndsAtZero` — 10 minutes of windows cut at `BOUNDARY` flags: `frame[0] == 0` and `frame[n−1] == 0` for every window; `≥ 40` zero frames before every boundary.
- `PgheRender.DutyGovernorHoldsTheCap` — worst-case dwell and accents for 5 minutes: on-fraction over every 60 s window ≤ 0.30 + 1e-6; no on-run > 2.0 s; the scale-down is ramped.
- `PgheRender.OnsetIntervalAndBurstGuardHold` — randomised 10-minute PSV stress run: no two onsets closer than 1.5 s; no 2 s window with ≥ 3 onsets.
- `PgheRender.SubThresholdFloorRendersExactZero` — a target just above 0 emits exact zeros, not a hum; hysteresis prevents chatter around 0.03–0.05.
- `PgheRender.MuteReachesZeroWithinThirtyMs` — mute mid-pulse: amplitude 0 by frame 3, monotone descent; stays 0 for 60 s despite hostile PSVs, a mood override at confidence 1 and a set change (`MuteWinsOverAnyPsv`); un-mute resumes only at a boundary through the normal fade-in.
- `PgheRender.NoAmplitudeControlRendersSilence` — `has_amplitude_control = 0` → all zeros for 10 minutes under a hostile PSV.
- `PgheRender.StaleInputDecaysToBaseline` — no fresh snapshot for 90 s then 5 × 60 s: parameters within 1e-3 of `B_p`, monotone and step-free; then the set falls back to Presence.
- `PgheRender.ShutdownFinishesTheCurrentPulse` — stop → the pulse completes its release, then zeros; never a cut.
- `PgheRender.OfflineRenderIsDeterministic` — two renders of the same scripted snapshot schedule are byte-identical (`memcmp` on floats), same build.
- `PgheRender.EveryRailHoldsOnEveryFrameUnderHostileState` — one combined run asserting ceiling, slew, duty, interval and floor simultaneously per frame.

**`tests/pghe/dsp_test.cpp`** — `PgheDsp.OnePoleMatchesTheClosedForm`, `PgheDsp.NextBoundaryIsFrameAccurate` (the duplicated helpers get their own pins).

**`tests/pghe/rt_safety_test.cpp`** — `PgheRtSafety.TenMinuteRenderRunAllocatesNothing` (instrumented global allocator, thread-local audit flag, a writer thread publishing every 2 ms into the haptic exchange; zero allocations/frees on poll → consume → render — same construction as `tests/pgae/rt_safety_test.cpp`); `PgheRtSafety.MuteTogglingAllocatesNothing` (1000 toggles from a second thread during the audited run).

**`tests/psv/exchange_test.cpp`** — `RtExchange.TwoInstancesFedBySameWriterDeliverIndependently` (both readers see every sequence, no interference, no tearing).

**`tests/core/abi_test.cpp`** — `PrismAbi.HapticRenderBeforeEnableIsInvalidStateAndZeroFilled`; `PrismAbi.HapticEnableWhileRunningIsInvalidState`; `PrismAbi.HapticConfigRangesAreValidated` (ceiling above built-in, duty > 1, frame rate outside [50, 400] → INVALID_ARGUMENT); `PrismAbi.HapticRenderProducesBoundedFramesThroughTheAbi`; `PrismAbi.HapticsOnlyHostNeedsNoAudioScene`; `PrismAbi.MoodOverrideReachesHapticParams`; `PrismAbi.HapticMuteIsHonouredThroughTheAbi`; `PrismAbi.HapticLastSequenceTracksPublishes`; `PrismAbi.AudioAndHapticsConsumeTheSamePublish` (sequence equality between `prism_get_psv` and `prism_haptic_last_sequence` after a render); `PrismAbi.AbiVersionIs040` (tripwire).

**`tests/core/c_compat.c`** — extended to reference every new declaration (pure-C compile proof for ffigen).

**ctest** — `firewall_includes` rules extended plus the H1 negative self-test; `rt_lock_primitives` closure extended plus a one-time mutation check that a planted `std::mutex` in `pghe/include` fails.

**Dart** (`bindings/dart/prism_core_bindings`) — `host_roundtrip_test`: enable, start, render 100 frames offline, assert bounded; mute round-trip.

**Golden regressions** — there is no haptic probe, so "golden" here means the recorded PSV trajectories already in `tests/golden/` plus frozen expected frame files produced by the first reviewed implementation: **self-consistency regressions, explicitly not parity** (`PgheGolden.TrajectoryMatchesCommittedSnapshot`). Where they live and how large they may be is Open Item 11.

---

## 14. Measurement plan (the haptic annexure)

The haptic analogue of PRISM-PROT-BM-001. Everything device-independent is produced by the desktop harness's offline render and by unit tests, with no actuator, because stages 2–6 are a pure function of the snapshot schedule *as consumed* (the frame at which each poll returned it) and the frame index — the same classification the audio annexure used for its Metrics 7–9. Device-dependent metrics are reported **per device, never aggregated**.

**Record format (every measurement, no exceptions):** git commit hash (`git rev-parse HEAD`) + dirty flag, device model and OS version (or `desktop-offline`), build preset, frame rate, ceiling/duty config, pattern set, date, operator; written to `docs/perf-haptics.md` (and `docs/perf-haptics-<device>.md`) in the `perf-desktop.md` / `perf-ios.md` table style. The audio annexure did not always carry the hash; the haptic one always does.

| # | Metric | How | Record | Class |
|---|---|---|---|---|
| **H-M1** | **Parameter sweep / linear exactness** (audio Metric 8 analogue) [PRIORITY] | for each dimension `d ∈ {arousal, cognitive_load, readiness}` and each parameter `p`: (a) value sweep 0→1 in 101 steps at `confidence(d) = 1`, other dimensions neutral at confidence 0; (b) confidence sweep 0→1 in 101 steps at value 1.0 and at value 0.0; (c) valence sweeps; targets read through `prism_haptic_get_params` and the harness header | per `(d, p)`: least-squares slope vs `K_{d,p}` (expected equal to 1e-9), max absolute residual over the unsaturated region (expected ≤ 1e-12, reported as measured), `R²`, saturation onset points, `linear_exact` flag; independence residual max over a 21×21×21 grid; one table, one plot per parameter — the per-dimension-authority table for the complete | device-independent |
| **H-M2** | **Ablation: confidence-weighted vs unweighted vs thresholded** (audio Metric 9 analogue) [PRIORITY] | harness replays every `tests/golden/fixture-*.jsonl` PSV trajectory (they carry per-dimension confidence; `cold-sparse` and `scattered-crunch` are the interesting ones) plus a synthetic script with confidence ramping 0→1 over 5 minutes at fixed extreme values, rendering at 100 Hz under three test-build-only modes: **A** spec (`effective = 0.5 + (v − 0.5)·c`); **B** unweighted (`c` forced to 1); **C** thresholded comparator (`c ≥ 0.5 → 1 else 0`) | per fixture and mode, on the commanded amplitude and on each mapped target: overshoot count and magnitude (a latched peak exceeding the eventual settled peak by > 5 % of the move); oscillation (sign changes of the first difference outside a ±0.002 dead band, per minute); variance and total variation `Σ|Δ|` per minute, separately over windows where mean confidence < 0.3; RMSE-to-target with target = the mode-A settled value after each PSV arrival; gate flips per hour; silence→pulse transitions per hour; actuator on-fraction and pulse count per hour; cold-start excursion (max deviation from baseline while all confidences < 0.3); time to first non-baseline pulse. **Hypothesis to record, not assume:** A shows fewer gate flips, reversals and overshoots, lower low-confidence variance, and a later, gentler first departure from baseline than B and C. Report absolute values and A/B, A/C ratios; a result that does not support the hypothesis is reported as such; no significance claim without ≥ 10 sessions | device-independent |
| **H-M3** | **Latency** [PRIORITY] | (a) core, offline and deterministic: frames from a publish (sequence *n*) to the first frame whose latched pulse reflects *n*, via `prism_haptic_last_sequence`; and mood-override publish → first affected onset; reported as a function of period (expected ≤ 1 period + lookahead). (b) on device: `prism_set_mood_override` return → the shell's `vibrate()` / `player.start()` timestamp → (where measurable) accelerometer-detected actuator onset at ≥ 200 Hz (UNVERIFIED that every phone's IMU resolves its own LRA; fallback: an external IMU taped to the device); p50/p95 over ≥ 100 trials. (c) mute latency: `set_mute` → last non-zero frame (offline, expected ≤ 3 frames) and → actuator cancel (on device) | median, p95, max, in ms, per device, with window size | (a) device-independent; (b)(c) per device |
| **H-M4** | **Tick misses and seams** | on the haptic thread, count render calls whose inter-arrival exceeds 1.5 × the nominal window period, and windows the shell failed to submit before the previous one ended; on Android, the seam gap between the end of window *n*'s last non-zero frame and window *n+1*'s first non-zero frame as measured on the accelerometer, compared with the designed silent gap; any truncated pulse counts as a failure | counts and max gap per 30-minute run, per device, per shell (Dart timer vs native thread) | per device |
| **H-M5** | **Cold start** | frames from `prism_haptic_enable` + `prism_start` to (i) the first non-zero commanded amplitude (expected one period), (ii) the first pulse within 5 % of the baseline peak; 10 runs from a cold process for the wall-clock part | frame counts (device-independent) and median ms on device | mixed |
| **H-M6** | **Real-time safety and cost** | zero allocations/locks over a 10-minute offline render with a live publisher (instrumented allocator) + the static closure audit; offline throughput in × realtime; haptic thread CPU on the mid-range Android reference and the iPhone (design budget < 0.5 % of one core); resident memory delta from enabling PGHE (design budget < 256 KB) | as `perf-desktop.md` | mixed |
| **H-M7** | **Rail conformance** | over the H-M2 traces and a 10-minute hostile drive (extreme PSVs every 5 s, set changes every 30 s, accents on): max amplitude vs ceiling; max 60 s on-fraction vs duty cap; min onset interval; max onsets in any 2 s window; max per-frame delta; longest on-run; min silent gap; mute-to-zero frames; on device additionally the actuator acceleration peak vs frame amplitude (the transfer curve, which exposes OEM/system intensity scaling the core cannot see) | measured maxima next to their limits; every row must pass | offline + per device |
| **H-M8** | **CPU / memory / battery delta** | whole-app CPU and RSS with haptics on vs off on the same device and build, 30-minute runs (`xctrace` on iOS as `perf-ios.md`; `adb dumpsys batterystats` / bracketed manual readings on Android — tooling UNVERIFIED); actuator draw is not core CPU and is reported separately; actuator temperature is **not** measured in v1 (no instrumentation — listed as a risk; the duty cap stands in for it) | delta attributable to the haptic path, %/hour; design budget: PGHE adds < 1 %/hour over the audio-only figure | per device |
| **H-M9** | **Determinism / bitwise reproducibility** | the offline frame file for a scripted snapshot schedule is byte-identical across two runs on the same build (sha256 recorded) and matches across platforms (desktop vs the shell's offline mode on device) within 1e-12 per field; a CMake check asserts `-ffast-math` is absent | test identifier, pass/fail, hashes | device-independent |
| **H-M10** | **Dogfood record** (explicitly not a claim metric) | daily use through the smoke app for ~1 week; per session rate distraction in `deep_work` (0–4), comfort in `wind_down` (0–4), and "did you ever mistake a pulse for a notification" (yes/no); every coefficient change logged with its why; re-run H-M1–H-M4 after every change | tuning input only; feeds Open Items 1, 2, 4 | — |

Determinism discipline: no `std::rand`, no wall-clock read, no `-ffast-math` on the render path; the frame counter is the only clock.

---

## 15. Open items / to decide

1. **Non-zero neutral baseline.** A barely-there presence pulse at confidence 0 (proposed, `B_intensity = 0.22`) vs true silence until confidence rises or a host opt-in. Silence is the most unobjectionable fallback but makes the channel on/off with confidence and hides the baseline the mechanism rests on (and makes H-M2's cold-start segment less informative). Owner: Ridhwan; decide by product intent, confirm in H-M10.
2. **Arousal → rate direction.** Match activation (proposed, `K = +0.40`) vs draw-down (pace slightly below the inferred activation to entrain). The entrainment hypothesis is unsourced here; keep moderate until tested.
3. **Stop-and-ask sign-off** (H0): the `pghe/` module, the 0.4.0 ABI additions (frame struct with flags vs amplitude only; `get_params` as pure recomputation; the mute-reason enum; enable-before-start only), the second `RtExchange` in `PrismRt`, and the `CheckFirewall`/`CheckRtPrimitives` extensions. Aswath.
4. **Perceptual intensity transfer.** Identity in v1; a compressive-inverse curve (e.g. `drive = intensity^1.5`, per actuator class) after the law and before the rails is plausible from §5.6 but unsourced for these actuators. If adopted, the H-M1 sweep stays defined on the mapped parameter.
5. **Aqademiq product default.** Should haptics be **off by default** in a focus product and opt-in per user? The engine can only define the mechanism; the default is a shell decision. If dogfooding shows any haptic presence harms focus, the honest outcome is "off by default", which the engine design must not fight.
6. **Coefficient table** (§5.3), gates, time constants, cycle lengths, rails and frame rate — every value is a placeholder to tune (mirrors PGAE open item 1). Every change is recorded with its why and a re-run of H-M1–H-M4, so the complete cites final values with provenance.
7. **Android specifics.** Which `VibrationAttributes.USAGE_*` to declare and how OEM intensity scaling under it affects H-M7's transfer curve; devices with coarse amplitude steps; whether to require the API 36 envelope builders where present; Flutter timer jitter vs a native thread (H-M4 decides).
8. **iOS specifics.** Control points per parameter curve and per-window player cost (UNVERIFIED); background engine behaviour; confirm the `[0, 1]` parameter ranges at runtime.
9. **Staleness decay** (PSV §7.3) is not implemented in PGAE today either; implement in both engines together, from one formulation, with the frame counter as the clock on the wait-free paths.
10. **Shared DSP helpers.** `pghe/` duplicates `OnePole` and the boundary helper because it may not include `pgae/`. Accept duplication with pinned tests (proposed) or introduce a dependency-free `dsp_common/` module (a module addition → ask).
11. **Golden fixtures for haptics.** Frozen expected frame files from the first reviewed implementation (regression, not parity): where they live and how large they may be.
12. **Valence → sharpness "warmth"** once valence confidence improves (tracks PSV open item 2).
13. **Pattern manifest** (`haptic_patterns.json`) and a haptic authoring spec — compiled-in tables suffice for v1; author-editable patterns are a later slice.
14. **Placement / on-desk detection.** A phone on a desk turns a soft pulse into an audible rattle. What raw fact the shell forwards, whether any inference belongs in the core, and its ABI shape. v1: host-asserted `MUTE_PLACEMENT` only.
15. **Venues and automotive.** Pattern sets, actuator classes, the automotive rail table and the safety-arbitration protocol (hold-off length, who owns the mux) — needs the safety lead; re-confirm PSV §7.4 / open item 4 with them before any automotive haptic work.
16. **Cross-modal coherence.** Should haptic pulses lock to the audio loop clock when both engines run? A shared clock would have to be read from `core/` by both, never a dependency between them. v1: independent. Watch for "broken actuator" mis-readings in dogfooding.
17. **Complete-specification drafting dependency.** Agree with counsel which of the five parameters, the frame fields and the rails are named in the PGHE section, and that the H-M1/H-M2 records (with hash and device) are the exhibits. The PGHE section may only cite what H-M1–H-M5 have recorded; schedule the measurement runs so numbers exist by roughly month 9–10 of the twelve, with review time to spare.
18. **PSV spec companion line.** `docs/prism-state-vector-spec.md` lists only the PGAE companion; add this document once signed (owner decision).

---

## 16. Risks the design does not resolve

- **Attention capture.** A vibration is an interrupt by nature; the largest product risk is that even the neutral state reads as a notification. The linear law can only make haptics recede, not invisible. Mitigations are the low baseline, the rounded default, the slow time constants, the onset-interval and burst rails, and Open Item 5.
- **Mistaken for a notification.** A mis-read costs trust and, for accessibility users, real information. Mitigation: no bursts, ≥ 1.5 s spacing, rounded attacks, a ceiling far below alert level, accents never in deep work, rail 8.10.
- **Safety boundary in automotive.** Seat/wheel vibration is a safety-alert channel in many vehicles; an aesthetic pulse there risks being read as an alert or masking one. The mute contract is necessary, not sufficient; PGHE must never ship in a cabin without the safety lead owning the mux.
- **Platform fragmentation.** No amplitude control on low-end Android (binary buzz), no sharpness channel before API 36, iPad/Vision Pro without haptics, OEM intensity scaling, user vibration settings, iOS engine resets: the emitted envelope is not what the actuator does. H-M7's transfer curve is the only honest picture, per device.
- **Background.** An ambient focus product spends much of its life backgrounded; both mobile OSs restrict background haptics. PGHE's value on phones may be limited to foreground use.
- **Thermal and battery.** The duty governor is a model, not a thermometer; actuator temperature is unmeasured in v1.
- **Skin adaptation.** Steady low-level stimulation fades perceptually within minutes (qualitatively well known; magnitudes UNVERIFIED) — acceptable for a focus product, but it undermines any "grounding" claim. Measure before claiming.
- **Coefficient drift vs the measured record.** Every tuning change invalidates H-M1–H-M4 tables; without discipline the complete will cite numbers the code no longer produces. Benchmark runs are part of "done" for any coefficient commit.
- **Scope creep into the shells.** Pressure to put "a little" pulse logic in Kotlin/Swift/Dart would break the thin-shell rule and the single-artifact IP posture.
- **Second real-time path.** A second audited TU and a second host-owned real-time thread double the surface for an accidental lock or allocation; the closure audit must be extended in the same commit as the module.
- **Over-claiming.** Nothing haptic is implemented or validated today, and the provisional enumerates no haptic parameters; the only safe posture until §12 ships and §14 is recorded is "configured to control one or more haptic parameters". Reviewers should grep this document and any filing text for *validated, proven, feels, users reported, tested on device, alert, warn, notify, drowsiness* and treat every hit as a defect unless it cites a hashed record.

---

## References

**Repository (ground truth for structure and style):** `CLAUDE.md`; `docs/prism-pgae-consumption-spec.md`; `docs/prism-state-vector-spec.md`; `docs/slice-02-build-plan.md`; `docs/perf-desktop.md`; `docs/perf-ios.md`; `pgae/src/mapping.cpp`, `pgae/include/pgae/{mapping,engine}.h`, `pgae/include/pgae/detail/dsp.h`; `include/prism/prism_core.h`; `core/src/{prism_core,render}.cpp`, `core/src/rt_part.h`; `psv/include/psv/{psv,rt,exchange}.h`; `cmake/CheckFirewall.cmake`, `cmake/CheckRtPrimitives.cmake`; `tests/pgae/{mapping,render,rt_safety}_test.cpp`, `tests/core/abi_test.cpp`, `tests/psv/exchange_test.cpp`, `tests/golden/README.md`; `bindings/dart/prism_core_bindings/lib/prism_core_bindings.dart`; `examples/flutter_smoke/lib/main.dart`.

**Platform documentation (VERIFIED = page fetched during drafting; snippet = seen only through a search excerpt):**
- developer.android.com — `develop/ui/views/haptics/custom-haptic-effects` (VERIFIED), `develop/ui/views/haptics/haptics-apis` (VERIFIED), `develop/ui/views/haptics/haptics-principles` (VERIFIED), `develop/ui/views/haptics/actuators` (VERIFIED), `reference/android/os/VibrationEffect`, `reference/kotlin/android/os/Vibrator`, `reference/android/os/VibrationAttributes` (VERIFIED); AOSP `core/java/android/os/{VibrationEffect,Vibrator}.java` (VERIFIED, raw source).
- source.android.com — `docs/core/interaction/haptics/haptics-pwle` (snippet).
- developer.apple.com — `documentation/corehaptics/chhapticengine`, `…/chhapticengine/stoppedreason`, `…/preparing-your-app-to-play-haptics`, `…/chhapticevent/eventtype/hapticcontinuous`, `…/chhapticparametercurve`, `…/chhapticdynamicparameter` and `…/id`, `…/chhapticpatternplayer`, `…/chhapticadvancedpatternplayer`, `…/chhapticdevicecapability` (VERIFIED).
- Linux — `Documentation/input/ff.rst`, `include/uapi/linux/input.h` (VERIFIED, raw source); TI DRV2605 product page (snippet); api.dart.dev `NativeCallable.listener` (snippet).

**Perceptual literature (informative only; snippets):** Springer s13414-020-02025-y; NCBI NBK10895; Frontiers fnins.2022.958415; Springer BF03212732; IEEE 8716900; J Neurophysiol jn.00519.2015; PMC6753868; Polar Serene user manual.

---

## Changelog

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 13 Sep 2026 | Initial draft for engineering and IP review. Consumer-side companion to `PRISM-SPEC-PSV-001`, structural twin of `PRISM-SPEC-PGAE-001`. Proposal only; nothing implemented. |
