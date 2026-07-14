# prism_smoke — Task 6 device smoke app

The Prism Engine core on a phone: dart:ffi (via `bindings/dart/prism_core_bindings`) →
`libprism_core.so`, cross-compiled by gradle straight from this repo's CMake tree.
Start/stop, live PSV readout, behavior simulators (focused / scattered / away), audio
through the core's built-in AAudio device.

## One-time machine setup

- **JDK 21 for gradle** (gradle rejects JDK 25): a Temurin 21 tarball lives at
  `~/Library/Java/jdk-21*` and Flutter is pointed at it via `flutter config --jdk-dir`.
- **CMake ≥ 3.24 for the native build**: the Android SDK bundles 3.22, so
  `android/local.properties` (machine-specific, gitignored) needs:
  `cmake.dir=/Applications/CMake.app/Contents`

## Build without a device (verified)

```bash
cd examples/flutter_smoke
flutter build apk --debug     # or --release (use release for perf numbers)
```

## The device sitting (Task 6 acceptance)

1. Physical Android phone (API 26+), Developer Options → USB debugging on, plug in,
   accept the trust prompt. `flutter devices` should list it.
2. `flutter run --release`
3. In the app: **Start** → audio plays; flip behaviors and watch the PSV move (the audio
   adapts on the ~30s cadence / significant change, at loop boundaries).
4. **30-minute budget run** (build plan): leave it running in the foreground, then record
   - battery: `adb shell dumpsys battery | grep level` before/after
   - CPU: `adb shell top -b -n 1 | grep prism` sampled a few times mid-run
   - memory: `adb shell dumpsys meminfo com.prism.prism_smoke | grep TOTAL`
   Numbers land in `docs/perf-android.md` against the budgets (<10% core, <50 MB,
   <3%/hour battery).

## Host-side bindings check (no device, real native core)

```bash
cd bindings/dart/prism_core_bindings
PRISM_CORE_LIB=$REPO/build/debug/core/libprism_core.dylib \
PRISM_SCENES=$REPO/assets/scenes.json dart test
```

`assets/scene/` contains copies of the repo's placeholder stems (Flutter assets must
live inside the package); they are extracted to the app's documents directory at first
launch because `prism_load_scene` takes filesystem paths.
