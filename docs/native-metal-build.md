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
All three Metal traces matched the same capture. The retained renderer
port below rechecks queue width with the recovered dependencies.

### Retained renderer port

The shared renderer now includes the retained index scan cache and shader
identity reuse, vertex and texture lookup reuse, and the related upload,
publication, and checked depth paths. The iPad native Metal frontend
initializes the recovered policy before renderer creation. Explicit
environment settings override it. This port kept the Mac startup policy
while sharing the renderer implementation.

Set `CARA_REFRESH_TIMING=1` to write the elapsed time of every measured refresh
to `Documents/cara/refresh-timing.csv`. Compute the lab p99 over all 480
samples and slide a 120-refresh window one sample at a time to find the
slowest contiguous period. Speed below is relative to the 3DS refresh
cadence of approximately 59.84 Hz.

| M4 iPad, raw cartridge, portrait | Lab speed | Lab p99 | Slowest 120 refreshes | Mean in that window |
| --- | ---: | ---: | --- | ---: |
| FastInterp GLES | 100.01% | 20.42 ms | 3164–3283 | 16.75 ms |
| FastInterp Metal, recovered policy | 89.36% | 22.08 ms | 3328–3447 | 19.24 ms |
| FastInterp Metal, 16 submission slots | 83.73% | 23.72 ms | 3412–3531 | 20.35 ms |
| FastInterp Metal, 32 slots and queued pending depth export | 100.00% | 17.17 ms | 3294–3413 | 16.72 ms |

These runs booted from the same ordinary in-game save and enabled
`CARA_FASTINTERP_ALU_PAIRS=1` for both renderers. The three Metal captures
matched byte for byte. All four runs matched the guest ticks and PICA draw
requests in the four 120-refresh windows. To reproduce the last row, set
`CARA_METAL_SUBMISSION_WINDOW=32` and
`CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT=1`.

### Validated iPad submission policy

The iPad Pro 11-inch M4 tested here (`iPad16,3`) defaults to 32 tracked
submissions and enables queued pending depth export when native Metal is
selected. Explicit overrides still take priority. The queued path consumes
the producer on the same Metal queue and keeps its validation receipt
attached to the import. The adaptive policy below treats 32 as the starting
value on the verified iPad.

In separate traced replays of the lab, the eight-slot policy spent 4.96 ms
per refresh waiting on the oldest tracked draw. The combined policy recorded
zero submission-pressure waits and two queued depth exports per refresh.
The command timeline confirmed a 32-slot queue. Use the untraced replays
for the frame-time comparison.

Two normal boots with shipping FastInterp settings used the automatic
policy without queue or CPU overrides. The GLES control used the same
signed binary, raw cartridge, in-game save, inputs and clock. Both Metal
captures matched byte for byte. All three runs matched guest ticks and
336,000 draw requests over the 480-refresh lab interval.

| Renderer | Lab speed | Lab p99 | Slowest 120 refreshes | Mean in that window | p99 in that window |
| --- | ---: | ---: | --- | ---: | ---: |
| FastInterp Metal, first boot | 100.00% | 17.15 ms | 3231–3350 | 16.72 ms | 17.06 ms |
| FastInterp Metal, repeat | 100.00% | 17.27 ms | 3211–3330 | 16.72 ms | 17.05 ms |
| FastInterp GLES | 100.01% | 20.61 ms | 3436–3555 | 16.75 ms | 20.31 ms |

#### Adaptive Metal submissions

The native Apple frontends evaluate a 120-refresh window. Each slot tracks
one submitted command buffer and its validation dependencies. The verified
M4 iPad starts with 32 slots and the Mac Metal host starts with eight. When
the submission cap blocks the producer, the host can try the next step
from eight to 16 or from 16 to 32. It reverts an unsuccessful trial and
waits three windows before trying again. Thermal pressure, the Metal
working-set budget, and iOS memory warnings can lower the cap. Smaller caps
keep existing tickets alive and apply backpressure to new submissions.

Set `CARA_METAL_SUBMISSION_WINDOW` to 8, 16, or 32 for a pinned
comparison. `CARA_METAL_ADAPTIVE_QUEUE=0` disables trials and keeps the
platform's starting value. `CARA_METAL_QUEUE_POLICY_REPORT=1` records
samples and decisions in `metal-queue-policy.csv`.
`CARA_MAC_METAL_FULLSCREEN=1` uses a native macOS full-screen Space in the
Metal test host. Launch it through `open` when checking Apple Game Mode.
The Mac host uses the measured precompiled FastInterp options and core
refresh pacing. Set `CARA_METAL_HOST_DISPLAY_SYNC=1` to add display sync
for a controlled comparison.
Use the Metal Performance HUD to verify the actual system state separately
from frame-time measurements. The Developer-signed Mac verification build
showed Game Mode On in the HUD in native full screen. On the M4 iPad, the
system Game Overlay reported Game Mode On.

The following normal boots used the Dark Moon cartridge and the same
in-game save and input sequence. The sample covers lab refreshes
3121–3600. Timing includes core pacing, with the Metal HUD disabled.

| Device and renderer | Lab speed | Lab p99 | Slowest rolling 120 refreshes | Mean in that window |
| --- | ---: | ---: | --- | ---: |
| M4 iPad, adaptive Metal, default start | 100.00% | 17.15 ms | 3224–3343 | 16.72 ms |
| M4 iPad, adaptive Metal, eight-slot start | 100.01% | 17.23 ms | 3161–3280 | 16.72 ms |
| M4 iPad, GLES | 100.00% | 20.51 ms | 3470–3589 | 16.75 ms |
| M3 Pro Mac, adaptive Metal, native full screen | 100.27% | 17.97 ms | 3311–3430 | 16.76 ms |

