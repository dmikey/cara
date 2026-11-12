# Cara

[![Build](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dmikey/cara/actions/workflows/build.yml?query=branch%3Amain)
[![Tests](https://img.shields.io/github/actions/workflow/status/dmikey/cara/build.yml?branch=main&label=tests)](https://github.com/dmikey/cara/actions/workflows/build.yml?query=branch%3Amain)

<img src="emulator/src/ios/artwork/cara-orange.png" alt="Cara Cara orange icon" width="104" align="right">

Cara is a new, open-source Nintendo 3DS emulator core and emulator shell focused
on **Apple hardware, performance, and memory efficiency**. Built for iPhone and iPad, it combines a
no-CPU-JIT execution path with custom GPU rendering enhancements and its own
native app shell.

Cara builds on Citra MMJ and Azahar's FastInterp, with Apple-focused work on how
the CPU, GPU, and memory interact. The goal is practical gameplay performance,
correct graphics, and a reusable 3DS core designed for Delta compatibility.
Development is active; compatibility and performance vary by game.

## What makes Cara different

Cara builds on **Citra MMJ**, weihuoya's performance-focused Android fork.
MMJ brought a mobile-first approach to 3DS rendering. Cara carries that foundation
forward for Apple hardware with its own GPU pipeline, memory optimizations, and
native presentation.

| MMJ's mobile foundation | Cara's Apple-focused enhancements |
|---|---|
| **Mobile GPU rendering** | A custom Apple vertex pipeline builds on MMJ's OpenGL ES renderer, putting more graphics work on the GPU. |
| **Shader generation and reuse** | MMJ's shader foundation powers Cara's custom Apple GPU shaders, moving vertex processing and packed-data decoding off the CPU. |
| **Mobile performance tuning** | Cara extends that performance focus with prepared-input reuse, bounded GPU caches, and fewer CPU/GPU transfers. |

- **Azahar FastInterp.** Cara adapts Azahar's ARM interpreter for execution without
  CPU JIT. Guest instructions become decoded data dispatched to precompiled
  handlers in the signed app. Cara adds integration with its memory, scheduling,
  and save-state systems.
- **Custom rendering enhancements.** Cara's GLES3 pipeline moves PICA vertex work
  and packed vertex decoding onto the GPU, reuses prepared input data, and reduces
  repeated uploads and CPU/GPU transfers. Direct GPU presentation avoids copying
  every displayed frame through CPU memory. Rendering correctness and bounded
  cache memory are part of the performance work.

## A reusable core and a native shell

Cara is developed as a reusable 3DS core for integration into Delta, alongside
its own standalone iOS shell. The shell provides game loading from Files, touch
controls, customizable Delta-compatible skins, audio, save states, and iCloud
save-folder support. Delta integration is a development target; the standalone
Cara app is the playable build provided here.

## Features

- Open games directly from Files.
- GPU-accelerated graphics and native stereo audio.
- Touch controls and customizable controller skins.
- Save states with per-game slots.
- Sync in-game saves through a selected iCloud Drive folder.
- Optional adaptive guest CPU clock for demanding scenes.

## Benchmarks

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

## Build

Requires macOS, Xcode, Python 3, CMake 3.31.10, and Ninja 1.13.2.
Emulator and dependency source is bundled in the repository.

```sh
python3 tools/prepare.py
python3 tools/build.py ios --renderer gles3
python3 tools/audit_no_jit.py ios --renderer gles3
```

The unsigned app is written to `work/build-ios-gles3/bin/cara-ios.app`.
Device installation requires development signing.

See the guides for [save states](docs/save-states.md),
[adaptive CPU clock](docs/adaptive-clock.md), and [validation](docs/validation.md).

## Attribution

- [Citra Emulator Project](https://github.com/citra-emu) and its contributors —
  the original emulator and core emulation work.
- [Citra MMJ](https://github.com/weihuoya/citra), maintained by
  [weihuoya](https://github.com/weihuoya) — the fork that forms Cara's source foundation.
- [Azahar](https://github.com/azahar-emu/azahar) and its contributors — FastInterp
  and adapted core improvements. [warmenhoven](https://github.com/warmenhoven)
  contributed [FastInterp](https://github.com/azahar-emu/azahar/pull/2376).

Save-state support also builds on original Citra work by Hamish Milne and other
contributors, restored from MMJ and adapted for Cara.

Upstream copyright notices and licenses are preserved. See [LICENSE](LICENSE),
[NOTICE](NOTICE), [source revisions](emulator/VENDORED_SOURCE.json), and
[FastInterp provenance](docs/fastinterp-upstream.json). Third-party libraries retain
their respective licenses.
