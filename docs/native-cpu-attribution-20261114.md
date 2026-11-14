# Native M1 CPU attribution, 2026-11-14

This work measures the Lilia dialogue, “Hey you two! Over here!”, in Monster
Hunter Stories on the local M1. It is not the original grass checkpoint or an
iPad measurement. Guest clock and cap remained 100%, with hardware rendering,
hardware vertex shaders, SDL/CoreAudio sound and no CPU/shader JIT. Native
state reuse and native GPU depth reinterpretation were explicitly off. The
packed iPad GLES path already enables several optimizations absent from this
native control, so costs and candidate gains do not transfer to iPad.

## Exclusive emulation-thread CPU categories

The opt-in native frontend now takes category snapshots immediately after
`RunLoop` returns, when all category scopes are closed. It keeps the baseline
in memory and writes reports after the final sample. Calibration occurs before
warmup. Existing callback benchmark/input controls are rejected in this mode.
Common phase wall clocks must be on under the existing category gate; sampled
phase CPU timing and PICA command profiling must be off. This is instrumented
attribution, not production performance or an optimization result.

Example environment, in addition to a normal isolated native ROM launch:

```sh
env \
  CARA_NATIVE_DIAGNOSTICS=1 \
  CARA_NATIVE_PROFILE_SNAPSHOT=1 \
  CARA_NATIVE_TIMING_COUNTERS=1 \
  CARA_CPU_CATEGORY_TIMING=1 \
  CARA_NATIVE_CPU_CATEGORY_PROFILE=1 \
  CARA_NATIVE_CPU_CATEGORY_WARMUP=120 \
  CARA_NATIVE_CPU_CATEGORY_FRAMES=720 \
  CARA_PHASE_CPU_TIMING=0 \
  CARA_PICA_COMMAND_PROFILE=0 \
  /path/to/native/cara /path/to/game.3ds
```

The last frame setting is the end threshold: the requested measured interval
is 120–720, not 720 measured refreshes. Use normal guarded state loading only
when the genuine build fingerprint matches. Unset other benchmark, scripted
input and capture controls. Reports are `native-cpu-category-before.json`,
`native-cpu-category-after.json` and `native-cpu-category-summary.json` in the
private user directory. Missing reports are unavailable data, never zero.

A native-only targeted frontend relink retained genuine engine fingerprint
`ad30f3c1aa76dde7f100f40b8ec019fd439c9b4a51a698ae5a65175a5199fb80`.
Unchanged engine archives and original binaries were hashed and retained; this
was not a fresh full-engine build. Binary SHA-256 was
`8fd58a5b61d2c936a39f4683d22430c45f6cbfca17761a7985d4ffe0dbbde56b`.
The actual replay restored its own Slot 1 through normal guards and measured
exactly 600 refreshes after 120 warmup refreshes:

| Exclusive category | Instrumented thread CPU, ms/refresh |
| --- | ---: |
| GPU command callbacks, including nested renderer/driver work | 61.4316 |
| Guest CPU execution, including memory/backend work | 12.9897 |
| HLE SVC | 0.7831 |
| Other RunLoop work | 0.4814 |
| DSP HLE tick | 0.0334 |
| Entire emulation thread | 75.7815 |

Wall time was 120.6154 ms/refresh. Other threads are excluded. There were no
category clock failures or counter overflows. The calibration estimate was
176.145 ns per thread-clock read; it is not total observer overhead and was
not subtracted. HLE entry counts are high, so its small measured category is
particularly sensitive to clock overhead.

The first observer incorrectly required the global scheduler counter to
advance. `RunLoopSingleCore` advances the local CPU timer while that scheduler
counter can remain zero. The observer now accepts nondecreasing global values,
retains strict local-tick/frame/wall checks and labels global ticks as unsuitable
for elapsed-time normalization. No core timing behavior was changed. The first
failed replay was preserved; the corrected replay completed normally.

## Separate trace and packet observations

A separate 20-second Time Profiler capture normally restored the same scene,
warmed 120 refreshes and disabled PICA/common timing diagnostics. Its exact
executable and UUID were verified. Among 12,627 main-thread exported sample
rows, 9,679 included `Draw`; 9,043 of those had identified native-driver ancestry.
`ExecuteBlock` had 1,924 self rows; `mach_msg2_trap` had 4,056. These are exported
weights including syscalls/waits, not CPU-duration shares. Nested rows must not
be summed or equated with the category table above.

The diagnostic packet census separately covered exact list endpoints
165376–193792 while the verified dialogue awaited input. All 868,242 VS-source
packets, containing 37,337,698 declared extras, met the proposed repeated-stream
metadata predicate. They were 1.206% of parsed headers. This establishes neither
payload validity nor safe batching, CPU savings or a game speedup. In that
instrumented command interval, 96.676% of inclusive command CPU was nested in
draw processing; shader-upload dispatch cannot explain its dominant cost.

## Timing-counter control

Four same-binary replays used counter-off/on/on/off, 120 warmup and 600 measured
refreshes, identical guarded checkpoint and filesystem basis, no input,
category profiling off, and sampled phase CPU/PICA clocks off. Captured ARM/VFP,
ticks, timers, HID and full final refresh-721 RGBA matched. This is not complete
RAM or every-intermediate-frame equivalence.

Enabled master counters/common wall clocks increased mean thread CPU by an
observed 0.4914 ms/refresh (0.67%). The mean wall difference was 0.7962 ms, while
baseline wall drift was larger; no precise wall-effect or confidence interval
is established. These controls do not measure category-profiler overhead or an
emulator speedup, and cannot be assigned to clocks alone.

The benchmark tool's interactive `--resume-best` path previously retained
profiling flags. It now omits observers, captures, fixtures and scripted input
while preserving selected optimizations, guarded restore and normal audio/input
policy. Benchmark requests remain unchanged. Several frontend flags are
presence-based, so setting them to `0` is insufficient. This changes future
launch requests, not an existing process. Normal iOS startup does not enable
these diagnostics automatically; archived launch requests do not prove the
physical iPad's current state.

## Next test and evidence limits

Successful `ValidateSurface → CopySurface/BlitTextures` operations lack the
source/destination and invalidation-cause records provided for RAM fallbacks.
A bounded metadata trace should distinguish new invalidation, allocation churn,
different fragments and alternating aliases before proposing reuse. A source
must remain valid over the entire copied range, with ownership preserved.
Address equality alone is not evidence that copying is redundant. Existing
exact-lookup caching should not be duplicated.

A separate fresh native Sonic smoke rendered both title screens and exited
normally with sound opened, no JIT and timing counters disabled. It establishes
title presentation only, not gameplay or an iPad/GLES missing-visuals fix.
No new cross-game compatibility or physical-device acceptance is claimed here.

Private receipts are under `profiles/pica-packet-census-20261114`,
`profiles/native-cpu-category-20261114`, `profiles/native-observer-overhead-20261114`
and `profiles/sonic-native-smoke-20261114`. Original saves and ROMs were untouched;
all task-owned game processes ended normally. No physical iPad was contacted.