The iPad's eight-slot run accepted 32 slots at refresh 790, and the
Mac accepted 32 at refresh 797. Both iPad Metal captures matched each other
byte for byte, and all iPad runs matched guest ticks and draw counts. The
Mac's slowest window ran at 99.75% of guest refresh speed.

The same Developer-signed Mac build used to verify Game Mode reached
100.27% with the HUD hidden, at 16.83 ms lab p99. Its slowest window was
refreshes 3248–3367 at 16.67 ms per refresh and 100.26% speed. The signed
and ad-hoc HUD-hidden captures matched byte for byte. The separate
HUD-visible run averaged 97.67% with an 18.41 ms lab p99.

## iPad Metal pipeline warmup on state load

Cara warms learned render pipelines during game open and after an in-session
state restore recreates the renderer. A normal preload keeps the existing
125 ms startup budget and stops at the budget boundary between recipes. Before
an in-session restore, Cara persists newly learned recipes. A game or state
without matching learned recipes can still compile pipelines on first use.

An M4 iPad replay used a copy of Dark Moon's slot 4 in the Front Yard. Both
passes ran FastInterp Metal at 100% guest CPU with the same touch and movement
inputs over 900 refreshes. The first pass had learned the initial view and
encountered new recipes after answering Gadd's call. The second pass warmed
all 70 learned recipes in 36 ms before starting the replay.

| Front Yard replay | Draw-time translations | Draw-time pipeline builds | First 120 p99 | Overall p99 | Worst refresh |
| --- | ---: | ---: | ---: | ---: | ---: |
| Learn the extended route | 8 | 9 | 70.81 ms | 22.05 ms | 516.84 ms |
| Replay with learned pipelines | 0 | 0 | 22.34 ms | 21.61 ms | 33.01 ms |

The 33.01 ms refresh was the first after restore. The longest later refresh
was 22.70 ms. These timings measure emulated refreshes.

An in-session reload from a disposable state restored the recorded CPU and
guest timing snapshot and continued with zero draw-time pipeline builds. Slot 4
remained byte-identical and matched the new app's engine fingerprint.

## Cold mansion gate on the M4 iPad

A separate signed test app replayed 900 Front Yard refreshes from a copy of
Dark Moon slot 4 at 100% emulation speed and 100% guest CPU. Rendering ran
on native Metal with FastInterp and both modeled GPU completion options active.
Rendering-only changes changed the engine fingerprint, so the isolated copy
used the test build's fingerprint and retained the original archive checksum.
The original device slot remained byte-identical. All completed runs had
434,138 PICA draw requests, 614 top buffer swaps, and identical final captures.

| Slot 4 replay | First refresh | Worst later refresh | First 120 p99 | Overall p99 |
| --- | ---: | ---: | ---: | ---: |
| Empty Cara cache, original Metal compiler keys | 9,853.89 ms | 575.71 ms | 575.71 ms | 22.13 ms |
| Vertex reuse, empty Cara cache, existing Metal compiler keys | 466.66 ms | 94.97 ms | 94.97 ms | 21.84 ms |
| Vertex reuse, locally learned archive | 21.29 ms | 23.19 ms | 20.49 ms | 21.52 ms |
| Vertex reuse, fresh Metal keys, serial stage compilation | 10,211.62 ms | 593.99 ms | 593.99 ms | 21.90 ms |
| Vertex reuse, fresh Metal keys, parallel stage compilation | 9,160.77 ms | 594.49 ms | 594.49 ms | 22.04 ms |

The second row reused system Metal compiler results from the first run.
Its translation time dropped from 736 ms to 583 ms across the same 29 pair
misses. The learned replay prepared 70 recipes in 28.34 ms, encountered zero
draw-time shader misses, and averaged 59.70 emulated refreshes per second.

The first baseline refresh spent 4.70 s creating vertex libraries, 1.51 s
creating fragment libraries, 3.11 s creating render pipeline states and 0.51 s
translating shader pairs. Refresh 18 spent 402 ms creating a vertex library and
130 ms creating a render pipeline. Refresh 89 spent 328 ms creating fragment
libraries and 125 ms translating. Submission pressure waits totaled under 2 ms
in each run. The trace timestamps are emulation-thread refresh intervals.

A diagnostic with separate Metal function names was used to avoid relying
on shared compiler results. Paired stage overlap reduced the first refresh
by 1.05 s and left the later hitch effectively unchanged. Metal's size
optimization experiment took 10.40 s on its first refresh versus 10.22 s for
the paired default setting. Parallel stage compilation is opt-in, and no
shader artifacts from the replay enter a distribution package.

The slot contains pending emulated GPU completions and cannot be restored
with that timing model disabled. GPU busy bits and completion IRQs derive
from emulated deadlines. Host compiler time remains wall time lost on a
synchronous draw miss. A timing-model comparison needs a normal boot or a
checkpoint without pending events.

### Retaining PICA calls on iPad

A second M4 iPad A/B replayed the same 900-refresh slot 4 route with the
new Performance Mode deadlines enabled in both arms. Both used vertex MSL
reuse, fresh Metal function names, and isolated app directories. The final
BGRA captures matched byte for byte. The tested variant uses
`CARA_METAL_KEEP_PICA_SUBROUTINE_CALLS=1` to leave generated calls intact
in large vertex shaders. The option has its own on-device cache identity.

