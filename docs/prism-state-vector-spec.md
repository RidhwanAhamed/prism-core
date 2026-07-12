# Prism State Vector — Interface Specification

**Document ID:** PRISM-SPEC-PSV-001
**Version:** 1.0.0 — *Draft for review*
**Status:** Draft — pending sign-off (engineering + IP)
**Owner:** Ridhwan (architecture) · Mohammed Aswath (implementation)
**Date:** 15 June 2026
**Audience:** Prism engineering, IP counsel
**Companion:** `PRISM-SPEC-PGAE-001` — PGAE consumption spec (`docs/prism-pgae-consumption-spec.md`)

---

## 1. Purpose & Scope

This document defines the **Prism State Vector (PSV)** — the single data structure that crosses the boundary between the Prism Context Engine (PCE) and any downstream consumer, including the Prism Generative Audio Engine (PGAE).

It is a **contract**, not an implementation guide. It specifies *what* the PCE emits and *how* a consumer must read it. It deliberately says nothing about how the PCE infers the vector internally, nor how the PGAE turns it into sound. Those live in separate specs and may change freely as long as this contract holds.

In scope: the vector schema, value semantics, confidence model, metadata, serialization, temporal behaviour, consumption rules, and evolution policy.

Out of scope: PCE model architecture, adapter implementations, PGAE DSP, and — critically — any safety-critical signalling path in the automotive vertical (see §7.4).

---

## 2. Architectural Context

```
Inputs ──► PCE ──► [ PRISM STATE VECTOR ] ──► Consumer (PGAE, haptics, UI, scheduling)
```

One rule governs everything below: **the PCE emits an abstract representation of user/environment state and nothing else.** It never emits audio parameters, stem IDs, BPM, or filter values. The translation from state to sound is the consumer's responsibility and the consumer's alone.

This separation is the structural property that lets the same PCE drive different consumers per vertical, and it is the basis of the intended IP position (see §12). Treat it as inviolable.

---

## 3. Design Invariants

These must hold for every emitted vector. A consumer may assume them; the PCE must guarantee them.

1. **Bounded.** Every semantic dimension is a float in `[0.0, 1.0]`. `0.5` is neutral/baseline.
2. **Confidence-bearing.** Every dimension carries its own `confidence ∈ [0.0, 1.0]`.
3. **Consumer-agnostic.** The vector contains no audio-, haptic-, or UI-specific values.
4. **Self-describing.** Every vector carries its `schema_version`, `vertical`, and `update_timestamp`.
5. **Additive evolution.** New dimensions may be added within a major version; existing dimensions never change meaning (see §10).
6. **No audio parameters, ever.** Restating invariant from §2 because it is the one that protects the IP firewall.

---

## 4. The State Vector

### 4.1 Semantic dimensions (v1)

| Dimension | Range | `0.0` | `0.5` | `1.0` | Informative drivers (non-binding) |
|---|---|---|---|---|---|
| `arousal` | 0–1 | deeply relaxed / drowsy | calm-alert baseline | highly activated / keyed-up | HR, HRV, behavioural intensity, circadian phase |
| `valence` | 0–1 | negative affect | neutral | positive affect | *weakly inferable in v1 — see note* |
| `cognitive_load` | 0–1 | unloaded / idle | moderate demand | saturated / overloaded | task type, deadline proximity, app-switching; (auto: road/traffic/vehicle dynamics) |
| `readiness` | 0–1 | depleted / fatigued | moderate capacity | fresh / high capacity | circadian phase, session/trip duration, HRV trend, time-on-task |

**Note on `valence`:** affective valence is hard to infer reliably from the v1 input set. The slot is reserved and will usually ship with **low confidence**. Consumers must lean on it lightly until confidence improves. Whether to ship it in v1 or defer to v1.1 is an open item (§13).

**Dimensions are independent signals, not a taxonomy.** Consumers are expected to read *combinations*. For example, high `cognitive_load` with low `readiness` indicates at-risk overload — a state a consumer may treat very differently from high load with high readiness.

### 4.2 Confidence

Each dimension's `confidence` expresses how much the PCE trusts that estimate *right now*, given which inputs are available and how recently they updated.

- `0.0` — no usable input for this dimension; the value is a pure prior and should be treated as "unknown."
- `1.0` — high-quality, recent, multi-signal estimate.

Confidence is **not** optional and **not** global. A vector can carry a high-confidence `cognitive_load` alongside a near-zero-confidence `valence` in the same tick (common in v1).

### 4.3 Metadata

