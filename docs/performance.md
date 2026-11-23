# Performance measurements

## Native Metal/FastInterp investigation, 2026-11-20

November 21 correction: earlier 95–96% runs saved a checkpoint before their
measured interval, clearing renderer watch history; they do not establish
uninterrupted normal-play speed. After verified watch retirement and cache/
queue improvements, the explicit tuned v29 configuration completes the actual
Gadd normal-boot window at **16.713252 ms/ref (99.72%)**, with 14.763763 thread CPU
ms/ref. This run has no restore or save before/during global 3120..3600; the
checkpoint is saved only after measured snapshots. Original endpoint pixels
and retained CPU/ticks match exactly. The otherwise matched latency-critical
arm measures 16.691765 ms/ref, which does not independently establish a gain from
that single pair.

Same-binary restored policy comparisons measure 21.214799/21.272099 ms/ref for
shared 24 defaults, 17.678809 for the queue bundle, 20.550465 for vertex options,
20.803325 for checked depth, 17.503319 for queue+vertex, 17.045710 with depth added,
and 16.680386 with fenced async validation and borrowed RAM comparison added.
All exact same-build guards pass; these grouped results do not isolate every
flag. They motivate the source promotion to 42 defaults plus memory-gated larger
caches/paging and separately validated ordinal lookup. **The v30 shared 42 build now passes ordinary-default acceptance in the local
window, as recorded below.** Latency-critical activity, untracked
uploads and compute experiments remain outside that policy.

The ordinary-default v30 normal boot measures **16.838033 ms/ref** including
final GPU/capture drain, with 14.917846 thread CPU ms/ref: about 98.983% elapsed
speed, separately 99.14562% in the weighted speed-window summary, and 21.5965 ms
refresh p99. Original normal endpoint pixels and CPU/ticks match exactly.
Restored controls measure 16.791870/16.720592 ms/ref; B passes all strict guards
against A. Only learning/prewarm are explicit Metal overrides; ALU pairing equals
the shared FastInterp default. The normal interval preserves uninterrupted
renderer history. This verifies the shared 42 defaults locally, while exact
sustained 100% across gameplay or Apple devices remains unproven.

Two further uninterrupted normal controls measure 16.855755 and 16.861892 ms/ref,
with all strict guards passing and no explicit FastInterp tuning overrides.
The repeatable shared-default result is therefore about 99%, with the final GPU
drain/capture costs retained. The fresh diagnostic shows 1.392 ms/ref of queue
pressure waits and zero RAM texture cache misses in that interval; its enabled
profiling overhead excludes it from clean speed acceptance.

On this same build, Cooking Mama 5 passes a 600-refresh boot smoke. Driver stops
at unsupported D24 framebuffer admission, Ever Oasis at a display transfer, and
Monster Hunter Stories at color-aliased texture sampling. These are recorded
compatibility failures, not full-game passes. Six production-source iOS SDK
syntax checks and 46 profiler/build-policy helper tests pass; physical iOS
performance and a complete iOS link remain unverified.

The final v31 build keeps the writer-completion experiment off after a null
OFF/ON/ON/OFF comparison. Its uninterrupted normal check is 16.902863 ms/ref,
14.949752 thread CPU ms/ref and 99.20338% weighted speed, with exact original
pixels/CPU and a 21.140666 ms refresh p99. Other-ROM results and six iOS syntax
checks were repeated with the same outcomes. A separate three-second CPU trace
preserves individual instruction PCs: all 3,083 running main-thread samples
were on performance cores. It is restored, instrumented diagnostic evidence,
not a clean normal-play throughput result.

