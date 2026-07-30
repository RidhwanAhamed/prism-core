# Prism Venues ⇄ Prism Core — integration status

**Date:** 2026-07-30
**Scope:** everything from the start of sound-engine integration to now.
**Audience:** anyone picking this up — in particular whoever owns prism-core, since
§7 proposes an ABI addition that needs their review.

---

## 1. What we are trying to do

Put the real Prism engine behind the Prism Venues iPad app.

The app already works end-to-end against its own FastAPI/Supabase backend: a venue
manager signs in, sees zones, and drives a three-rung control model — **Auto**
(schedule or self-drive), **Mood Nudge** (tap one of six named moods), and
**Takeover** (hand the speakers to staff audio). Until now "changing the mood"
changed a database row and nothing else. The goal is for it to change what the room
actually sounds like.

Three behaviours define done:

1. Tapping one of the six moods changes the audio.
2. Self-drive lets the engine choose for itself.
3. Takeover silences the engine entirely so staff can use their own source.

The six moods are fixed client-side and shared with the backend as ids:
`morning-calm, daytime-flow, afternoon-lift, evening-warmth, peak, wind-down`.

---

## 2. Decisions taken, and why

| Decision | Rationale |
|---|---|
| **The engine runs on the iPad, in-process, via Dart FFI** | Matches prism-core's local-first design ("no networking exists in this repo"), and a Dart FFI binding already exists. The alternative — a separate player box in the venue — is what the backend's `devices` table and desired/reported split already assume, so that door stays open. |
| **Engine changes go in a fork** (`aaqibnp971/prism-core`) | Upstream `RidhwanAhamed/prism-core` is never pushed to. The fork's `main` is currently identical to upstream at `54ff0d2`. |
| **Stem curation starts from measurement** | Tempo/key analysis to propose mood assignments, corrected by ear afterwards. See §6 — this turned out to be partly moot. |

---

## 3. What prism-core actually is

Not a mood player. A context engine plus a generative audio engine, separated by a
hard structural firewall:

```
PCE (reads context) → PSV → PGAE (renders audio by mixing stems)
```

- **PSV** (Prism State Vector) carries `arousal`, `valence`, `cognitive_load`,
  `readiness` — each with a confidence — plus an optional `mode_hint` string.
- **PGAE** maps that vector to per-stem gain, filter cutoff and layer density, then
  mixes looped stems.
- `mode_hint` **selects the scene**; the continuous vector shapes it
  (`docs/prism-pgae-consumption-spec.md` §4).
- Scene changes are scheduled at loop boundaries and equal-power crossfaded — which
  maps directly onto the app's existing Seamless / Gentle / Lively setting.

The public C ABI is small and stable:

```
prism_create / prism_load_scene / prism_start / prism_stop / prism_destroy
prism_report_app_switch / prism_report_idle / prism_report_task_deadlines
prism_get_psv / prism_render / prism_device_start / prism_device_stop
```

---

## 4. Four gaps between the engine and the app

1. **No way for a host to choose a mood.** The engine derives its own state; there
   is no entry point to pin one. The app's central interaction has nowhere to go.
2. **The PCE is aqademiq-only.** Its three inputs are app switches, idle time and
   task deadlines — study signals. `PRISM_VERTICAL_VENUES` exists in the enum but
   the config comment states "v1 implements the aqademiq profile". So self-drive
   cannot yet "read a room". The harness confirms it: it prints
   *"behavior cycles every 90s: focused → scattered → away"*.
3. **Three venue mode hints, six app moods.** `docs/prism-state-vector-spec.md` §9
   defines `venue_energize`, `venue_sustain`, `venue_settle`. §10 explicitly permits
   adding `mode_hint` values as a MINOR additive change, so extending to six is
   in-spec rather than a redesign.
4. **One scene loads at a time.** `default_scene` is all that is read, so switching
   moods today means reloading a file — not the crossfade the spec describes.

---

## 5. The central finding: why the engine sounds quiet and thin

Measured engine output against the post-FX renders that were considered good:

| | Channels | Peak | RMS |
|---|---|---|---|
| `prism-render/moods` (engine) | mono | −15.8 to −23.9 dBFS | −33 to −42 dBFS |
| `prism-render/moods-fx` | stereo | −1.0 dBFS | −15.6 to −19.3 dBFS |