| Slot 4 after guest timing refinement | Inline | Retained calls |
| --- | ---: | ---: |
| First refresh | 10,162.76 ms | 7,165.51 ms |
| Worst later refresh | 601.78 ms | 488.21 ms |
| Vertex library creation, 13 libraries total | 5,227.03 ms | 2,133.35 ms |
| Render pipeline creation, 32 pipelines total | 3,616.53 ms | 3,691.04 ms |
| Warm p99, refreshes 171–900 | 20.90 ms | 23.72 ms |

The variant saves CPU compiler time on the initial scene but increases the
warm refresh tail, so it stays opt-in. The 488 ms refresh still spent
230 ms creating a vertex library and 214 ms creating a pipeline. Refresh 89
still took 443 ms, including 346 ms building fragment libraries. The average
after refresh 170 was 16.71 ms in both arms. No shaders from this replay
are distributed.

### Default compiler cold and warm retest

With the refined Performance Mode timing on, FastInterp at 100% guest CPU,
and a 100% speed limit, an M4 iPad ran the same 900-refresh slot 4 replay
twice. The first pass used an empty Cara cache and new Metal entry names.
The second used the first pass's locally learned archive. Retained PICA
calls stayed disabled. Final BGRA captures matched byte for byte.

| Refresh time | Fresh caches | Learned archive |
| --- | ---: | ---: |
| First refresh | 10,100.74 ms | 27.10 ms |
| Worst later refresh | 589.11 ms | 23.47 ms |
| First 120 p99 | 589.11 ms | 22.37 ms |
| Full replay p99 | 21.33 ms | 21.18 ms |
| Slowest contiguous 120-refresh mean | 110.92 ms | 16.77 ms |

The repeat prepared 70 recipes in 27.83 ms with 32 binary archive hits.
It averaged 16.70 ms per refresh with zero draw-time shader translations,
library builds, and render pipeline builds. The cold trace spent 3.56 s
creating pipelines in total. The 589 ms interval at refresh 18 included
414 ms of vertex library creation and 130 ms of pipeline creation. The
440 ms interval at refresh 89 included 344 ms of fragment library creation.

The native Metal queue already executes prepared draws independently. A
shader miss occurs during pipeline selection before the draw reaches that
queue. Host CPU execution can overlap prepared graphics work while keeping
guest timing unchanged. First-use overlap also needs enough earlier work
to compile, or a correct general shader to draw with until specialization
is ready. Reordering CPU-visible transfers would change guest behavior.

### Async Shaders on iPad Metal

Options → Display → Async Shaders applies when a game opens. The option is
on by default in Performance Mode. The renderer generalizes alpha
comparison and scissor mode for unlit, fog-free 2D draws using texture 0
and optionally texture 1. The fallback keeps the vertex program, texture
sampling, depth output, material slots and fixed-function raster state.
Uniforms carry alpha and scissor modes. When six distinct fragment shaders
miss for the same resident packed vertex shader and texture count within
32 guest refreshes, eligible draws switch to a runtime TEV combiner.
A family expires after 32 idle refreshes. The usual large-vertex fallback
remains available. Six four-word uniform records describe the sources,
modifiers, operations, scaling and delayed buffer updates of the guest's
six stages. Dot3 and other unsupported combinations use specialized
combiners. For comparisons, `CARA_METAL_RUNTIME_TEV=0` disables runtime TEV
and `CARA_METAL_RUNTIME_TEV=1` forces it on eligible draws.

Cara compiles the general shader on the device. Once that pipeline is ready,
draws can use it while a bounded worker compiles the exact shader and
pipeline. The worker takes owned shader sources and render state. The
render owner checks completion without waiting, adopts the validated
pipeline, and records its descriptor in the local learned archive.
Already queued draws retain their pipeline. The first general pipeline
can still require a synchronous compile. No game-derived shader artifacts
are added to the distributable.

The physical M4 iPad comparison replayed a copy of Dark Moon slot 4 for
900 refreshes per run with FastInterp Metal, Performance Mode guest timing,
and 100% speed. Both cold runs started with empty Cara caches and unique
Metal entry names. The async run used the general path on 2,862 draws.
Intermediate captures at refreshes 2–4, containing 272, 32, and 272
general draws respectively, matched the corresponding stock BGRA images
byte for byte. Both final captures also matched. Intermediate capture
drains occur in both runs and add diagnostic overhead to nearby refreshes.

| Slot 4 metric | Async off | Async on |
| --- | ---: | ---: |
| Cold first refresh | 10.44 s | 10.02 s |
| Cold worst later refresh | 599.47 ms | 608.09 ms |
| Warm replay mean | 16.71 ms | 16.71 ms |
| Warm replay p99 | 21.09 ms | 21.06 ms |
| Warm worst after first refresh | 21.87 ms | 23.63 ms |

The async repeat loaded 101 recipes in 32.18 ms and recorded zero runtime
shader translations, library compiles, pipeline builds, and general draws.
The large cold stalls are still outside the prototype's coverage.

### Slot 4 vertex fallback probe

The upload trace records guest PICA vertex program and swizzle writes and
sampled host time to a cold vertex library's first draw attempt. A
checkpoint restore is outside that measurement. At refresh 18, no program
or swizzle write had occurred between restoring slot 4 and starting the
cold draw, including writes of identical values. The two main costs were
410.09 ms compiling the vertex library and 131.30 ms creating its pipeline.
This replay offers no earlier in-stream upload to schedule against.

