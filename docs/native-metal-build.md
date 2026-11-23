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

## FastInterp profiling and configuration identity

The Apple frontends share established Metal residency, batching, raw-index,
pipeline and exact selection-reuse defaults through
`common/apple_metal_policy.h`. The Mac previously defaulted only to queued
transfers; this local integration promotes that common production policy to Mac.
iPad retains its existing eight-page upload arena separately; Mac keeps three
pages unless explicitly overridden. Retained ownership/depth/vertex experiments,
64-draw groups, index-scan experiments, uniform/unique-compute/recent-source and
shader-policy-cache options remain explicit. GLES renderer defaults are untouched.
Explicit launch values, including `0`, survive. FastInterp defaults are likewise
shared through `common/apple_fastinterp_policy.h`, with the existing memory
threshold for the larger decoded cache.

Exact keyed RAM texture snapshots are shared defaults as well; larger snapshots
are selected with at least 6 GiB RAM. They preserve complete-byte checks and
bypass reuse when native GPU ownership overlaps the source. The caches allocate
on demand with bounded payload budgets. This does not imply all FCRAM writes
have reliable page invalidation or that driver memory fits within those budgets.

Shared policy is not evidence of identical measured performance across devices.
Record the exact source revision, local changes, binary hash, effective defaults
and explicit flags. Private experimental device builds and current shared builds
remain distinct. See the [parity investigation](metal-fastinterp-parity-2026-11-20.md)
and [upstream source review](metal-upstream-research-2026-11-20.md). Do not promote
a private experimental gain into a claim about `main`.

For a FastInterp-only Mac build, omit `--cpu-jit`:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 tools/build_native_metal.py mac
```

The developer runner verifies the actual FastInterp core types and 100% guest
clock on every active timer, and writes `metal-performance.jsonl`. Each window
reports emulation speed separately from game FPS. The runner retains its normal
100% speed cap; reaching the cap does not measure additional throughput.

Use a fresh private data directory for each retained run:

```sh
python3 tools/profile_native_metal.py \
  --rom '/absolute/path/to/game.3ds' \
  --output work/metal-fastinterp-baseline --frames 7200 --warmup-frames 1200
```

The default expected title is Luigi Dark Moon USA; use `--expected-title` for
another game. `--inputs PATH` supplies an existing frame-based input script.
The helper uses a fixed RTC for reproducibility, clears inherited Cara options,
records the binary hash/explicit renderer overrides, and never enables CPU JIT.
It separates retained startup windows from the later-window summary. A fresh
application directory does not establish a cold Apple driver cache.

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

`--instrumented` enables the existing detailed Metal cost counters and takes a
five-second host stack sample after the warmup boundary. Treat that pass as a
diagnostic, not a clean speed comparison. `--metal-option CARA_METAL_NAME=VALUE`
records an explicit renderer experiment; it does not promote an option to the
Mac's supported default policy. Use matching inputs/checkpoints and compare
rendered output and guest state before accepting an optimization.

Metal phase spans can nest and command GPU spans can overlap. Their totals are
not exclusive CPU/GPU percentages. Stack samples identify wall-time residency,
including GPU/driver waits; an interpreter ancestor in a stack does not establish
that interpreter arithmetic is the bottleneck. Final captures wait for retained
GPU work, while the periodic emulation-speed windows measure guest progress.

For matched-scene experiments, `--save-state-at N` retains a normal guarded
checkpoint and an exact task-owned `checkpoint-filesystem` snapshot. Restore
using `--state CHECKPOINT --state-filesystem SNAPSHOT`; input/frame numbers then
start at zero after restore. Keep the same binary/fingerprint and verify restored
continuation against normal continuation before accepting scene comparisons.
Saving clears renderer page-watch/ownership history. For uninterrupted-play
measurements, use `--save-state-at-end` instead: it creates the same guarded
checkpoint/filesystem snapshot **after** elapsed, CPU, pixel and diagnostic
boundary reports. The two save modes are mutually exclusive. A restored seed
still rebuilds renderer history; it is not evidence of normal-play throughput.
`--compare-to RUN` requires state, filesystem, input and binary identities as well
as endpoint pixels and the retained guest boundary. `--shader-option` accepts only
the existing quaternion-slice and packed-fetch-reuse generator experiments.
Buffered refresh CSVs report VBlank/RunLoop boundaries, not physical scanout;
their percentiles supplement the averaged emulation-speed windows.

For steady-state pipeline measurements, collect a bounded learned cache during
normal execution, then prepare its recipes before the measured replay:

```sh
python3 tools/profile_native_metal.py \
  --rom '/absolute/path/to/game.3ds' --output work/metal-warm-replay \
  --frames 600 --warmup-frames 120 \
  --state work/metal-scene/checkpoint.carastate \
  --state-filesystem work/metal-scene/checkpoint-filesystem \
  --pipeline-cache-from work/metal-scene/metal-pipelines \
  --metal-option CARA_METAL_PIPELINE_LEARNING=1 \
  --metal-option CARA_METAL_PREWARM_ALL=1