A 17–22 dB shortfall. The cause is not the material and not a bug. Four things
compound:

```cpp
// psv/include/psv/psv.h:24
effective() = 0.5 + (value - 0.5) * confidence      // low confidence → 0.5

// pgae/src/mapping.cpp
levels = 0.5 + 0.7*a - 0.6*l                        // neutral PSV → 0.5

// pgae/src/engine.cpp:20
gain = opts_.amp_trim * pow(x, 1.5)                 // 0.4 * 0.5^1.5 = 0.141 = -17 dB
// pgae/include/pgae/engine.h:38
master_gain = 0.7                                   //                      =  -3 dB

// pgae/src/engine.cpp:12
kInitiallyActive = {true, true, false, false, false} // only bed + sub open
```

At cold start the PSV ships **low confidence**, so every dimension collapses to
neutral 0.5, every level lands at 0.5, and the gain curve turns that into ≈ −20 dB —
matching the measured gap. On top of that only **two of five stem roles** are open;
`pulse`, `lead` and `air` start closed.

**The engine is quiet because nothing has told it what the room is doing.**

`amp_trim` and `master_gain` must not be retuned — CLAUDE.md pins the probe's
heuristic constants verbatim with golden-trace parity as the acceptance bar.

---

## 6. What already existed (and we nearly rebuilt)

`D:\ANP\prism-scenes` already contains **six mood manifests in prism-core's own
format** — `mood_morning_calm.json` … `mood_wind_down.json` — with scene ids
matching the app's moods, referencing **38 stems** in `prism-scenes/stems`, every one
exactly **16.000 s / 48 kHz / stereo / PCM_16**, 112 MB total. They load through
today's ABI unchanged.

`D:\ANP\prism-render` holds rendered output: `moods` (engine), `moods-fx`
(post-processed), `moods-loud`.

The raw 859 MB library in `D:\ANP\Audio\Audio Stems\Snipped Audio Stems` is the
*source* material that was already snipped, resampled and curated into the above.

---

## 7. Proposed ABI addition — needs review

A mood is not a scene. **A mood is a named PSV preset plus a scene.** That framing
works with the engine's design rather than against it, and is purely additive:

```c
typedef struct prism_mood_override {
  const char* mode_hint;    /* selects the scene: "venue_peak", etc. */
  double arousal, valence, cognitive_load, readiness;   /* [0,1] */
  double confidence;        /* 1.0 for a manual pin — the manager is certain */
} prism_mood_override;

PRISM_API prism_result prism_set_mood_override(prism_core*, const prism_mood_override*);
PRISM_API prism_result prism_clear_mood_override(prism_core*);
```

Confidence `1.0` makes `effective()` pass values straight through, so Peak's high
arousal genuinely opens `pulse` and `air` and drives gain up, while Wind-down's low
arousal stays sparse and soft. The six moods differ in density and level **because
the engine's own mapping makes them differ** — no volume knob is bolted on, and no
pinned constant is touched.

It maps cleanly onto the app:

| App action | Engine call |
|---|---|
| Tap a mood | `prism_set_mood_override(...)` |
| Self-drive | `prism_clear_mood_override(...)` |
| Takeover | `prism_device_stop(...)` |

**Open for review:** where the override is applied so the pce/pgae firewall and the
real-time rules both hold (no allocation, locks, logging or IO on the render thread;
PSV handoff stays a pre-allocated atomic double buffer).

---

## 8. Work completed

**The engine builds and plays on a Windows dev machine.** MinGW GCC 16.1 + CMake 4.4
+ Ninja; no Visual Studio needed for this half. `prism_harness.exe` runs the six mood
manifests and produces audio at 48 kHz. The five uncommitted Win32 portability patches
in the working tree are what make this possible (drive-letter handling in
`CheckRtPrimitives.cmake`, backslash path splitting, `localtime_r`, `uselocale`,
`posix_memalign`).

**Loop seams found and fixed.** 27 of 38 stems stepped hard across their wrap point —
worst 432× a normal sample-to-sample delta, i.e. a thump roughly four times a minute,
indefinitely. Fixed with a 12 ms symmetric equal-power fade so both edges meet at
zero. Independently verified:

