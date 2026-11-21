# Queued Metal depth export and reimport

The opt-in path encodes the checked depth export and import behind the original
producer on the same Metal queue. A pending producer must be tracked, and the
depth proof must match its texture, generation, and guest byte coverage. An
exact single-owner export feeds the import buffer directly. Mixed sources keep
the existing ordering and fallback rules.

The import receipt retains the pending producer and both GPU error words.
Queue retirement, CPU exports, presentation, and capture validate its
dependencies before exposing the output. The bounded submission FIFO still
defaults to eight slots. The wider queue and async depth options are shared
between the native Apple Metal frontends.

The experimental settings used to exercise the 32-slot configuration are:

```sh
CARA_METAL_JOIN_CHECKED_DEPTH_IMPORT=1
CARA_METAL_ASYNC_CHECKED_DEPTH_IMPORT=1
CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT=1
CARA_METAL_SUBMISSION_WINDOW=32
```

Build the native macOS Metal app and run the GPU fixture on Apple hardware:

```sh
python3 tools/build_native_metal.py mac
python3 tests/run_metal_async_depth_validation_test.py --build-dir work/build-native-metal-mac
```

The fixture checks pending admission, exact D24S8 round trips, and failure
propagation across queue and guest-read boundaries. CI links it against the
FastInterp-only Metal build. The performance experiment was run separately
on an M3 Pro. Two normal boots to Gadd's lab averaged 99.97% and 100.26% with
32 slots and queued depth. Their worst rolling 120-refresh means were both
16.77 ms, with refresh p99 of 17.67 and 17.82 ms. Those runs also used local
renderer tuning. The mainline change keeps the experimental options off by
default.