`CARA_METAL_VERTEX_FALLBACK=1` enables the opt-in probe. It can reinterpret
vertices on the CPU and use the existing Metal StageIn route for independent
triangles with compatible draw state. A single background worker compiles the
exact pipeline. The render owner flushes GPU writers before the CPU reads
guest vertex data. Unsupported draws or unavailable fallback pipelines keep
the exact rendering path. The probe does not ship precompiled game shaders.

At 100% pacing in Performance Mode, admission limited to refresh 18 cut
that interval from 597.81 ms to 55.70 ms, with two fallback draws. Captures
at refreshes 17–19 and 900 matched the exact path byte for byte. A subsequent
vertex compile took refresh 20 to 557.00 ms. The warm repeat had no runtime
vertex library builds or fallback draws and averaged 16.71 ms per refresh
with a p99 of 21.24 ms.

A second comparison used the same build for both runs and allowed fallback
throughout the 900-refresh replay. The CPU fallback drew 24 batches. The
cold frame at refresh 18 improved from 597.72 ms to 53.94 ms, with
byte-identical captures at refreshes 17–20 and 900. The largest later stall
moved to refresh 21, where 376.71 ms went to a vertex library and 138.98 ms
to pipeline creation. The cold p99 rose from 23.96 ms to 25.92 ms. The
synchronous guest-memory flush alone took 7.04 ms at refresh 18 across
both fallback batches. The learned-cache repeat averaged 16.71 ms per
refresh, with a p99 of 24.23 ms and zero runtime library and pipeline builds.
Its five captures matched the exact path. Keep this probe opt-in while
cold stalls and CPU readback costs remain.

### Refresh 21 vertex compilation reuse

The cold trace tied the 549.44 ms interval to a live render pipeline with
an uncached vertex function. The bounded background worker was already
building a different pipeline key with byte-identical translated vertex
MSL. The old admission rule compared whole pipeline keys, then the live
path compiled the vertex library a second time. The live vertex build
and pipeline creation took 376.71 ms and 138.98 ms respectively.

Eligible draws can now use StageIn while another pipeline sharing their
translated vertex function is being built. Both lanes can reuse validated
stage functions. The asynchronous worker only receives owned sources and
immutable Metal objects. It returns a reflected pipeline and publishes
stage functions only when they match the translated runtime inputs.

Three 900-refresh physical M4 iPad runs used the same build and slot 4
input replay at 100% FastInterp Metal with Performance Mode timing.
The exact path and both candidate passes matched byte for byte at
refreshes 17–22 and at refresh 900.

| Refresh metric | Exact cold | Candidate cold | Candidate warm |
| --- | ---: | ---: | ---: |
| First refresh | 10,024.98 ms | 6,943.30 ms | 27.14 ms |
| Refresh 18 | 611.49 ms | 54.55 ms | 23.47 ms |
| Refresh 21 | 12.33 ms | 27.15 ms | 13.90 ms |
| Largest later interval | 611.49 ms | 448.78 ms | 29.44 ms |
| Full replay p99 | 23.76 ms | 31.31 ms | 24.46 ms |
| Live vertex library builds | 13 | 7 | 0 |

The candidate removed the duplicate compilation from refresh 21. Four
StageIn draws remained there, including 11.24 ms exporting GPU-owned data
for the CPU. Refresh 89 still spent 120.55 ms translating shaders and
300.09 ms compiling fragment libraries. Keep CPU vertex fallback opt-in
while the cold p99 and worst remaining intervals exceed the frame budget.

A separate cold run enabled the existing materialized readback reuse
probe. It reduced export calls at refresh 21 from 10,024 to 849, but
export time fell only from 11.24 ms to 9.63 ms. The refresh took
25.32 ms, and all seven captures still matched. The fallback still
requests the whole guest address space, so the result supports narrowing
it to vertex and index input ranges before considering default admission.

### Refresh 89 fragment shader reuse

At the mansion gate, six generated fragment shaders differed from earlier
variants in writes to a TEV combiner buffer whose value the shader never
read. With Async Shaders enabled, the native Metal generator omits these
unused writes. Draws reuse the earlier translated sources and compiled
functions. Shaders that read the buffer retain the original stage updates.

On a physical M4 iPad, three 900-refresh slot 4 replays used FastInterp,
100% guest CPU, a 100% speed limit and Performance Mode. The experimental
CPU vertex fallback was off. The cold control disabled the new shader
reuse. Cold runs used separate empty Cara caches and fresh Metal entry
names. Each raw run produced identical captures at refreshes 88–90 and 900.

| Measurement | Cold control | Cold with reuse | Warm with reuse |
| --- | ---: | ---: | ---: |
| Full replay p99 | 24.01 ms | 21.84 ms | 21.81 ms |

Cold refresh 89 fell from 446.70 ms to 17.56 ms. Its 120.46 ms of
translation and 301.26 ms of fragment compilation both fell to zero.
The next cold bottleneck was the vertex shader and pipeline at refresh 18
(613.19 ms). The warm pass averaged 59.65 refreshes per second.

Opening the Downloads Deflate ZIP on the same M4 iPad built a 1,024-entry
seek index in 1.44 s. Its cartridge member matched the raw cartridge by
SHA-256. The warm ZIP replay completed all 900 refreshes at 59.70 refreshes/s
with a p99 of 21.19 ms. Refresh 89 took 17.61 ms. ZIP pixels matched the
raw control at refreshes 88–90. Draw counts first differed at refresh 188
and the final capture differed. The cause of the later divergence is open.

