# Performance measurements

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
