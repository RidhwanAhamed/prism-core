# bindings

- `dart/prism_core_bindings/` — dart:ffi bindings for the public C ABI
  (`include/prism/prism_core.h`): ffigen output in `lib/src/` (regenerate with
  `dart run ffigen --config ffigen.yaml`) plus the idiomatic `PrismCore` wrapper.
  Host round-trip test runs against the macOS dylib:
  `PRISM_CORE_LIB=... PRISM_SCENES=... dart test`.

Bindings capture and forward raw events only — no inference logic lives here
(CLAUDE.md: thin shells).
