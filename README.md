# Cara

Cara is a work-in-progress Nintendo 3DS emulator for iOS. It combines Citra MMJ's
rendering path with Azahar's FastInterp CPU interpreter, with the goal of smooth
play without JIT.

## Assumptions

- MMJ's GLES3 renderer may offer a lighter path for mobile hardware. A comparison
  with Azahar under the same conditions should guide whether we keep it.
- The CPU interpreter and renderer can evolve independently. Improvements to one
  should not require replacing the other.
- The emulator core should be independent of the app's interface, file browser,
  controller layout, and cloud storage.

## Goals

- Consistent gameplay speed, correct graphics, and smooth audio without JIT.
- Predictable memory use and sustained performance on iPhone and iPad.
- A reusable core that can power both Cara and a Delta integration.
- Reliable game saves and save states.
- A straightforward build with bundled source and clear upstream provenance.

## Benchmarks

### Cara GLES3 and Azahar/MoltenVK on M4 iPad

Both completed the same 9,000-refresh Xenoblade opening replay at native render
resolution, 100% guest CPU clock, with sound enabled and scripted A presses at
refreshes 900 and 1500. Both used FastInterp with CPU and host shader JIT off.
The same ROM was selected through Files, with separate save fixtures and app containers.
Cara measurements use checkpoint `a9e0658`, before save-state support.

| Measurement | Cara GLES3 | Azahar / MoltenVK |
|---|---:|---:|
| Emulation speed | 99.5% | 84.6% |
| Replay duration | 151.24 s | 177.82 s |
| Peak sampled physical footprint | 758 MiB | 1261 MiB |
| Requested audio frames not supplied | 0.66% | 15.52% |

These are whole-app results for this replay. The MoltenVK test host exposed only
FIFO presentation and spent substantial time waiting on presentation. The builds
also used different frontends and compiler settings, so this does not isolate the
cost of GLES3 versus Metal. Memory was sampled every 300 refreshes. Audio counts
include startup and recovery buffering; output implementations differed.

### Manic with Azahar on M4 iPad

Manic 2.0.2 completed a 9,000-refresh Xenoblade opening replay in 154.42 seconds
at 97.4% average emulation speed. It used its bundled Azahar core with FastInterp,
100% guest CPU clock, native render resolution, sound enabled, and CPU and host
shader JIT disabled. Scripted A presses occurred at refreshes 900 and 1500.

| Measurement | Cara GLES3 | Manic / Azahar |
|---|---:|---:|
| Emulation speed | 99.5% | 97.4% |
| Replay duration | 151.24 s | 154.42 s |
| Peak sampled physical footprint | 758 MiB | 1373 MiB |

Manic's sound was initially reported clean, with crackling later in the replay.
Two late 300-refresh windows ran at 76.4% and 70.1% speed. Its audio instrumentation
counted frames submitted to the frontend, so no comparable output-underrun
percentage is available. This was a privately re-signed release with compiled
timing and input instrumentation; its renderer and audio frameworks were retained.
The apps used separate save data and different Azahar revisions. These results
describe this opening replay, rather than sustained gameplay or a renderer-only
comparison.

### Manic's Citra core on M4 iPad

The same opening test was also attempted with Manic's Citra core at 100% guest CPU
clock and at its default no-JIT Compatibility preset of 20%. Both used native
resolution, New 3DS mode, sound enabled, and CPU and host shader JIT disabled.
Recorded boot settings differed only in guest CPU clock.

| Guest CPU clock | Recorded refreshes | Recorded duration | Guest refresh throughput | Peak sampled footprint | Observed result |
|---|---:|---:|---:|---:|---|
| 100% | 9,000 | 183.11 s | 82.1% | 1302 MiB | Cutscene froze while guest refresh ticks continued |
| 20%, Compatibility | 7,800 | 130.77 s | 99.7% | 1331 MiB | Scene moved normally with occasional audio cuts, then the app crashed |

Neither run completed a visually valid replay. The 100% figure measures counter
throughput despite the frozen scene. The 20% figures describe its last recorded
checkpoint, rather than the exact crash time or a completed 9,000-refresh run.
Its crash report records an invalid memory access in the bundled MoltenVK renderer
on the Vulkan worker thread. The second run retained the first run's disk shader
cache. Guest refresh throughput at an underclocked CPU setting does not establish
equivalent game frame rate or timing. Native Citra audio output was not instrumented.

## Attribution

Cara builds on the work of the [Citra Emulator Project](https://github.com/citra-emu),
[Citra MMJ](https://github.com/weihuoya/citra), and
[Azahar](https://github.com/azahar-emu/azahar). FastInterp was contributed upstream
by [warmenhoven](https://github.com/warmenhoven).

Upstream copyright notices and licenses are preserved. See [LICENSE](LICENSE),
[NOTICE](NOTICE), [source revisions](emulator/VENDORED_SOURCE.json), and
[FastInterp provenance](docs/fastinterp-upstream.json). Third-party libraries retain
their respective licenses.