The [history correction and current evidence](metal-fastinterp-parity-2026-11-20.md#v30-shared-42-ordinary-default-acceptance)
retain run identities and limits. This is one local M3 Pro window approaching
the 60 Hz target, not sustained full-game 100% or M1/M4/iPad/iPhone acceptance.

The [current parity investigation](metal-fastinterp-parity-2026-11-20.md) maps
the retained GLES findings to native Metal, records the local M3 Pro title-window
experiments, and separates accepted mechanisms from rejected or noisy changes.
Early title controls measured 58.0/59.6% speed, then 64.2% with 64-draw groups.
A later mansion-window ABBA isolated a texture snapshot-cache miss pattern:
54.662/57.845 ms became 20.324/20.557 ms per refresh, with matching endpoint
pixels and retained CPU state. This mansion window is separate from the requested
Gadd lab conversation. The first fully warmed lab replay measured 19.641 ms per
refresh (84.95% steady speed, p99 24.560 ms); subsequent controls drifted, so
candidate changes require repeated matched controls. These are local M3 Pro
results, not full-speed gameplay or iPad acceptance. The ongoing target remains
100% speed at 100% guest clock without JIT or reduced work. The parity document
records checkpoint equivalence limits, cache warmth and detailed run identities.

## Latest M1 development result: Luigi GPU residency, 2026-11-16

On an Apple M1 Mac using native OpenGL and FastInterp, the existing GPU RGBA8
alias remapper removed a recurring tiled-to-linear readback/upload fallback in
one Luigi’s Mansion: Dark Moon checkpoint. Both arms used 100% guest CPU,
100% speed cap, sound, normal graphics checks, no CPU/shader JIT, unchanged
effects and unchanged guest timing. The remap switch was the only arm difference.

| Same-build run | Remap | Completed-work wall time / refresh | Readbacks / 120 refreshes |
| --- | --- | ---: | ---: |
| A1 | Off | 28.254 ms | 240 |
| B1 | On | 16.650 ms | 0 |
| B2 | On | 16.651 ms | 0 |
| A2 | Off | 28.569 ms | 240 |
| **Mean** | **Off → On** | **28.412 → 16.651 ms** | **240 → 0 in each arm** |

That is **11.761 ms less completed-work time per refresh (41.4%)**. Enabled runs
reach approximately the existing 60 Hz cap; the cap hides any additional
throughput. This is one counterbalanced ABBA, not four independent campaigns or
an uncapped GPU benchmark. It does not establish full-speed performance across
Luigi gameplay, other games, the iPad, or Android.

Final RGBA, both decoded LCD images, selected CPU/VFP/CPSR/timer/HID state and
persistent files matched across all four arms. Timing endpoints include GPU
completion. Main-thread CPU uses snapshot-complete clocks; process/worker CPU
fields include initial snapshot overhead and must not be added together.
Detailed PICA counters were disabled, so this does not prove equality of every
intermediate draw or all guest RAM. Save/load and Zelda opener checks are
separate correctness results. A separately packaged Mac gameplay build has passed normal Luigi startup; these
measurements do not assert a released default or current-main binary identity.

The [development update](updates/2026-11-16.md) records the mechanism, rollout
status and cross-title limits. The [public evidence summary](evidence/m1-native-rgba8-2026-11-16.json)
retains exact metrics, source/binary identity and private report hashes without
shipping game memory or screenshots.

## Historical M4 iPad whole-app comparison

Xenoblade Chronicles 3D's opening sequence on an M4 iPad, at native resolution
with sound enabled and JIT disabled. All runs used **100% guest CPU clock**.

| Build | Emulation speed | Peak sampled memory |
|---|---:|---:|
| **Cara GLES3** | **99.5%** | **758 MiB** |
| Manic / Azahar | 97.4% | 1,373 MiB |
| Azahar / MoltenVK | 84.6% | 1,261 MiB |
| Manic / Citra (frozen scene) | 82.1%† | 1,302 MiB |

† Manic / Citra's scene froze while its counter continued. Its figure does not
represent a completed, playable replay.

The first three builds completed the opening replay. Cara used about 40–45% less
sampled memory than the two Azahar-based builds in this test. These are whole-app
results, including differences in frontend and presentation behavior. Cara was
measured at checkpoint `a9e0658`, before save-state support.

## Interpreting the results

Emulation speed measures progress relative to the emulated system’s normal speed;
it is not a rendered game frame-rate measurement. Peak sampled memory is the
largest observed sample, not a continuous measurement of every allocation.
These results compare whole apps and do not isolate a single renderer or CPU
optimization. They are historical measurements, not a guarantee for current
builds or other games.

See [validation](validation.md) for additional tests, measurement constraints,
and optimization work.


The [November 17 M1 integration](m1-performance-integration-2026-11-17.md) records the checked-remap result, source-cache limits, closed shader-sharing comparison and default-OFF research policy.

## Gadd normal-boot refresh tail, 2026-11-21

The Metal ROM compatibility changes landed in main at `3ff9d63`. The following
performance replays use the local FastInterp-only developer build with the
existing Apple performance policy. Each run boots Dark Moon normally from the
same in-game save, replays inputs at refreshes 720, 960 and 1200, and measures
refreshes 3121 through 3600 at the 100% guest clock and speed cap. Metal uses
the same prewarmed pipeline cache across arms. Timing is measured at RunLoop
refresh boundaries.

| Backend and policy | Cadence speed | Refresh p99 | Worst rolling 120 mean |
| --- | ---: | ---: | ---: |
| JIT OpenGL, fresh reference | 99.98% | 19.630 ms | 16.761 ms |
| FastInterp Metal, 8 slots, queue trace | 93.16% | 22.670 ms | 18.400 ms |
| FastInterp Metal, 32 slots and uniform reuse, queued depth disabled | 94.92% | 23.441 ms | 17.856 ms |
| FastInterp Metal, 32 slots, queued depth, uniform reuse, cached-input retirement at Record, A | 99.99% | 17.815 ms | 16.744 ms |
| FastInterp Metal, same policy, B | 100.10% | 18.091 ms | 16.791 ms |

The 8-slot trace recorded 927 submission waits. In its worst 120-refresh
window, the emulation thread used 17.249 ms of CPU time per refresh and the
oldest-command waits averaged 1.163 ms. The longest individual wait in that
window was 2.810 ms behind a transfer with a 0.024 ms GPU span. A trace with
32 slots, queued depth and uniform reuse recorded zero oldest-command waits.
Its worst rolling window still used 17.004 ms of thread CPU per refresh. This
identifies the CPU budget as the remaining constraint in the observed trace.
The 32-slot control with queued depth disabled demonstrates why capacity alone
is insufficient. These observations describe the captured scene and host.

Use `CARA_METAL_SUBMISSION_WINDOW=32`, `CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT=1`,
`CARA_METAL_REUSE_UNIFORM_UPLOADS=1`, and
`CARA_METAL_RETIRE_CACHED_INPUTS_AT_RECORD=1` with the local Apple defaults
when reproducing the two tuned replays. The direct load/compare/branch option
remained disabled. Endpoint Metal pixels and guest boundary digests matched
across all measured arms. The JIT OpenGL replay matched its own retained
reference. The worst rolling windows were 99.54% and 99.26% of the 60 Hz
budget, so the result is near the cap with residual variation. These options
remain opt-in. Local commands and raw measurements are retained in
`work/gadd-compat-tail-2026-11-21`.

An attachment-validation reuse experiment checked identical resource identities,
generations and guest intervals within a draw group. All eight GPU fixture
configurations and exact lab endpoint checks passed. The two candidate p99
results were 18.623 and 19.294 ms against 18.573 ms for the same-binary control.
The experiment was removed after the timing comparison.

### Indexed ZIP reads in the Gadd tail

A separate diagnostic run recorded cumulative archive reads and Deflate
restarts at each refresh. The 480-refresh Gadd window requested six 64 KiB
reads. Four restarted from an index checkpoint and decoded 2,787,136 bytes
before reaching the requested data. The six reads took 6.924 ms total.
Refreshes 3238 and 3528 each spent roughly 2.22 ms inside the archive reader.
Four read-bearing refreshes fell within the slowest 10%. Index construction
finished before the game booted, and both the raw and the ZIP replays
recorded zero oldest-command submission waits during the lab interval.

The diagnostic p99 was 19.057 ms for ZIP and 18.738 ms for raw playback.
Removing measured ZIP read durations from the affected timestamps as an
arithmetic sensitivity check changes the ZIP p99 to 18.939 ms. The original
larger gap varied across boots. Host focus and thread CPU timings also varied.
The original archive runs, refreshed diagnostic comparison, and per-refresh
archive/CPU/queue receipts are in `work/zip-rom-index-2026-11-21`.
