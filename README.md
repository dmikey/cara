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

Xenoblade Chronicles 3D's opening sequence on an M4 iPad, at native resolution
with sound enabled and no JIT. A speed of 100% means normal game pacing.

| Build | Average speed | Peak sampled memory |
|---|---:|---:|
| **Cara GLES3** | **99.5%** | **758 MiB** |
| Manic / Azahar | 97.4% | 1,373 MiB |
| Azahar / MoltenVK | 84.6% | 1,261 MiB |
| Manic / Citra, full CPU speed | 82.1% (frozen scene) | 1,302 MiB |
| Manic / Citra, default 20% CPU speed | 99.7% before crash | 1,331 MiB |

The first three builds completed the opening replay. Neither Manic / Citra run
completed it successfully; its frozen-scene counter and partial run are not
measures of playable performance. These compare the apps as tested, rather than
isolating their graphics engines. Cara was measured at checkpoint `a9e0658`,
before save-state support.

## Attribution

Cara builds on the work of the [Citra Emulator Project](https://github.com/citra-emu),
[Citra MMJ](https://github.com/weihuoya/citra), and
[Azahar](https://github.com/azahar-emu/azahar). FastInterp was contributed upstream
by [warmenhoven](https://github.com/warmenhoven).

Upstream copyright notices and licenses are preserved. See [LICENSE](LICENSE),
[NOTICE](NOTICE), [source revisions](emulator/VENDORED_SOURCE.json), and
[FastInterp provenance](docs/fastinterp-upstream.json). Third-party libraries retain
their respective licenses.