### macOS cold-run window responsiveness

A single native Metal test binary at `3948557` ran Dark Moon for 3,600
refreshes each with FastInterp and four-core Dynarmic JIT. Both runs used
100% guest CPU, Performance Mode timing, Async Shaders, and separate cold
runtime shaders. The longest intervals were 15.51 s at refresh 310 with
FastInterp and 16.83 s at refresh 294 with JIT. Runtime pipeline creation
was logged during both gaps, and both runs completed. Checkpoints by
refresh showed different game progression, so this pair measures launch
behavior and gives no renderer pixel-parity claim.

The old `cara-metal-rom` harness ran emulation and serviced SDL window
events on the main thread. A draw that waited for a Metal library or a
pipeline could block event polling until the compiler returned. The
updated harness keeps AppKit and SDL window work on the main thread and
runs Metal setup, emulation, draw preparation, and capture on one owner
thread. It queues input for that owner and joins it before UI teardown.

A cold rerun on the M3 Max completed 1,200 refreshes with each CPU backend
and matched the corresponding old frame 1,200 capture byte for byte.
Samples taken during both runs placed actual library and pipeline creation
on the emulation owner, with SDL and AppKit polling on the main thread.

| Backend | Longest cold refresh | Window events delivered during it | Event delay p99 / max |
| --- | ---: | ---: | ---: |
| FastInterp | 15.71 s at 310 | 287 | 1 / 160 ms |
| Dynarmic JIT | 16.80 s at 294 | 309 | 1 / 2 ms |

Window responsiveness and game-frame latency are separate here. Cold
exact shader compilation still delays gameplay until the draw is ready.
The measured window path is the native Metal harness. The test did not
cover manual closing or fullscreen transitions during a compile.

## Cold shader work and command-list lead time

On an M3 Pro, FastInterp Metal booted Dark Moon through Gadd's lab and the
mansion entrance with the learned cache disabled. The guest CPU stayed at
100%. Replaying with both GPU completion options enabled, the 100% and 200%
host speed limits averaged 55.6 and 84.7 emulated refreshes per second.
Both produced byte-identical pixels at 40 matching capture refreshes.
The 200% diagnostic still had an 847.79 ms refresh interval during the
entrance sequence. These are emulator refresh intervals. The run order also
allowed the system Metal compiler cache to carry over between processes.

With `CARA_METAL_FRAME_COST=1`, `pica_list_to_first_shader_translation`
measures from PICA command-list entry to the first runtime translation miss
in that list. In the 200% replay, 31 of 42 such lists reached their first miss
in less than 1 ms, and all 42 in less than 5 ms. Runtime translation handled
259 new pairs in 8.96 s total. A submission hook could inspect an immutable
command-list snapshot. On this route, its head start would usually cover a
small fraction of translation. Later misses within the same list can have
more lead time, including time spent on earlier shader misses.

`CARA_METAL_REUSE_VERTEX_MSL=1` tests a bounded reuse cache for identical
linked vertex SPIR-V and vertex mode. Each pair still links and validates
its resources and varyings. The cache skips the SPIRV-Cross MSL emission for
a repeated vertex stage paired with a different fragment.

In the matched 200% replays, it reused 198 of 259 vertex stages and cut
draw-time translation from 8.96 s to 5.86 s. All 41 captured images,
including the final capture, matched byte for byte. Overall refresh p99
went from 35.62 ms to 29.40 ms. The slowest mansion-entrance refresh
still took 543.22 ms, down from 847.79 ms. Vertex MSL reuse now defaults on. Set `CARA_METAL_REUSE_VERTEX_MSL=0` for an exact translation control.

### Mansion gate with Performance Mode defaults

The M4 iPad replay restores a copy of Dark Moon slot 4, applies the same
inputs for 900 refreshes and caps emulation at 100%. Async Shaders enables
the compatible StageIn vertex fallback. Before a CPU vertex read, Metal
flushes the index range and the attribute spans referenced by the draw.
The shader worker builds the exact pipeline. The learned-cache replay
needed zero runtime vertex library or pipeline builds.

| FastInterp Metal | First refresh | Refresh 18 | Full p99 | Slowest after first |
| --- | ---: | ---: | ---: | ---: |
| Exact vertex path, cold control | 10,212.13 ms | 610.94 ms | 21.59 ms | 610.94 ms at 18 |
| Async Shaders default, fresh shader namespace | 6,805.80 ms | 46.33 ms | 23.26 ms | 271.02 ms at 2 |
| Same default, learned cache | 31.64 ms | 23.07 ms | 24.15 ms | 26.37 ms at 131 |

The cold run recorded 99 vertex fallback draws, 2,326 general fragment
draws and 20 linked vertex translation cache hits. Both passes matched exact-path
pixels at refreshes 17–21 and 900. Refresh 18 still spent 24.76 ms in
runtime shader translation. At refresh 2, pipeline creation took 158.11 ms,
fragment library creation 56.55 ms and translation 28.92 ms. These long
intervals can fall outside p99. Timing includes the diagnostic captures.
The warm run delivered 59.68 refreshes/s. The slowest rolling 120-refresh
window averaged 75.29 ms cold (1–120) and 16.78 ms warm (12–131).

The GLES generator uses the same dead combiner-write analysis. The GLES
shader cache reuses identical emitted source. GLES device tests can capture
individual refreshes with `CARA_GLES_PARITY_START` and
`CARA_GLES_PARITY_COUNT`. These benchmark captures read back the 400 × 480
guest display in bottom-left RGBA8. Metal captures use top-left BGRA8 at
the device presentation resolution.

