# Cara

[![Build](https://github.com/dmikey/cara/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/dmikey/cara/actions/workflows/build.yml?query=branch%3Amain)
[![License: GPL v2 or later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)

<img src="docs/artwork/cara-citrus-readme.png" alt="Cara citrus app icon" width="104" align="right">

**Apple-first Nintendo 3DS emulator core and emulator shell.**

Cara is a Nintendo 3DS emulator designed around Apple hardware, with a portable
core and frontends for macOS, iOS, Android, and Linux. On iPhone and iPad,
FastInterp runs games without CPU JIT. Reuse in Delta is a development goal.

Cara aims to improve speed while preserving emulation correctness and
stability. Guest behavior guides timing and compatibility decisions, and
measurements on real hardware guide host-side choices. We prioritize engine
fixes and check their effects on gameplay, rendering, and saves.

## Made for iPad

- **A library built for the big screen.** Browse your collection, favorites,
  and recently played games from an iPad sidebar. Add games through Files.
- **Play your way.** Use touch controls, customize a Delta-compatible skin, or
  connect a supported game controller.
- **Pick up where you left off.** Make per-game save states and sync in-game
  saves through a selected iCloud Drive folder.
- **Per-game play settings.** Choose Accuracy for a fixed 100% guest CPU clock
  and game speed, or Performance for adaptive tuning.

**3DS activity** in Library & storage lets games receive Play Coins from
recorded steps and exchange eligible StreetPass messages through Files or
an opt-in nearby Cara library. [Setup and compatibility](docs/3ds-system-activity.md)

### What Performance Mode does on iPad

- **Adaptive guest CPU.** Cara adjusts the emulated 3DS CPU clock under load
  to help keep gameplay near the speed limit. Turn on **Use fixed guest clock**
  if you want to set the clock yourself.
- **Adaptive Frame Skip.** Cara can combine display updates when it falls
  behind, while still running every 3DS draw and transfer.
- **Less waiting for shaders.** **Options → Display → Async Shaders** is on by
  default in Performance Mode. Compatible draws can use a general shader while
  Metal builds a specialized pipeline. Eligible vertex draws can keep going
  through CPU interpretation. Metal and GLES also reuse fragment shaders when
  only unused combiner-buffer writes differ. First visits can still hitch.

Cara models GPU busy flags and completion interrupts in 3DS guest time so
games receive those signals at the expected points.

**Measured on an M4 iPad Pro.** In the same normal-boot *Luigi’s Mansion:
Dark Moon* Gadd’s lab replay, Metal and GLES both held **100% emulation
speed**. Metal cut the 99th-percentile time between refreshes from
**20.51 ms to 17.15 ms**, about **16% shorter**. At the mansion gate, Async
Shaders cut one first-visit stall from **611 ms to 46 ms** in the Metal
replay. The first refresh and other cold stalls remained. See the
[Gadd’s lab comparison](docs/native-metal-build.md#adaptive-metal-submissions)
and [mansion gate frame times](docs/native-metal-build.md#mansion-gate-with-performance-mode-defaults).

## Play compressed cartridge ROMs

Open a standard ZIP containing one `.3ds` cartridge image from the macOS
game picker or Cara’s Files picker on iPad. Cara reads the original archive in
place. Stored entries are read directly. For Deflate, Cara builds an
in-memory seek index at first open and decompresses data on demand. See the
[ZIP cartridge guide](docs/native-mac-dev.md#zip-cartridge-images).

Cara opens a 7z archive containing one regular `.3ds` or `.cci` file. At launch,
it expands the cartridge to private temporary storage and shares it across
ROM readers until the game closes. Allow free space for the uncompressed
image. Password-protected and multi-cartridge archives are unsupported.

Cara also opens Azahar v1 cartridge images with the `.z3ds` or `.zcci` extension.
The shared ROM reader uses
the embedded Zstandard seek table to read individual frames on demand. Both
Apple game pickers and the iOS library accept the original compressed file.
[Azahar format specification](https://github.com/azahar-emu/azahar/pull/1208).

Open a decrypted base-game `.cia` directly in either Apple frontend. Cara reads
the executable and RomFS from the package and uses the same game ID for saves
as the cartridge. Open a matching update CIA while the game is stopped to
install it for subsequent launches. Updates and DLC stay out of the playable
library. System and DLC package installation is unsupported.

**Test status.** Cara played a Deflate ZIP of *Luigi’s Mansion: Dark Moon*
on macOS and on a physical M4 iPad. On the iPad, selecting the original ZIP
through **Add games → Open game from Files…** booted directly to the title
screen. The loader indexed the archive in 1.37 seconds. A before-and-after
check of the app’s storage found no extracted cartridge file. A separate
3,600-refresh FastInterp Metal replay reached Gadd’s lab from a normal boot
with both formats. Lab p99 was 19.60 ms for raw and 19.99 ms for ZIP, and
all three sampled frames matched. A 900-refresh mansion gate replay from
ZIP averaged 59.70 refreshes/s on the M4 iPad.

## Platforms

| Platform | Current frontend |
|---|---|
| **macOS** | Native desktop app and SDL development host. |
| **iOS** | Native touch-first app for iPhone and iPad, without CPU JIT. |
| **Android** | Native Android frontend with OpenGL ES rendering. |
| **Linux** | SDL command-line frontend with desktop OpenGL. |

**Platform stability and feature parity are still in progress.** Game
compatibility, performance, controls, settings and save workflows vary across
platforms. Features described for one frontend may not yet be available in the
others. Our goal is a smooth, consistent experience across all four.

On macOS, open **Cara → Settings** from the launcher or while a game is running.
**Accuracy** locks the guest clock and game speed limit at 100%.
**Performance** can adapt the guest clock under load, or you can set a fixed
guest clock and a separate game speed limit. Adaptive Frame Skip coalesces
host display updates while preserving guest rendering. Under **Display**,
**Lossless 2×** and **Lossless 4×** center the screens at whole pixel multiples.
Play and display settings apply when you save. CPU engine changes apply when
you next open a game. The native Metal runner reads the saved Performance Mode
and Async Shaders preferences.

Cara opts into Apple Game Mode by default on supported devices. It activates
automatically in full-screen play unless you turn it off. The **Game Mode**
entry in Cara's settings on iPhone and iPad, or the **Cara** menu on Mac,
shows where to change it in Apple's Game Overlay or Game menu.

## Graphics

| Backend | Role in Cara |
| --- | --- |
| **Metal** | The native graphics direction for Apple devices. A development renderer is being tested on Apple hardware. |
| **OpenGL ES** | The established hardware-accelerated path on iOS and Android and a reference for renderer compatibility. |
| **Vulkan** | Planned for broader cross-platform support. |

Cara moves vertex processing and packed input decoding onto the GPU, keeps
eligible surface conversions and presentation GPU-resident, and reuses prepared
indices, uploads, and shader stages. Metal development builds also explore
grouping compatible draws, reusing decoded textures and GPU fills, and loading
translated shader caches off the draw thread. Surface ownership tracking
preserves guest-visible data when rendering, copies, and CPU reads intersect.

## From Android performance work to a shared core

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
budgets across chained blocks. In FastInterp mode, guest CPU instructions remain
interpreted without runtime CPU code generation. Optional JIT support depends on
the platform and build.

Cara combines these execution changes with its graphics pipeline, bounded
memory and upload caches, a reusable core, and platform frontends.

## FastInterp and handheld roadmap

FastInterp decodes guest instructions into reusable blocks and dispatches them
to precompiled handlers. Block chaining, predecoded operands, and fast paths
for mapped RAM and ARM condition flags reduce interpreter work. Cara integrates
these changes with guest timing, save states, and memory-coherence checks for
GPU-visible memory.

FastInterp already handles guest ARM and Thumb instructions. The Rockchip
handheld roadmap builds on that support by expanding Thumb correctness tests
and tuning decoding, dispatch, and memory access for low-power ARM Linux
devices. Rockchip devices are a development target.

See the [architecture](docs/technology.md), [FastInterp notes](docs/fastinterp.md),
and [performance and validation results](docs/performance.md) for implementation
details and benchmark methodology.

## Open source, built in the open

Cara is licensed under **GPL-2.0-or-later**. Emulator and dependency source is
bundled in the repository, with build tools and validation guides included.

[Build Cara](docs/build.md) · [Performance](docs/performance.md) ·
[Architecture](docs/technology.md) · [Save states](docs/save-states.md) ·
[Validation](docs/validation.md)

## Build and contribute

A Linux x86_64 OpenGL developer build is available through the
[Linux OpenGL workflow](https://github.com/dmikey/cara/actions/workflows/linux-opengl.yml).
It uses a command-line SDL frontend and FastInterp without JIT. See the
[Linux build and runtime requirements](docs/build.md#linux-opengl).

The [native Metal build guide](docs/native-metal-build.md) covers opt-in
development builds for Apple Silicon Macs and physical iOS devices.

Start with the [build guide](docs/build.md). Reports should include the Cara
revision, device, operating system and version, game region, settings and steps to reproduce.
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
