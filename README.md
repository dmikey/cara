# Cara

[![Build](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dmikey/cara/actions/workflows/build.yml?query=branch%3Amain)
[![Tests](https://img.shields.io/github/actions/workflow/status/dmikey/cara/build.yml?branch=main&label=tests)](https://github.com/dmikey/cara/actions/workflows/build.yml?query=branch%3Amain)

<img src="emulator/src/ios/artwork/cara-orange.png" alt="Cara Cara orange icon" width="104" align="right">

Cara is an experimental Nintendo 3DS emulator built for iOS play without CPU JIT.
Its custom graphics path moves more work onto the GPU and cuts repeated data
transfers to improve gameplay speed.

It combines Citra MMJ with Azahar's FastInterp CPU interpreter and a custom GLES3
renderer, with a focus on correct graphics and lower memory use on iPhone and
iPad. Compatibility and performance vary by game.

We didn't set out to create another 3DS emulator. Our goal was to contribute the
most complete portable 3DS core we could for reuse in Delta. Along the way, we
found that the graphics pipeline still needed substantial work. Improving it
became part of that same goal: a reusable core with correct graphics and practical
performance on iOS.

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
