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

## Attribution

Cara builds on the work of the [Citra Emulator Project](https://github.com/citra-emu),
[Citra MMJ](https://github.com/weihuoya/citra), and
[Azahar](https://github.com/azahar-emu/azahar). FastInterp was contributed upstream
by [warmenhoven](https://github.com/warmenhoven).

Upstream copyright notices and licenses are preserved. See [LICENSE](LICENSE),
[NOTICE](NOTICE), [source revisions](emulator/VENDORED_SOURCE.json), and
[FastInterp provenance](docs/fastinterp-upstream.json). Third-party libraries retain
their respective licenses.