```

Normal Mac/iOS play shares pipeline-learning policy; diagnostic profiling defaults
to quiet learning-off unless explicitly selected. The helper copies only bounded
regular learned-cache artifacts into its fresh output and records their hashes.
The cache is device/generator/build scoped; incompatible artifacts fall back to
normal compilation. Full prewarm is explicit and may add startup time. Require
ready/attempted coverage and runtime-miss logs, rather than assuming a cache
folder means the measured scene is warm. No strict replay or missing-draw
suppression is used. Keep detailed GPU-stage timestamps in separate diagnostic
runs via `--metal-option CARA_METAL_DRAW_STAGE_PROFILE=1 --instrumented`.

## Controlled CPU and compiler comparisons

`--lto` enables ThinLTO for Release CXX, Objective-C++ and executable linking.
The build audit checks all three; CMake IPO alone is insufficient for the Metal
Objective-C++ bridge. Omitting `--lto` restores the helper-managed Objective-C++
flags without removing unrelated custom flags. No device-specific ISA target is
required. Use the same compiler mode on both CPU backends when comparing them.

The profiler defaults to FastInterp. `--cpu-backend jit` is a macOS diagnostic
and requires a binary-matched audit from a `--cpu-jit` build. It does not change
iOS interpreter policy. Backend/build compatibility checks remain active when
loading checkpoints; create a normal scripted boot after an incompatible build.
Use each backend's verified scene reference rather than weakening pixel checks
for a cross-backend image difference.

`metal-measured-interval.json` reports elapsed time plus emulation-thread and
whole-process CPU time when the host clock is available. Thread CPU includes
renderer calls on that thread. `cpu-execution-warmup.json` and
`cpu-execution.json` retain actual guest model, clock scales and available CPU
diagnostics. Four allocated CPU objects do not prove that the guest model is
New3DS. Timing tables can also differ between CPU backends even at100% clock;
retain work counters and qualify comparisons accordingly.

Profiler filesystem/checkpoint/cache copies use independent-inode APFS clones
when available, with a normal copy fallback. They are not hardlinks; source
symlinks and nonregular inputs are rejected. This avoids multiplying physical
checkpoint storage while preserving isolated writes and input manifests.

Current opt-in bridge experiments include
`CARA_METAL_CACHE_LINEAR_TEXTURE_ALIGNMENT=1`,
`CARA_METAL_REUSE_DEFERRED_UNIFORMS=1`, and
`CARA_METAL_VERTEX_READ_PLAN_RELATIVE_BASE=1` (the latter also requires
`CARA_METAL_REUSE_VERTEX_READ_PLAN=1` and
`CARA_METAL_VERTEX_READ_PLAN_CACHE_128=1`). These are not established performance
defaults. Device-specific alignment answers stay queried from Metal, uniform
reuse stays within an immutable command/arena lease, and relative read plans
validate every current address before payload callbacks.

FI-only compiler PGO is a separate private experiment. Keep `--lto` and CPU
backend policy identical on generation/use/control builds:

```sh
python3 tools/build_native_metal.py mac --cpu-jit --lto --fastinterp-pgo-generate
# Run representative local gameplay through profile_native_metal.py.
# It isolates LLVM_PROFILE_FILE and marks generation reports instrumented.
# Merge its fastinterp-*.profraw with the configured compiler's llvm-profdata.
python3 tools/build_native_metal.py mac --cpu-jit --lto \
  --fastinterp-pgo-use /absolute/path/to/fastinterp.profdata
```

Only `fastinterp.cpp` is instrumented/optimized from this profile; the Metal
bridge keeps its existing compiler flags. Generation links the compiler's
profiling runtime into consumers of the instrumented core. Generation and use
are mutually exclusive and cannot be combined with whole-engine PGO. Profile
mismatch diagnostics are errors; the audit retains mode, compiler, profile path
and SHA. Omitting both flags clears this helper-managed experiment. Generation
performance is not a clean benchmark, and a profile based on one scene does not
establish a benefit or correctness across games/devices. Inspect actual branch
metadata and test held-out gameplay before promotion. Keep local ROM-derived
profiles, checkpoints and captured guest data outside version control.