| Field | Type | Required | Description |
|---|---|---|---|
| `schema_version` | string (semver) | yes | Version of *this* spec the vector conforms to, e.g. `"1.0.0"`. |
| `vertical` | enum string | yes | Adapter profile that produced the vector: `aqademiq` \| `venues` \| `automotive`. |
| `update_timestamp` | int64 (epoch ms) | yes | Emission time. Monotonic within a stream. ISO-8601 acceptable in JSON transport. |
| `sequence` | int64 | recommended | Monotonic counter for ordering and dedup. |
| `mode_hint` | enum string \| null | optional | Discrete summary of state for consumers that want a shortcut (see §9). `null` = no hint; use the continuous vector. |

---

## 5. Serialization

The nested `{value, confidence}` form keeps each dimension's estimate and trust together and makes additive evolution clean.

### 5.1 JSON (reference transport — Aqademiq, Venues)

```json
{
  "schema_version": "1.0.0",
  "vertical": "aqademiq",
  "sequence": 4821,
  "update_timestamp": 1750000000000,
  "mode_hint": "deep_work",
  "state": {
    "arousal":        { "value": 0.72, "confidence": 0.81 },
    "valence":        { "value": 0.55, "confidence": 0.30 },
    "cognitive_load": { "value": 0.85, "confidence": 0.77 },
    "readiness":      { "value": 0.31, "confidence": 0.68 }
  }
}
```

