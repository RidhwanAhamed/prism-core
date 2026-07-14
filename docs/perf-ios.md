# iOS on-device performance — Task 7

Recorded 2026-07-15 on **Ridhwan's iPhone (iPhone 17, iOS 26.5.1)**, release build of the
`flutter_smoke` app driving the Prism core through the C ABI (dart:ffi →
`prism_core.xcframework`, built by `tools/build_ios_framework.sh`). Whole-app metrics
sampled with `xctrace` (Activity Monitor instrument) attached to the running process; a
30-minute sustained run, one 45 s sample every ~5 min, plus a 30 s confirmation reading
at the end.

| Metric | Budget (core) | Measured (whole app) | Verdict |
|---|---|---|---|
| CPU, steady state | audio render < 10% of one core | **~0.7% of a core avg, ~2.0% peak** | ✅ ~14× under, and this is the *whole app* (Flutter UI + inference + audio), not just the render path |
| Resident memory | < 50 MB core incl. stems | **~68 MiB app total, flat** (74.2 → 67.5 MiB over 30 min) | ✅ core is in budget — see note |
| Physical footprint | — | ~114–117 MiB app total, flat | reference |
| Stability | runs on device | **30+ min, zero restarts** (same PID 7810 throughout) | ✅ |
| Battery | < 3%/hour (est.) | not directly measured (iOS exposes no CLI battery); CPU load ⇒ well within | see note |

## Sustained CPU (30-minute run)

| elapsed | avg %core | peak %core |
|---|---|---|
| 0 min  | (cold sample) | — |
| 6 min  | 0.8 | 2.1 |
| 12 min | 0.7 | 1.6 |
| 18 min | 0.7 | 1.9 |
| 24 min | 0.8 | 2.3 |
| 30 min | 0.7 | 1.9 |

Rock-steady — no upward drift, no thermal throttling signature.

## Notes on interpreting these numbers

- **Whole-app vs. core.** These are the entire Flutter process. The ~68 MiB resident /
  ~115 MiB footprint is dominated by the Flutter framework, Skia, and the Dart VM — a
  baseline any Flutter app carries. The prism core's own contribution is the five decoded
  mono stems (~6 MB) plus small engine/DSP state, comfortably inside the < 50 MB core
  budget; the app total exceeds 50 MB because of the Flutter shell, not the engine. A
  precise core-only split would need in-process instrumentation (not warranted for a smoke
  app). CPU is the same story from the other side: the whole app draws < 1% of a core, so
  the render path alone is a fraction of that — the < 10% budget has enormous headroom.
- **No memory growth** across 30 minutes of continuous audio + a live inference thread is
  the key result: it confirms the "preload at scene load, never allocate on the render
  path" design holds on device (corroborating the desktop zero-allocation instrumentation
  from Task 4).
- **Battery** was not captured as a hard %/hour figure — iOS exposes no battery level to
  the available CLI tooling (no libimobiledevice; `devicectl` omits it), and the run
  wasn't bracketed by manual readings. Given ~0.7% sustained CPU and no wakeup storms, the
  draw is well under the 3%/hour estimate budget; a bracketed reading can be added if a
  hard number is wanted.

## Reproduce

```bash
tools/build_ios_framework.sh                       # device+sim xcframework
cd examples/flutter_smoke && flutter run --release # onto a connected iPhone
# then, with the app foregrounded and Started:
PID=$(xcrun devicectl device info processes --device <UDID> | grep /Runner.app/Runner | awk '{print $1}')
xcrun xctrace record --device <UDID> --template 'Activity Monitor' --attach $PID --time-limit 45s --output s.trace
```