| | Worst ratio | Median | Audible (≥20) |
|---|---|---|---|
| `stems/` (original) | 432.3 | 42.5 | 47 / 76 channels |
| `stems_prepared/` | **8.3** | **1.5** | **0 / 46** |

Originals untouched. Tooling lives in `D:\ANP\Audio`: `analyse_stems.py`,
`loop_seam.py`, `prepare_stems.py`.

---

## 9. Corrections made along the way

Recorded because each one changed a conclusion:

- **The loudness diagnosis was wrong.** The plan called for normalising the stems
  louder. Measurement showed the summed scenes already sit at −12 to −20 dBFS RMS
  with peaks near full scale — every scene had to *back off* for headroom. Raising
  levels would have cost headroom and fixed nothing; the attenuation is in the
  engine's PSV-driven gain chain (§5).
- **The first seam fix was wrong.** Crossfading tail against head ends the file on
  sample *n−1* rather than sample *0*, relocating the discontinuity instead of
  removing it — 13/23 improved, several worse. The symmetric fade-to-zero is the
  correct technique for a self-contained loop with no material beyond it.
- **39 raw stems were analysed unnecessarily.** `prism-scenes` already held curated
  mood scenes (§6). That should have been checked first.

---

## 10. Blockers

**No native build target on the current dev machine.** `flutter doctor`:

```
[√] Chrome        - develop for the web
[X] Visual Studio - Visual Studio not installed; this is necessary to develop Windows apps
[X] Android       - Unable to locate Android SDK
```

FFI requires a native build. VS Code is installed but is an editor, not a compiler.
Resolution is Visual Studio **Build Tools** with the "Desktop development with C++"
workload (~7 GB), which unlocks `flutter run -d windows`; or an Android SDK; or a Mac
for the real iOS target. This gates *only* the app-side audio step — all engine work
proceeds without it.

**The Stem Production Specification does not exist in the repo.** The consumption
spec defers to it as source of truth for "the six stem roles, their material, keys,
loop lengths", and for the coprime loop-length architecture that hides scene seams.
Without it, the role taxonomy and the 16 s uniform loop length are effectively our
invention. *Does this document exist elsewhere?*

**Both prism-core repos are public.** `START-HERE.md` Gate 0 states "Private repo.
Nothing public until the provisional patent is filed." An unauthenticated probe
returns HTTP 200 for both upstream and the fork. The fork did not cause this — the
disclosure already existed upstream — but if the provisional is not yet filed, a
public repo can start a disclosure clock or bar patentability in some jurisdictions.
**Flagged for a decision, not something code can fix.**

A related detail: a naive MinGW shared build auto-exports ~1251 symbols, including
mangled `prism::pce` internals. Any DLL we ship needs explicit `__declspec(dllexport)`
on the 16 public ABI symbols, or engine internals leak.

---

## 11. Planned next steps

1. Commit the five Win32 patches to a branch on the fork; add a Windows CI job.
2. Add the six venue `mode_hint` values and the override API (§7).
3. Merge the six manifests into one multi-scene `scenes.json`; wire `mode_hint` →
   scene selection with the crossfade the spec already describes.
4. **Listening pass** — hear all six moods at their intended density and judge
   whether they are genuinely distinct. This is a musical judgement, not a
   measurable one.
5. Build a properly-exported shared library for Dart FFI.
6. Wire the Flutter app: mood tap → override, self-drive → clear, takeover → stop.
   *(Blocked on a native toolchain.)*

---

## 12. Questions

1. Does the **Stem Production Specification** exist? It governs stem roles, keys and
   coprime loop lengths, all currently improvised.
2. Is the provisional patent filed? Both repos are public (§10).
3. Is the proposed override ABI (§7) acceptable in shape, and where should it be
   applied to respect the firewall and the real-time rules?
4. Is a **venues PCE profile** planned? Without it, self-drive cannot read a room —
   the app's "Prism is picking the vibe" copy currently overpromises.
5. The six moods need six `mode_hint` values against the spec's three. Extend the
   enum (§10 permits it), or map six onto three and accept less distinction?
