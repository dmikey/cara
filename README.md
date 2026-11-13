# Cara

[![Build](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dmikey/cara/actions/workflows/build.yml?query=branch%3Amain)
[![License: GPL v2 or later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)

<img src="emulator/src/ios/artwork/cara-orange.png" alt="Cara Cara orange icon" width="104" align="right">

**A new Nintendo 3DS emulator core for Apple hardware.**

Our goal is the smoothest 3DS experience on Apple hardware without CPU JIT.
Cara brings a performance-first approach to emulation on iPhone and iPad:
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
| **Play without CPU JIT** | Cara extends Azahar FastInterp with static arithmetic fast paths, decoded-block caching and execution-budget fixes inside the signed app. |
| **A core and a shell** | A reusable core developed for Delta integration, alongside Cara’s standalone iOS app. |

## From Android performance work to Apple hardware

**Citra MMJ**, weihuoya’s performance-focused Android fork, gave Cara a mobile
rendering foundation to build on. We carry that work forward in three key areas:

- **Mobile GPU rendering.** MMJ’s OpenGL ES rendering path is the starting point
  for Cara’s Apple-focused graphics engine. Cara adds custom GPU vertex processing,
  packed-data decoding, and native screen presentation.
- **Shader generation and reuse.** MMJ generates and reuses GPU shaders and programs
  to avoid repeated compilation. Cara adapts that shader foundation for Apple GPUs
  and extends it with its own rendering enhancements.
- **Graphics data reuse.** MMJ’s texture and render-target caching provides the
  foundation for reusing graphics resources. Cara takes that focus further with
  prepared-input reuse, bounded graphics caches, and fewer CPU/GPU transfers.

**Azahar FastInterp** supplies the interpreter foundation. Cara extends it with
static arithmetic fast paths, faster instruction-timing lookup, and an eight-way
decoded-block cache on iOS. Cara also fixes decode-record reuse and execution
budgets across chained blocks. Guest CPU instructions remain interpreted inside
the signed app, without runtime CPU code generation.

Cara combines these execution changes with its Apple graphics pipeline, bounded
memory and upload caches, a reusable core, and its own native shell.

## Going to Vulkan was never the answer

Cara’s progress has come from understanding more of the machine: how the guest
CPU produces graphics commands, who owns each byte of shared memory, when work
becomes visible, and how the host presents it. Changing the graphics API alone
would leave that machinery to solve. Metal remains a future direction; the
current work makes the engine underneath it better.

The GLES3 results above demonstrate what this approach can deliver. They compare
whole applications, so they do not isolate the performance of one graphics API.
The stronger evidence comes from changing one mechanism at a time:

| Finding | What changed or what we learned |
|---|---|
| **Keep graphics data on the GPU** | GPU alias remapping lets compatible surfaces share GPU-resident data while preserving guest-visible bytes. A matched Luigi laboratory replay fell from roughly 24.5 to 16.7 ms per refresh, eliminating six downloads per refresh with matching final pixels. [Results](docs/gpu-surface-aliases.md) |
| **Preserve memory ownership** | Targeted cache cleanup and alias invalidation preserve coherence without flushing unrelated dirty surfaces. Partial writes, overlapping views and CPU reads need explicit correctness checks. [Residency research](docs/gpu-residency-prior-art.md) |
| **Fix host pacing** | Bounded pacing recovered near-full speed in a matched Xenoblade replay while preserving guest event timing, draw counts and final pixels. [Results](docs/frame-pacing.md) |
| **Follow the guest’s work** | Stories traces distinguish command submission, draw work, completion events and display refresh. Offline instruction inspection identified copying, visibility tests and a tick-poll pacing loop; their execution counts alone do not establish CPU cost or justify skipping them. [Trace findings](docs/stories-gpu-trace-2026-11-13.md) |
| **Keep experiments accountable** | Staging only shader-used inputs removed 14% of configured input registers in a Stories comparison, but interleaved data kept payload bytes unchanged and there was no measured speed gain. The option stays off. [Results](docs/vertex-input-staging.md) |

These research results describe the development builds recorded in each report.
The target is full speed at **100% guest CPU with sound and no CPU JIT**. Some
Stories scenes still fall short. Underclocking can help playability, but it also
changes how much work the guest produces; it cannot establish a full-clock gain.
Hardware diagrams and register research guide the investigation, while matched
checkpoints, memory-coherence fixtures and pixel comparisons test each change.

Explore the [architecture](docs/technology.md),
[hardware evidence](docs/hardware-source-audit.md), and
[research references](docs/research-references.md).

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
[Validation](docs/validation.md) · [Experimental Android and IPA artifacts](docs/android-experimental.md)

## Build and contribute

Start with the [build guide](docs/build.md). Reports should include the Cara
revision, device, iOS version, game region, settings and steps to reproduce.
For performance work, include comparable runs and correctness checks.
See [validation](docs/validation.md) and
[existing issues](https://github.com/dmikey/cara/issues).

Game ROMs, Nintendo firmware and keys are not included. Cara is not affiliated
with Nintendo.

## Credits

Cara builds on the work of the [Citra Emulator Project](https://github.com/citra-emu),
[weihuoya’s Citra MMJ](https://github.com/weihuoya/citra), and
[Azahar](https://github.com/azahar-emu/azahar), including
[warmenhoven’s FastInterp](https://github.com/azahar-emu/azahar/pull/2376).
Save-state support builds on original Citra work by Hamish Milne and other contributors.

Development inspection uses [Capstone](https://www.capstone-engine.org/) by Nguyen Anh
Quynh and contributors. Research also draws on fearkov’s
[3dsrecomp](https://github.com/fearkov/3dsrecomp) and
[Zakuro](https://github.com/fearkov/zakuro). See
[inspection tool credits](docs/inspection-tool-attributions.md) for usage and revisions.

Our research also learns from [Dolphin](https://github.com/dolphin-emu/dolphin),
[PPSSPP](https://github.com/hrydgard/ppsspp),
[Panda3DS](https://github.com/wheremyfoodat/Panda3DS),
[Mikage](https://github.com/mikage-emu/mikage-dev), and
[V8’s interpreter work](https://github.com/v8/v8), alongside DMP, 3dbrew,
GBATEK and community hardware tests. The
[research reference guide](docs/research-references.md) credits their contributions
and links the source reviews separately from Cara’s inherited code.

Upstream copyright notices and third-party licenses are preserved. See
[NOTICE](NOTICE), [LICENSE](LICENSE), and [source provenance](emulator/VENDORED_SOURCE.json).