| FastInterp GLES, same replay | First refresh | Refresh 18 | Full p99 | Slowest after first |
| --- | ---: | ---: | ---: | ---: |
| First process, fresh Cara user directory | 5,864.91 ms | 234.65 ms | 25.43 ms | 346.09 ms at 2 |
| Repeat process | 59.72 ms | 21.29 ms | 25.79 ms | 28.66 ms at 64 |

GLES delivered 42.02 refreshes/s on the first pass and 59.70 on the repeat.
The slowest rolling 120-refresh window averaged 69.79 ms on the first
pass and 16.92 ms on the repeat (1–120 in both). All six raw GLES
captures matched between passes. The test did not reset
Apple’s GLES driver cache. Cross-backend byte equivalence was not tested.

Using the same route with an indexed ZIP and warm caches, Metal averaged
59.66 refreshes/s with 24.09 ms p99. GLES averaged 59.65 with 25.51 ms
p99. Both matched their raw-ROM captures at refreshes 17–21. At refresh
900 both differed from raw. The cause of that divergence remains open.

On the M3 Mac, the AppKit options panel persists Async Shaders for the
separate native Metal runner. Four 120-refresh Dark Moon boot checks
verified the effective switch for a scratch profile (off), saved Performance
(default on), Performance with the option off, and saved Accuracy (off).
The AppKit gameplay renderer remains OpenGL.

### Concurrent cold Metal compilation

When both shader libraries miss, Async Shaders starts the vertex library while
the render owner compiles the fragment library. The background specialization
worker takes the already translated pair and still reflects the resulting
pipeline before publication. Accuracy mode keeps its previous behavior.
No ROM-specific shaders or binary archives are shipped.

On the M4 iPad, alternating the stage policy in fresh compiler namespaces
gave first-refresh times of 6,773.50 ms serial and 6,372.15 ms concurrent.
Refresh 2 measured 268.05 and 269.18 ms, respectively. Its trace contained
one fragment library, three pipeline creations and zero vertex libraries.
P99 was 23.16 and 23.00 ms. Raw captures matched at 17–21 and 900.