### 5.2 JSON Schema (validation)

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["schema_version", "vertical", "update_timestamp", "state"],
  "properties": {
    "schema_version": { "type": "string", "pattern": "^\\d+\\.\\d+\\.\\d+$" },
    "vertical": { "enum": ["aqademiq", "venues", "automotive"] },
    "update_timestamp": { "type": "integer", "minimum": 0 },
    "sequence": { "type": "integer", "minimum": 0 },
    "mode_hint": { "type": ["string", "null"] },
    "state": {
      "type": "object",
      "additionalProperties": {
        "type": "object",
        "required": ["value", "confidence"],
        "properties": {
          "value":      { "type": "number", "minimum": 0, "maximum": 1 },
          "confidence": { "type": "number", "minimum": 0, "maximum": 1 }
        }
      }
    }
  }
}
```

### 5.3 Binary transport (automotive / embedded)

For the in-cabin and embedded-venue paths, use **protobuf** with the same field semantics (scaled int or float fields). The JSON form remains the canonical reference; the protobuf schema must mirror it field-for-field and is versioned in lockstep.

---

## 6. The Neutral / Cold-Start Vector

On first run, before any personalization or input history exists, the PCE emits the **neutral vector**: every dimension `value = 0.5`, every `confidence = 0.0`, `mode_hint = null`. Consumers must render this gracefully (calm, generic output) rather than treating absence of data as an error. This is also the fallback a consumer falls back to under prolonged staleness (§7.3).

---

## 7. Temporal Semantics

### 7.1 Emit cadence

| Vertical | Nominal emit | Event-driven emit between ticks | Notes |
|---|---|---|---|
| `aqademiq` | every ~30 s | on significant state change | human context changes slowly |
| `venues` | every ~60 s | on occupancy step change | room state is slow-moving |
| `automotive` *(aesthetic path)* | every ~5–10 s | on context change | responsiveness for comfort adaptation only — **not** safety |

Cadence targets are subject to battery/CPU confirmation with engineering (§13).

### 7.2 Who smooths

The **PCE** emits its best current estimate. It may apply light internal smoothing (documented per vertical), but it makes **no rate-of-change guarantee** — values may step.

The **consumer** owns domain-appropriate smoothing. The PGAE, for instance, eases toward new targets and schedules stem swaps at loop boundaries; that smoothing logic belongs to the audio domain and must not be pushed up into the PCE. **Consumers must tolerate arbitrary jumps between successive vectors.**

### 7.3 Staleness

If `now − update_timestamp` exceeds 3× the vertical's nominal cadence, the consumer should hold the last vector but **decay its effective confidence toward 0**, falling back first to `mode_hint`, then to the neutral vector (§6).

### 7.4 Safety channel is out of band

In automotive, any safety-critical intervention (e.g. a drowsiness alert) is **not** represented in this vector and **must not** be derived from it by a consumer. Safety signalling travels on a separate, rule-based channel with its own latency and certification budget. The PSV governs *aesthetic* adaptation only. This boundary is non-negotiable and exists to keep the certifiable safety path free of any ML-derived state.

---

## 8. Consumer Contract

A consumer reading the PSV must:

1. **Respect confidence.** Scale a dimension's influence by its confidence. Recommended rule — blend toward neutral as confidence falls:
   `effective = 0.5 + (value − 0.5) × confidence`
   At `confidence = 0` the dimension exerts no influence; at `1.0` it exerts full influence.
2. **Map deltas, not absolutes,** for continuous modulation, to avoid jarring jumps when context shifts.
3. **Use `mode_hint` for discrete choices** (which stem set, which scene) and the continuous vector for fine modulation. Never *require* `mode_hint` — it is a convenience, and may be `null`.
4. **Treat the vector as advisory.** A consumer's own hard guardrails (loudness limits, prohibited frequency bands, safety rails) always take precedence over anything the vector implies.
5. **Validate `schema_version`.** Reject or down-convert vectors from an unknown major version (§10).

### 8.1 Interpretation examples (informative)

| Vector (`arousal, valence, load, readiness`) | Reading | A consumer might… |
|---|---|---|
| `0.30, 0.50, 0.20, 0.80` | rested, calm, unloaded | render sparse, low-energy ambience (pre-session / wind-down) |
| `0.72, 0.55, 0.85, 0.31` | activated, heavily loaded, depleting | hold steady supportive texture; suppress novelty/transients to protect focus |
| `0.45, 0.40, 0.30, 0.35` | low energy, slightly negative, fatiguing | gentle lift in energy without raising cognitive demand |

---

## 9. Mode Hints

`mode_hint` is an optional categorical shortcut. The continuous vector remains the source of truth.

| `mode_hint` | Vertical | Meaning |
|---|---|---|
| `deep_work` | aqademiq | sustained high-focus work |
| `review` | aqademiq | lighter review / reading |
| `creative` | aqademiq | open / divergent work |
| `admin` | aqademiq | low-load routine tasks |
| `wind_down` | aqademiq | session ending / recovery |
| `venue_energize` | venues | raise room energy |
| `venue_sustain` | venues | hold steady ambience |
| `venue_settle` | venues | calm the room |
| `drive_focus` | automotive | steady-state attentive driving |
| `drive_energize` | automotive | gentle alertness support (comfort, **not** a safety alert) |
| `drive_calm` | automotive | reduce stimulation under high cognitive load |
| `null` | any | no hint; consume the continuous vector |

`drive_energize` is an aesthetic comfort hint. It is **not** the safety alert (§7.4).

---

## 10. Versioning & Evolution

- **Semver on this spec.** `MAJOR.MINOR.PATCH`.
- **Additive (MINOR):** new dimensions or `mode_hint` values may be added. A new dimension must define a **neutral default** (typically `0.5`) and ship at `confidence = 0` for consumers that don't yet read it, so existing consumers are unaffected. *Example:* a future `social_density` dimension for venues would be added this way in `v1.1.0`.
- **Breaking (MAJOR):** changing the meaning, range, or removal of a dimension. Requires a major bump and explicit consumer migration.
- **Forward compatibility:** consumers must ignore unknown fields and unknown `mode_hint` values gracefully rather than erroring.

---

## 11. Privacy

The PSV is derived **on-device** from raw inputs that never cross the PCE boundary. The vector itself is low-dimensional and non-identifying, but it is behaviourally derived and must be treated as personal-data-adjacent:

- Raw inputs (screen activity, biometrics, gaze, CAN data) **never** leave the device.
- The vector is consumed locally; it is not transmitted as a matter of course.
- Any model-improvement telemetry must be aggregated on-device or federated — never raw event upload.
- Handling must satisfy UAE PDPL and GDPR-style data minimization.

---

## 12. Note for IP Review

*For counsel — not legal drafting.*

This spec defines the PCE output as an **abstract, consumer-agnostic representation of state**. The PCE does not emit audio parameters; audio generation is performed by a separately specified downstream engine (PGAE). Counsel should assess whether this layered separation, combined with the novel Aqademiq input set — **deadline proximity + behavioural screen signals + chronobiological signals, fused via multi-head attention** — supports the intended claim structure and a clean freedom-to-operate position relative to existing node-graph/circadian-spine audio patents.

**Disclosure constraint:** do not publicly disclose the input set or architecture (decks, threads, public repos) before the provisional is filed.

---

## 13. Open Items

1. **PSV dimensionality — DECIDED (16 Jun 2026).** v1 ships the **four** dimensions defined in §4.1 (`arousal`, `valence`, `cognitive_load`, `readiness`). Expansion to a larger set (e.g. the previously discussed 6-dimension vector) is **deferred to a future version** and, if adopted, will be added **additively** per §10 — new dimensions default to neutral at confidence 0, so existing consumers are unaffected. No reconciliation work is required for v1.
2. **`valence` in v1.** Confirm whether to ship `valence` at low confidence or defer to `v1.1`. The slot is reserved either way.
3. **Per-vertical cadence.** Confirm nominal emit intervals (§7.1) against battery/CPU budget with Aswath.
4. **Automotive latency + firewall.** Confirm the aesthetic-path latency target and re-confirm with the safety lead that the safety-critical channel is fully outside this contract (§7.4).

---

## Changelog

| Version | Date | Change |
|---|---|---|
| 1.0.0 | 15 Jun 2026 | Initial draft for engineering + IP review. |
| 1.0.0 | 16 Jun 2026 | Recorded dimensionality decision (4 dims for v1; expansion deferred, additive per §10). Added companion PGAE consumption spec (`PRISM-SPEC-PGAE-001`). |
