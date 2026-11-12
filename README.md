# Cara

[![Build](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dmikey/cara/actions/workflows/build.yml?query=branch%3Amain)
[![License: GPL v2 or later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)

<img src="emulator/src/ios/artwork/cara-orange.png" alt="Cara Cara orange icon" width="104" align="right">

**Nintendo 3DS emulator core and emulator shell. Built for Apple hardware.**

Cara brings a performance-first approach to 3DS emulation on iPhone and iPad:
custom GPU rendering, efficient memory use, and gameplay without CPU JIT.
Open source, with a native shell of its own and a reusable core designed for Delta
integration.

## Performance you can measure

In a recorded Xenoblade Chronicles 3D opening replay on an M4 iPad, Cara reached
**99.5% emulation speed** using **758 MiB peak sampled memory**—about **40–45% less
memory** than the two Azahar-based apps in the same comparison.

| Build | Emulation speed | Peak sampled memory |
|---|---:|---:|
| **Cara GLES3** | **99.5%** | **758 MiB** |
| Manic / Azahar | 97.4% | 1,373 MiB |
| Azahar / MoltenVK | 84.6% | 1,261 MiB |

All three completed the opening replay at native resolution, with sound enabled,
CPU JIT disabled, and **100% guest CPU clock**. This is a whole-app comparison of
one scene; performance and compatibility vary by game. See the
[full results, build revision, and methodology](docs/performance.md).

## What makes Cara different

| Focus | The Cara approach |
|---|---|
| **Built for Apple hardware** | Custom GPU rendering and native presentation for iPhone and iPad. |
| **More work on the GPU** | GPU vertex processing and packed-data decoding reduce work on the CPU. |
| **Less repeated work** | Prepared-input reuse and fewer CPU/GPU transfers target the costs that slow gameplay down. |
| **Memory-conscious design** | Bounded graphics caches and data reuse keep memory efficiency central to performance tuning. |
| **Play without CPU JIT** | Azahar FastInterp powers the CPU execution path inside the signed app. |
| **A core and a shell** | A reusable core developed for Delta integration, alongside Cara’s standalone iOS app. |

## Proven foundations. Cara enhancements.

**Citra MMJ** brought a mobile-first performance focus to Citra: Android GPU
rendering, shader reuse, and mobile performance tuning. Cara builds on that
foundation with its own Apple-focused rendering enhancements and memory work.
**Azahar FastInterp** supplies the interpreter foundation for execution without
CPU JIT. Cara brings these pieces together with its native shell and ongoing
performance engineering.

Explore the [architecture and upstream foundations](docs/technology.md).

## Made for playing

- **Your games, through Files.** Open extracted game files from the file browser.
- **Your controls.** Touch controls and customizable Delta-compatible skins.
- **Your progress.** Per-game save-state slots and in-game save syncing through a selected iCloud Drive folder.
- **Your experience.** GPU-accelerated graphics, native stereo audio, and an optional adaptive guest CPU clock for demanding scenes.

The standalone Cara app is the playable build provided here. Delta core integration
is a development target.

## Open source, built in the open

Cara is licensed under **GPL-2.0-or-later**. Emulator and dependency source is
bundled in the repository, with build tools and validation guides included.

[Build Cara](docs/build.md) · [Performance](docs/performance.md) ·
[Architecture](docs/technology.md) · [Save states](docs/save-states.md) ·
[Validation](docs/validation.md)

## Credits

Cara builds on the work of the [Citra Emulator Project](https://github.com/citra-emu),
[weihuoya’s Citra MMJ](https://github.com/weihuoya/citra), and
[Azahar](https://github.com/azahar-emu/azahar), including
[warmenhoven’s FastInterp](https://github.com/azahar-emu/azahar/pull/2376).
Save-state support builds on original Citra work by Hamish Milne and other contributors.

Upstream copyright notices and third-party licenses are preserved. See
[NOTICE](NOTICE), [LICENSE](LICENSE), and [source provenance](emulator/VENDORED_SOURCE.json).
