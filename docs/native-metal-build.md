# Native Metal developer builds

The Apple Silicon macOS runner and the physical-device iOS app build the same
native Metal renderer. These opt-in commands leave the normal OpenGL Mac and
GLES iOS build commands available.

Use full Xcode with the Metal tools selected, Python 3.9 or newer, Git, CMake
3.31.10 and Ninja 1.13.2. For Mac builds, install SDL2 development files and
libpng, for example with `brew install sdl2 libpng`. Use `--prefix-path` for
an alternate installation prefix.

```sh
python3 tools/build_native_metal.py mac --cpu-jit
python3 tools/build_native_metal.py ios
```

The Mac command includes the optional ARM CPU JIT, with FastInterp still
available. Omit the flag for a FastInterp-only build. Both commands compile
the native Metal renderer and the same PICA shader resources. The Mac output
is a locally ad-hoc-signed SDL developer runner with dynamic SDL dependencies
and the iOS output is an app without provisioning for installation. The audit
records the architecture inputs, native Metal build inputs, compiler identities,
bundled shader resources and linked JIT policy. CI verifies compilation and
the Metal transfer planner. It does not run an emulator scene.

To run the Mac compatibility frontend with a local ROM and isolated app data,
pass an absolute user directory, the ROM and a refresh limit:

```sh
mkdir -p "$PWD/work/metal-profile"
CARA_USER_DIR="$PWD/work/metal-profile" CARA_CPU_BACKEND=jit \
  work/build-native-metal-mac/bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom \
  "/absolute/path/to/game.3ds" 2400
```

Use `CARA_CPU_BACKEND=fastinterp` for the interpreter. The iOS build includes
FastInterp and retains its GLES option. The existing release and signing
workflows continue to package their current configurations.

## Runtime shader compiler sources

The build helper fetches two pinned Khronos source revisions as needed.
`--shader-sources PATH` reuses clean offline checkouts named `glslang` and
`spirv-cross` inside that directory. Both dependencies are built as static
libraries for the selected SDK and translate shaders at runtime on each target.

- glslang 16.6.0, commit
  [e1b562a](https://github.com/KhronosGroup/glslang/tree/e1b562a8bed273a02f30b59b66a5d499793cede5).
- SPIRV-Cross Vulkan SDK 1.4.350.0, commit
  [1a61695](https://github.com/KhronosGroup/SPIRV-Cross/tree/1a6169566c73d3da552748fc372fe2bbb856e46e).

No per-title shader capture, ROM contents, prepared Gate catalog, private
signing identity or device provisioning is required to compile. The build
includes the unmodified upstream license texts and notices in the app's
`Licenses` directory. Preserve the included Khronos notices and the emulator's
corresponding-source material when packaging these developer builds. glslang's
notices cover several licenses and SPIRV-Cross includes the Apache 2.0 license
and a separate `LICENSES` directory for components.

## ROM compatibility

For ROM compatibility, boot with a fresh user directory and include scripted
confirmation inputs when the game waits on first-run dialogs. Check that the
runner presents the full refresh interval and inspect the captured screen.
Keep compatibility runs separate from timed scene replays.

The shared Metal renderer handles Ever Oasis's packed RGB565 horizontal crop
and its tiled two-axis reductions in RGB565 and RGBA8.
Driver Renegade uses a three-byte tiled D24 surface and a packed-format cube
map. D24 uses the host combined depth/stencil attachment with guest
stencil disabled, and its import and export preserve the three-byte layout.
The cube importer decodes each face with the guest texture format before
assembling the native RGBA8 cube. Cooking Mama 5 uses transparent border
sampling, which maps to Metal's zero border for decoded RGBA8 textures.
Monster Hunter Stories uses both render-target feedback and procedural
texturing. For the supported primary RGBA8 feedback case, publish the previous
draws and decode the authoritative guest bytes into a separate sampled
texture before drawing. The Metal shader uses the shared PICA procedural
texture generator and takes its lookup tables from the native LUT atlas.

## iPad Metal timing

Run a paced FastInterp benchmark with `CARA_FRAME_TIMING=1` and
`CARA_HOST_CPU_TIMING=1` to compare elapsed refresh time with emulation-thread
CPU time. The iPad app writes `Documents/cara/frame-timing.json` with
per-window p50, p95, p99, and maximum elapsed time. Add
`CARA_METAL_FRAME_COST=1` to record deltas from the shared Metal phase
counters on the renderer's owner thread. Phases can be nested. Compare
throughput again with phase tracing disabled after identifying a hotspot.

For a raw-versus-ZIP comparison, use the same signed app, seed the same
in-game save, boot each cartridge normally, and match the final screen
capture. For Dark Moon, the lab replay uses A presses at refreshes 720,
960, and 1200. Inspect the four 120-refresh windows after refresh 3120
separately from startup and shader compilation.

### M4 iPad lab replay

The same FastInterp build at 100% guest clock reached the Gadd conversation
from a normal boot with both renderers. Over refreshes 3121–3600, GLES averaged
16.71 ms per refresh and Metal averaged 24.00 ms. Both executed the same
guest tick count and 336,000 PICA draw requests across the four windows.
Raw and indexed ZIP cartridges produced byte-identical Metal captures and
the same 24.00 ms lab mean.

A retained local build based on `cce1a67` with additional Apple tuning
completed the same lab window at 19.15 ms per refresh. It matched the main
build's final pixels, guest ticks, and PICA draw requests. Emulation-thread
CPU time fell from 18.03 to 13.61 ms per refresh. The local binary included
unmerged renderer changes and a 42-option Apple policy. That comparison
measures the whole retained build.

On that same local binary, disabling the options absent from the main app
raised the mean to 24.07 ms. Restoring only the queue and ownership options
recovered 19.84 ms. Restoring only the shader and vertex options lowered
emulation-thread CPU time to 16.15 ms but raised the wall time to 26.11 ms.
The matched captures and guest draw counts survived all three comparisons.
Applying the available queue options and a port of color-lease reuse to the
main renderer instead took 28.10 ms. Recovering the old result therefore
requires validating renderer dependencies along with the option values.

With Metal phase tracing, the default eight slots averaged 24.34 ms and
spent 4.87 ms per refresh waiting for the oldest tracked submission. The
worst 120-refresh window had a 28.24 ms p99. The
32-slot trace averaged 27.92 ms. Submission pressure disappeared and
upload-arena waits rose to 2.71 ms per refresh. The eight-slot async
depth variant removed two synchronous depth exports per refresh and
raised submission waits to 11.44 ms per refresh. Its lab mean was 30.05 ms.
All three Metal traces matched the same capture. Keep queue width and
async depth as experimental controls until matched runs show a gain.

## Upstream boundaries

- **Azahar** is the upstream boundary for 3DS execution. Propose PICA,
  renderer, FastInterp, and Dynarmic changes against its core interfaces. Cara
  shares Metal shaders, pipeline and submission code between Mac and iPad.
  An Azahar adapter would map its renderer ownership, PICA state, guest-memory
  coherency, and presentation contracts onto that shared code.
  [Azahar rasterizer interface](https://github.com/azahar-emu/azahar/blob/master/src/video_core/rasterizer_interface.h)
- **Delta and DeltaCore** are the reference for the Apple emulator shell and
  controller skin interoperability. Cara owns its library UI, game lifecycle,
  and 3DS skin extensions. A host adapter should translate control layouts
  and button combinations to ordinary 3DS input events, and carry session,
  video, audio, and save operations between the shell and core.
  [DeltaCore bridge](https://github.com/rileytestut/DeltaCore/blob/main/DeltaCore/Cores/EmulatorBridging.swift)
