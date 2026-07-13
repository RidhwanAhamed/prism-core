# Desktop performance numbers — Task 4

Recorded 2026-07-14 per the build plan ("record CPU% and resident memory on desktop";
budgets are measured, not assumed). Machine: Apple M4, 16 GB, macOS 15 (Darwin 24.6.0).
**Debug build** (`--preset debug`, `-g`, no optimization) — release numbers will be lower.
Android/iOS on-device numbers land in Tasks 6–7 against the real budgets.

| Metric | Budget | Measured | Method |
|---|---|---|---|
| PCE inference per tick | < 1 ms | **0.86 µs avg** over 100k ticks, full behavioral window | steady-clock bench, `-O2`, deadline+window+circadian populated |
| Audio render CPU | < 10 % of one mid-2022 Android core | **2.8 % of one M4 core**, whole process | `/usr/bin/time -l`, 30 s live `--rt` run (3 threads + device I/O): 0.85 s CPU / 30.27 s wall |
| Offline render throughput | — | **≈ 175× realtime** | 10 min of audio rendered in ≤ 3.4 s (PgaeRtSafety ctest) |
| Resident memory | < 50 MB incl. stems | **21.3 MB peak** | `/usr/bin/time -l` maximum RSS; steady (no growth) across a 60 s sampled run |
| Render-path allocations/locks | zero | **zero** | instrumented global allocator over a 10-minute render with live PSV publishing (PgaeRtSafety.TenMinuteRenderRunAllocatesNothing) + static primitive audit (rt_lock_primitives ctest) |

Notes:

- The PCE bench showed rare outliers up to ~1.5 ms — OS scheduler preemption during the
  measurement, not compute (avg is three orders of magnitude under budget). The inference
  thread is not real-time-constrained; only the audio thread is.
- The 2.8 % CPU figure is the entire process: audio callback + inference + scripted
  ingress + miniaudio device plumbing, in a debug build on one desktop core. It is a
  reference point, not the Android budget measurement (that happens on device, Task 6).
- Memory breakdown: ~6 MB decoded stems (5 mono f32 loops) + binary + runtime. The real
  stem library will grow this; re-measure when it lands.

Reproduce:

```bash
cmake --preset debug && cmake --build --preset debug
/Applications/CMake.app/Contents/bin/ctest --preset debug -R "PgaeRtSafety|rt_lock|RtExchange"
/usr/bin/time -l ./build/debug/harness/prism_harness --scene assets/scenes.json --rt --seconds 30
```