A second slot 4 replay checked compiler options with the worker translation
reuse active. At 100% pacing, keeping large PICA vertex subroutine calls
out of line cut the first refresh from 6,331.76 to 4,566.62 ms but raised
refresh 2 from 265.16 to 349.16 ms and p99 from 23.14 to 23.68 ms.
The option remains diagnostic. Apple's size optimization level measured
6,417.77 ms at refresh 1 and 267.26 ms at refresh 2, so the renderer keeps
the default compiler optimization level. All six sampled images matched
for both experiments. [Apple’s shader optimization guidance](https://developer.apple.com/documentation/metal/minimizing-the-binary-size-of-a-shader-library)

Allowing a cached StageIn pipeline to cover an exact PSO miss with a
resident vertex function still left refresh 2 at 271.58 ms, versus
265.16 ms in the preceding control. This probe stayed out of the default
path. Conservative control-flow analysis also found every emitted function
and dispatcher case reachable in twelve large gate vertex programs. Folding
the entry wrapper changed the median Mac PSO compile time by 0.08 ms
over 24 matched pairs.

A trace of the three synchronous links at refresh 2 separated two inexpensive
descriptors from the large multi-texture draw. The first refresh already
encountered two of the exact pairs. Queueing them early saved one synchronous
link but increased the first refresh by 641 ms. The remaining draw introduced
a second texture sample that modulates alpha. Its exact fragment first became
known at refresh 2.

With Async Shaders enabled, eligible multi-texture draws with a resident
vertex function and at least 32 KiB of vertex MSL can now use interpreted
guest vertices and the exact fragment through Metal StageIn while the worker
links their packed pipeline. Existing geometry, assembly and resource checks
still apply. Allowing new StageIn descriptors also avoided a 98.77 ms cold
link at refresh 99 seen when only resident descriptors were accepted.

The following M4 iPad slot 4 replays used identical gate traversal inputs,
FastInterp, native Metal, Performance Mode and a 100% speed limit for 900
refreshes. Each cold run used a fresh pipeline archive and compiler namespace.
P99 covers all 900 refresh intervals.

| Run | Refresh 1 | Refresh 2 | Refresh 99 | P99 | Refreshes/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| Control, cold | 6454.52 ms | 265.68 ms | 23.73 ms | 23.65 ms | 41.35 |
| Updated, cold | 6429.48 ms | 107.49 ms | 23.28 ms | 23.10 ms | 41.71 |
| Control, warm | 31.27 ms | 20.42 ms | 24.01 ms | 24.15 ms | 59.68 |
| Updated, warm | 19.66 ms | 26.56 ms | 23.74 ms | 23.08 ms | 59.69 |

At refresh 2 the three foreground links took 12.480, 0.092 and 0.053 ms.
Aggregate foreground link time fell from 157.60 to 12.63 ms. The remaining
fragment library took 52.19 ms. BGRA captures at refreshes 2, 3 and 900
matched byte-for-byte across all four runs. Set `CARA_METAL_TRACE_COLD_PSO=1`
to log runtime pair discovery and synchronous descriptor link times.

A separate Mac FastInterp comparison booted the Dark Moon ZIP normally
for 660 refreshes. All 11 captures matched. Before and after the combined
changes, p99 measured 114.97 and 109.54 ms, with the worst interval at
refresh 310 falling from 12.77 to 10.45 s. These are single runs with
fresh compiler namespaces. The Mac boot and iPad checkpoint cover different
workloads.

## Cartridge gameplay checks

The M4 iPad library sweep on November 21 exercised 18 local files from
the regular Cara app’s Roms directory. The collection showed 18 entries
and the library search found Planet Robobot. Four games ran directly from
ZIP: Cooking Mama 5, Kirby: Planet Robobot, Monster Hunter Stories and
One Piece: Romance Dawn. The 14 raw cartridges were 7th Dragon III,
Ever Oasis, Gravity Falls, Hyrule Warriors Legends, LEGO Batman 3,
A Link Between Worlds, Ocarina of Time 3D, Dark Moon, Mario Kart 7,
Monster Hunter 4 Ultimate, Paper Mario: Sticker Star, Persona Q2,
Sonic Boom: Fire & Ice and Xenoblade Chronicles 3D.

Each boot used a separate test save directory, FastInterp and native Metal.
Fifteen titles completed the first 900-refresh input replay. Three stopped
and passed the same replay after renderer fixes on the same device:

| Game | First stop | Fix |
| --- | ---: | --- |
| Kirby: Planet Robobot | 330 | Filter arbitrary border colors at each mip level. |
| Ocarina of Time 3D | 431 | Accept optimized-out sampler slots when reflecting translated shaders. |
| Paper Mario: Sticker Star | 13 | Convert cropped, tiled RGB5A1 to linear RGB565 on display transfer. |

These are boot and early-rendering checks. Later gameplay needs scene-specific
input routes and longer captures.

### Kirby shader translation on the M4 iPad

Planet Robobot was launched from its ZIP at 100% with Performance Mode and
FastInterp. With different fresh compiler namespaces and empty per-run archives,
the same binary replayed the same 900 refreshes with linked vertex reuse off
and on. The optimization reuses a validated vertex program when only the
fragment main body changes. It still validates the fragment inputs.

| Native Metal | Foreground translation, total | Refresh 332 | Refresh 523 | P99 | Slowest 120-refresh mean |
| --- | ---: | ---: | ---: | ---: | ---: |
| Control | 788.9 ms | 2699.3 ms | 2098.2 ms | 242.30 ms | 62.05 ms |
| Linked vertex reuse | 462.8 ms | 2664.3 ms | 2046.2 ms | 229.27 ms | 60.97 ms |

Final cold BGRA captures matched byte for byte. Even with reuse, refresh 332
spent 1406.7 ms in fragment library builds. The warm replay reached 58.49
refreshes/s with a 28.93 ms p99. A warm GLES control on the same route reached
59.77 refreshes/s with a 27.15 ms p99. Booting with fresh guest storage and
relaunching with the game's saved data can exercise different shaders.

In a separate experiment, storing more unique render descriptors increased
warm recipe availability from 67/214 to 214/214 but changed warm p99 by only
0.05 ms and increased preload time from 55 to 88 ms. The experiment stayed
out of the default. Disabling Async Shaders increased the largest cold
interval to 4007 ms in the same opening.

### Runtime TEV comparison

The M4 iPad replayed 900 refreshes from the Kirby ZIP on FastInterp at the
100% speed limit in Performance Mode. Matching cold runs captured both
screens at refreshes 331–335. Each image matched byte-for-byte.

| Cold Metal metric | Specialized combiner fallback | Runtime TEV fallback |
| --- | ---: | ---: |
| P99 refresh interval | 276.45 ms | 161.11 ms |
| Worst refresh interval | 2788.9 ms | 2465.5 ms |
| Slowest rolling 120-refresh mean | 62.59 ms | 49.41 ms |

The warm runtime-TEV replay reached 59.25 refreshes/s with a 27.03 ms p99.
An Apple GPU differential separately checked the generated TEV arithmetic
against 77 specialized stage sequences extracted from 119 eligible cached
Kirby fragments. All 78,848 RGBA8 outputs matched across randomized
primary colors, post-sample texture values and combiner constants. This
arithmetic test complements the frame captures, which also exercise texture
sampling and rasterization.

Fresh guest storage and compiler namespaces also exposed cold regressions in
two other titles. Dark Moon fell from 32.10 to 30.98 refreshes/s and its
worst interval grew from 9255 to 10042 ms. Ocarina of Time fell from 50.88
to 50.34 refreshes/s and its worst interval grew from 1330 to 1681 ms,
although p99 fell from 37.1 to 31.9 ms. Both games matched at refreshes
429–432 and 900. The always-on setting remains diagnostic.

Adaptive admission replayed the same 900-refresh routes with fresh caches.
Kirby cold p99 fell from 276.45 to 169.09 ms, its worst interval from 2789
to 2599 ms and its slowest rolling 120-refresh mean from 62.59 to 56.78 ms.
The worst interval in Ocarina was 1329 ms versus the 1330 ms control.
Dark Moon measured 9235 ms versus 9255 ms. All sampled frames matched the
corresponding control byte-for-byte. These measurements support adaptive
admission as the default with Async Shaders.

An M3 Pro Mac probe tested whether an unrelated busy specialization worker
could redirect cold draws to already resident StageIn pipelines. Both
900-refresh Kirby boots used the same ZIP, fresh guest storage and fresh
compiler namespaces at 100% with FastInterp, Performance Mode and Async
Shaders. Only one of 28 eligible busy-worker draws found a matching resident
StageIn pipeline. Its ordinary packed link took 0.093 ms. The largest missed
draw without a resident fallback used 4,395 vertices, lighting and three
textures. Vertex MSL digest `0bf34ed76ae5` and fragment digest
`c4457036d70a` identified its 169 ms link. At refresh 331 the control took 3175 ms and the probe
3165 ms; refresh 332 took 613 and 617 ms. P99 was 159.56 and 156.92 ms,
respectively, and final pixels matched. Resident-only reuse offers little
coverage of this cold burst, so the probe remains outside the default path.

Boot each local cartridge with a fresh user directory and FastInterp. Drive
menus and gameplay with the same inputs when comparing performance. Capture
both screens after reaching a controllable scene and verify that directional
or touch input moves the player or advances the game. Use the 100% speed
limit for timing. A 200% limit can shorten an intro replay when the host has
headroom. Report the measured refresh rate separately.

| Cartridge | Gameplay checkpoint | Metal behavior exercised |
| --- | --- | --- |
| Cooking Mama 5 | Meat-mincing step in Cabbage Roll Tomato Soup | Transient invalid LCD stride at boot, then valid scanout. On the Mac, mouse input reaches the touch screen. |
| Driver Renegade | First pursuit with acceleration and steering | Immutable cube-map face chains are reused until the decoded faces change. |
| Monster Hunter Stories | Secret Path after Cheval’s opening dialogue | Material samples that overlap the active depth attachment use a separate texture snapshot. Queued D24 and D24S8 exports retain shader-side validation before presentation or a guest read. |
| Ever Oasis | Great Tree Spring after the prologue, with free movement | Opaque black sampler borders map to Metal on Apple GPU family 7 and later. |

Monster Hunter Stories timings use refreshes 4001–4600 of the Secret Path
replay at the 100% speed limit. The worst mean covers a rolling 120-refresh
window. Each comparison on the same device uses the same input script.

| Device and build | Refreshes per second | p99 | Slowest window | Mean in that window |
| --- | ---: | ---: | --- | ---: |
| M3 Pro Mac, initial playable Metal build | 16.19 | 78.40 ms | 4023–4142 | 64.75 ms |
| M3 Pro Mac, queued imports and fragmented exports | 26.02 | 48.89 ms | 4135–4254 | 39.35 ms |
| M4 iPad, first queued depth-to-texture build | 11.04 | 107.06 ms | 4391–4510 | 90.75 ms |
| M4 iPad, queued imports and fragmented exports | 30.24 | 39.87 ms | 4357–4476 | 33.45 ms |
| M4 iPad, FastInterp GLES control | 46.96 | 26.76 ms | 4424–4543 | 21.37 ms |

The Mac recorded 19,451 synchronous depth exports over refreshes 2001–3000
before queueing all the eligible fragments and zero afterward. Over the same
interval, the time spent preparing PICA draw bindings fell from 27.13 to
13.93 ms per refresh. It still issued about 645 PICA draws per refresh.
The command trace retained about 216 tracked submissions per refresh over
refreshes 2001–2600, whose GPU start/end intervals covered 13.72 ms per
refresh after merging overlaps. CPU sampling showed PICA register handling,
draw preparation, and queue backpressure on the emulation thread. The title
still runs below full speed on both devices. The same Metal build replayed
Dark Moon’s Gadd lab on the Mac at 59.92 refreshes per second over
refreshes 3121–3600, with p99 of 18.13 ms.

The Driver Renegade pursuit matched byte for byte at the final capture
after cube reuse. Over refreshes 5721–6200, the mean moved from 20.87 to
17.87 ms and p99 moved from 35.27 to 28.90 ms. This is a before and after
comparison of the compatibility builds.

Run the checked-depth fixture on an Apple Silicon Mac with
`python3 tests/run_metal_async_depth_validation_test.py --build-dir <native-metal-build>`.
It exercises ordered depth imports and exact three-byte D24 round trips with
Metal API validation enabled, including an intentionally pending producer
and independently failing shader-side checks.

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

### Kirby first-use shader fallback on iPad

Display → First-use shader fallback opts a Performance Mode session into CPU
vertex processing when the game next opens. On Metal, enable Async Shaders
to also use the general texture combiner immediately for supported draws.
The fallback starts off because CPU vertices can increase missed deadlines
over a full session.

Two 900-refresh normal boots per configuration on an M4 iPad used the same
Kirby ZIP, FastInterp, 100% pacing, fresh guest storage and fresh shader
identities. Both backends saw 93 draws at refresh 332 and 115 at refresh 523.

| Renderer | Worst refresh (332) | Refresh 523 | p99 |
| --- | ---: | ---: | ---: |
| GLES | 1296–1308 ms | 890–899 ms | 60.5–60.6 ms |
| GLES with fallback | 278–286 ms | 270–281 ms | 56.3–58.7 ms |
| Metal | 2607–2643 ms | 2027–2030 ms | 170.3–172.8 ms |
| Metal with fallback and Async Shaders | 1354–1386 ms | 1016–1040 ms | 155.2–155.5 ms |

The Metal fallback still spent 992 ms building 14 fragment libraries at
refresh 332 in one run. Both fallback paths matched their renderer's visible
image at refresh 535 and final capture at 900. Metal recorded 105–108 late
paced refreshes with the fallback versus 43 in the second control.

For independent developer comparisons, `CARA_METAL_RUNTIME_TEV=burst`
admits the shared fragment after three distinct eligible misses of one
texture layout within a refresh. `CARA_GLES_RUNTIME_TEV=1` tests the shared
combiner with GLES. Both experiments produced smaller gains than the
vertex fallback and remain explicit launch overrides.
