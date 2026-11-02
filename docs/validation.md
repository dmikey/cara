# Validation

## Local development evidence — November 2, 2026

- Source baseline: `weihuoya/citra` revision `302b0c3bc4312bc96489c11584a97664894b343f`.
- Build tools: CMake 3.31.10, Ninja 1.13.2, Xcode 27.1, iOS 27.1 SDK.
- iOS ARM64 app compiled and linked with statically linked emulator libraries.
- Installed and launched as a development build on a physical iPad Pro 11-inch (M4), iPadOS 27.0.1.
- A locally supplied Zelda: A Link Between Worlds USA dump booted and rendered its opening animation on both screens.
- Physical-device samples: 540 displayed frames in 33.911 seconds (15.924 FPS), later 900 in 89.426 seconds (10.064 FPS). These are early boot averages, with CPU interpretation, software graphics, and audio off.
- macOS probe: 278 frames in 30 seconds; longer run 921 in 120 seconds, returning success.
- The linked iOS binary had no Dynarmic, JitX64, mmap, mprotect, or vm_protect symbols. Its dynamic dependencies were Apple system libraries only. Development signing used no JIT entitlement or JIT-enabling debugger.

The measurements vary by scene and do not establish full-game performance. Gameplay, save/reload reliability, hands-on controls, device rotation, and long-session behavior remain unverified. Game screen captures and private device/signing receipts are retained locally, outside this repository.

## Continuous integration

The workflow builds from the emulator and dependency source committed inside Cara. Source validation rejects external git submodules and excluded game/system assets; no MMJ repository or dependency mirror is fetched. It produces unsigned artifacts and audits the actual linked products. The native I/O self-test exercises write, flush, tell, seek, resize, read, invalid seek, and reopen behavior without a ROM.

A green CI result proves compilation, linkage, the no-JIT audit, and the host I/O regression check. It does not prove iOS simulator execution, physical-device installation, or commercial-game compatibility.

## Renderer work — November 2, 2026

A deterministic 600-frame host comparison recorded 1,200 per-screen pixel hashes. The original software path took 52.2723 seconds (11.4784 FPS), combiner evaluation changes took 35.6809 seconds (16.8157 FPS), parallel scanline rendering took 26.6222 seconds (22.5376 FPS), and additional state/combiner/blend fast paths took 23.2904 seconds (25.7617 FPS). All 1,200 hashes matched the original in each case.

On the physical M4 iPad, the same 600-frame benchmark improved from 29.0166 seconds (20.6778 FPS) to 14.5803 seconds (41.1513 FPS) with the optimized software path.

The first experimental native GLES3 GPU run completed 600 frames in 5.56822 seconds (107.754 FPS, uncapped). The device reported `Apple M4 GPU` and `OpenGL ES 3.0 Metal - 105`. This is a GLES3 frontend using Apple's driver, not a direct Metal renderer. CPU JIT remained excluded. Early screen captures showed the game's opening animation. Longer-scene rendering and game compatibility are still being checked; this short benchmark is not a full-game/native-speed guarantee.

The native GLES3 adaptation uses 2D float textures for lookup tables, GLES 3.0-compatible shader generation, and framebuffer blits in place of newer copy-image operations. Unsupported shadow image operations and format reinterpretations require further compatibility work. The software path remains the reference/fallback build.

## Direct iOS GPU presentation and frame budget

The GLES3 frontend now presents a GPU renderbuffer directly through a native screen layer. Normal play no longer reads each frame back into CPU memory or builds UIKit images. Diagnostic captures read back only every 60 frames. The combined two-screen surface preserves the 400x480 layout and maps touch coordinates to the lower 320x240 region. A physical-iPad screenshot confirmed orientation and both screens; the frame-3600 top-screen PNG was byte-identical to the earlier readback path.

An uninterrupted 3,600-frame run of the earlier GLES3 frontend completed in 71.4814 seconds (50.3627 FPS, uncapped, zero pauses). With opt-in timing enabled, comparable runs measured 49.6843 FPS before direct presentation and 58.0952 FPS afterward. The final 600-frame window improved from 26.31 ms to 22.74 ms per frame; readback fell from 3.98 ms to 0.057 ms and native presentation took 0.22 ms. Interpreted PICA shader work still consumed 9.07 ms per frame in that window. The post-change 95th-percentile frame time was 26.89 ms, above the native 16.71 ms budget. These measurements do not establish a locked frame rate.

A larger per-draw vertex cache matched all 1,200 host screen hashes but barely reduced shader calls in the title scene; it was not retained. Further work should target GPU vertex processing and remaining CPU/driver costs, then validate real gameplay and sustained performance. CPU JIT remains excluded.

For local, uncapped timing, launch a development app with `CITRA_BENCHMARK_FRAMES=3600` and `CARA_FRAME_TIMING=1`. `Documents/citra/frame-timing.json` records 600-frame windows with mean, median, p95, maximum, over-budget frame count, shader calls, and time in interpreted PICA shaders, GPU readback, and native presentation. Timers are opt-in and add measurement overhead; shader/readback/presentation timings are wall-clock scopes on the emulation thread, not hardware GPU timestamps. Normal play is paced to the emulated display rate of approximately 59.834 Hz.

## GLES3 GPU vertex processing

The iOS GLES3 default now runs PICA vertex programs on the GPU. Triangle lists, strips and fans are expanded using the existing primitive assembler, decoded inputs are uploaded as a float texture, and the fixed geometry-stage output mapping is folded into the vertex shader. The first vertex is re-evaluated where needed to preserve each triangle's quaternion-sign correction. Draws using guest geometry shaders, oversized draws, and unsupported programs retain the CPU interpreter fallback. There is no guest CPU code generation or executable-memory allocation. `CARA_CPU_VERTEX=1` selects the earlier interpreted vertex path for local comparisons.

Apple's GLES linker crashed on the first generated vertex program's switch/fallthrough tree. The native decompiler now emits an equivalent conditional chain inside a single-iteration loop, retaining case fallthrough and jump behavior. A ROM-free GPU transform-feedback fixture passed on the physical M4 iPad: both uniform-branch outcomes, a counted loop, position, quaternion sign correction and color saturation matched the CPU interpreter within 0.00001. Run it during local app startup with `CARA_VERTEX_SELF_TEST=1`; its result is saved as `Documents/citra/gpu-vertex-self-test.json`. CI compiles this fixture but does not run it on a physical device.

The GPU-vertex 3,600-frame test completed in 45.6867 seconds (78.7975 FPS uncapped), followed by 45.2674 seconds (79.5274 FPS) with decoded-input reuse. Both runs recorded 382,984 accepted accelerated batches, 125,132,880 expanded vertices, zero CPU vertex draws and zero paused seconds, with CPU JIT disabled. In the latter run, the final 600 frames averaged 15.27 ms, but p95 was 18.47 ms and 112 frames exceeded the 16.71 ms budget. The opening-600 top image was pixel-identical to the earlier renderer; the frame-3600 top image matched 99.88% of pixels exactly, with a maximum channel difference of 9/255. These are bounded boot/title tests with audio off, not a full-game locked-60 claim.

`CITRA_BENCHMARK_FRAMES` remains uncapped unless `CARA_PACED_BENCHMARK=1` is also present. Paced benchmark receipts report `paced` and `late_paced_frames` (frames that finished more than 1 ms after their scheduled deadline). In paced timing reports, frame intervals include intentional sleep, so use uncapped reports to measure rendering headroom. Shader compilation can still cause first-use hitches. The next validation target is sustained playable scenes, followed by shader warmup/cache work and audio.

## New 3DS interpreter cache regression

A user-supplied Xenoblade Chronicles 3D USA Rev 1 dump (`000400000F700100`) selects MMJ's existing New 3DS override and multicore scheduling path. The initial host interpreter run crashed during startup after its first display frame. The interpreter stored decoded instructions in one shared arena but retained independent per-core lookup tables. Resetting the arena on one core left another core's offsets pointing into overwritten records.

The shared arena now has an invalidation generation, checked before each block lookup. A synthetic two-core regression runs distinct ARM `MOV` instructions at the same guest address, overwrites the shared arena, then resumes the first core. Without the generation check it deterministically returns the second core's value (29 rather than 17); with the fix all 17 executions pass. This is decoded instruction data, not generated executable code. `citra-core-cache-self-test` now runs in host CI.

After the fix, a local macOS software-renderer test completed 436 display frames in 30.1064 seconds (14.482 FPS), returning success and rendering a loading animation. This establishes progress past the original crash, not physical-iPad or GPU performance for Xenoblade. Its 4 GB dump was copied successfully into private app storage after a transfer retry. Device execution must be validated separately. iOS benchmark receipts now include `new_3ds`, and each ROM startup resets the frontend mode before title overrides are applied.

## Native GLES3 depth-cache repair

Xenoblade booted on the M4 iPad after the interpreter fix, but most of the title
scene was black. Switching to interpreted vertices did not fix it. Disabling depth
testing revealed the scenery; a diagnostic depth reset restored the complete
scene. Neither workaround is shipped.

Tracing the game's actual memory fills showed depth clears arriving normally,
but an expanded cached depth surface was missing from part of its registered
address range. A ROM-free reproduction reduced this to two cached intervals
separated by a gap and one larger overlapping insertion. The old Boost ICL
headers fail this test with the current Apple compiler, losing coverage at
address 100. This matches the upstream libc++ 22 interval-lookup compatibility
issue. Cara backports Boost's maintained map/set adaptors, with source revision
and license retained in `externals/boost/CARA_PATCHES.md`; no runtime download is
required. The regression now runs in host CI.

With this repair, the physical iPad's 1,200-frame Xenoblade test rendered the
sky, sword and grass with normal depth testing. It completed in 15.8431 seconds
(75.743 display FPS uncapped), with New 3DS mode, zero paused time, 67,236 GPU
vertex batches and no CPU vertex draws or CPU JIT. This is a bounded boot/title
scene with audio off; display FPS does not establish the game's unique-frame
rate or full-game performance. Captures and benchmark receipts remain local.

For repeated physical-device benchmarks, `CARA_TEST_KEEP_AWAKE=1` optionally
keeps the app awake for fifteen minutes after a frame-count test finishes. Normal
play and ordinary test shutdown behavior are unchanged.

A subsequent 3,600-frame paced iPad run completed in 60.385 seconds (59.6175
display FPS), with zero pauses and a complete title scene at the final capture.
It recorded 1,836 frames more than 1 ms late, so this is not a locked-frame-time
claim. GPU vertex processing remained active throughout, with no CPU vertex draws
or CPU JIT.

The same iPad build also completed Zelda's 1,200-frame compatibility test in
13.6097 seconds (88.1726 display FPS uncapped), rendering the title scene with
zero paused time, no CPU vertex draws and no CPU JIT.

## iOS quick-tap input repair

After the scenery repair, the user reported that A/Start did not advance
Xenoblade. The frontend now delivers control touches without the scroll view's
default delay, and retains each press until the emulated HID device polls it.
Releases still end held input normally. Stop/background clears both held and
pending input. `citra-input-self-test` covers taps that begin and end between HID
updates, holds, simultaneous buttons and resets; it runs in host CI.

On the physical iPad, temporary input tracing showed UIKit A presses reaching
emulated HID reads, and the user confirmed that the game now advances. Input
tracing is opt-in (`CARA_INPUT_TRACE`) and ordinary launches do not enable it.
The user subsequently reported about 16 FPS in cutscenes: title-scene averages
above do not establish gameplay or cutscene performance.

## Cutscene vertex-upload optimization

The user reported approximately 16 FPS after entering Xenoblade's cutscenes.
Built-in timing reproduced a 61.37 ms average display-frame window. GPU vertex
processing stayed active; CPU vertex fallback was not responsible. More detailed
timing identified synchronous vertex-input texture uploads as the largest cost.
The old frontend updated the same 4 MiB input texture for every draw.

The native GLES3 path now retains separate vertex-input textures for each draw
in three rotating frame banks. Texture heights follow the required input data,
rounded to a power of two. Each bank retains at most 32 MiB of texture data and
4,096 textures; draws beyond these limits use fresh-storage uploads. OpenGL
continues to preserve in-flight data if a bank is reused before the GPU finishes.
This changes upload storage and lifetime, not vertex values, shaders, guest CPU
execution or emulated depth behavior.

A physical-M4-iPad comparison used the same 6,000 display frames, fixed RTC, and
scripted A presses at frames 1,200, 1,800 and 2,400. The old upload path took
129.295 seconds (46.4053 display FPS uncapped); the frame-bank implementation took
89.4333 seconds (67.0891 display FPS). Both processed exactly 691,947 GPU vertex
batches and 148,031,280 expanded vertices, with zero CPU vertex batches, pauses or
CPU JIT. The baseline's slowest 120-frame window, ending at frame 4,440, improved
from 59.6817 ms (16.76 FPS) to approximately 18.08 ms (55.3 FPS). Vertex-upload
call time in that window dropped from 36.1757 ms to about 1.08 ms per frame.
Top-screen captures at frames 3,600, 4,800 and 6,000 and the bottom-screen capture
at frame 6,000 matched pixel-for-pixel. Captures and detailed reports remain local.

Diagnostic timing now also records guest CPU execution, input preparation,
vertex upload and draw submission. Guest CPU timing includes HLE calls and nested
drawing, so those fields must not be added together. `CARA_TIMING_WINDOW_FRAMES`
selects windows from 60 to 600 frames (default 600).
`CARA_TEST_PRESS_A_FRAMES` provides comma-separated A-press frames only during an
explicit frame-count benchmark; ordinary play never injects input.

These are bounded opening-cutscene measurements with audio off, not a guarantee
of locked 60 FPS throughout the game. Shader first-use, CPU interpretation and
other scenes still need further performance work.

The final default implementation completed the same 6,000-frame paced sequence
in 104.848 seconds (57.2255 display FPS), with 3,328 frames over 1 ms late. It
therefore improves cutscene speed substantially but does not establish locked
60 FPS. Zelda's 1,200-frame check completed at 87.2773 display FPS uncapped,
with a top-screen capture pixel-identical to the prior verified build. Both
linked products passed their no-JIT audits; host input, interval-cache,
multicore-cache, native I/O and raster checks passed.

For temporal-artifact diagnosis only, `CARA_CAPTURE_BURST_START` and
`CARA_CAPTURE_BURST_COUNT` capture 1–120 consecutive frames (default 36),
requiring `CITRA_CAPTURE_FRAMES`. Burst images are written to the app's private
`Documents/capture-burst` directory. PNG capture can affect timing, so these
bursts are excluded from performance comparisons.


## Later-scene packing and titan rendering investigation

The 16-register vertex-input upload was reduced to the registers mapped by the
current PICA program. Native shaders fetch a packed layout from a 128-texel-wide
atlas; the layout mask is part of the shader cache key. Frame banks reclaim
unused tail allocations before falling back, while retaining the 32 MiB per-bank
budget. `CARA_VERTEX_UPLOAD_MAX_DRAWS` overrides the default 4,096 slots for
measurement (clamped to 1–8,192).

On the physical M4 iPad, the later 120-frame window ending at display frame 7,680
improved from 50.665703 ms to 33.738624 ms. Vertex uploads fell from 16.11057 ms
to 0.828586 ms, with no fallback uploads and about 69.2 MiB of retained input
textures. All 48 captured frames at 7,440–7,487 matched the previous layout
pixel-for-pixel. This still leaves roughly 30 display FPS in that window;
interpreted guest execution remains a substantial limit. The automatic replay
later stalls, separately from the user's report of slow but moving animation.
Neither measurement establishes full-game compatibility.

The ROM-free physical GPU vertex fixture now checks 99 vertices, crossing atlas
rows, with full, sparse and missing input registers. It covers branches, loops,
quaternion sign correction, primary color and PICA clip-space limits. PICA's
[-w, 0] Z range is mapped to GLES' [-w, w] range; the fixture rejected a positive-Z
triangle only after this correction. The correction alone did not resolve the
user's titan flicker.

`CARA_DEPTH_SELF_TEST=1` checks D16 and D24 depth ties against an integer PICA
reference. Raw GL depth failed both cases on the M4; explicit truncation passed
both. Native fragment shaders enable that conversion by default; `CARA_RAW_DEPTH=1`
restores the old behavior for comparison.
The user reports reduced, but still visible, overlapping-surface flicker with
this option. It is not a complete rendering fix. A lighting-table CPU-staging
experiment produced identical pixels across 48 matched titan frames and was
removed. Private software-rendered reference frames are being used to investigate
the remaining differences; ROMs and game captures are not shipped in the repo.


Native vertex positions now round screen X/Y to the software PICA rasterizer's
1/16-pixel grid, before converting back to clip coordinates. This applies to
both GPU-programmable and interpreted-vertex shaders, using the guest's viewport
corner and half-sizes. It retains GPU rendering and does not enable host JIT.
The user reported the physical-iPad vertex-rounding candidate was "much much
better" during the titan sequence. This is a visual improvement report, not proof
that every surface is correct. `CARA_UNSNAPPED_VERTICES=1` restores the old path
for controlled comparisons. The physical vertex fixture compares rounded
positions using a fractional viewport as well as register/branch/color results.
Direct-depth interpolation changed few pixels and the optional custom clipping
plane changed none of the 48 matched titan frames; those experiments were removed.


The final default candidate passed the 99-vertex physical GPU fixture, including
fractional-viewport rounding, and the integer-depth tie fixture. Zelda completed
1,200 display frames at 80.0278 FPS uncapped with no CPU vertex fallback or CPU
JIT, and a visually complete title scene. Rounding changes edge pixels, so this
capture is not pixel-identical to the older unrounded renderer. macOS regression
checks and audits of both linked products passed locally.


## Automatic idle-intro upload pressure

The user reported low FPS when the title screen automatically enters its intro
after being left idle. This differs from the three-A-press benchmark, which can
advance past portions of the sequence. The title must be left untouched when
reproducing this report.

An opt-in physical-iPad profile captured a 60-frame slow window at 35.142297 ms
per display frame. Vertex uploads consumed 9.321077 ms, with 3,147 fresh-storage
fallbacks in the window and all three 32 MiB banks full. GPU completion after
presentation took 0.813692 ms; surface downloads and shader compilation were
idle. The iPad reported nominal thermal state. This points to upload storage
pressure rather than expensive fragment lighting in that measured window.

Native upload banks pool textures by power-of-two height, with a 64 MiB payload
budget per bank (192 MiB total). Changing draw order reuses matching allocations
without resizing. Under pressure, unused tails in the largest size classes are
released first; inputs already submitted in the current frame remain untouched.
`CARA_VERTEX_UPLOAD_BANK_MIB` selects 16–128 MiB per bank for comparisons. These
limits count texel payload, not the driver's entire memory footprint. A rejected
per-draw-position capacity-retention experiment filled the larger budget and
increased fallback uploads; it is not included in the implementation.

Opt-in timing reports now separate surface downloads, shader compile/link time,
CPU surface invalidations, lit draws, texture storage allocations and upload
fallbacks. `CARA_GPU_WAIT_TIMING=1` additionally calls glFinish once per displayed
frame while timing is enabled. That diagnostic changes CPU/GPU overlap, so its
FPS must not be presented as ordinary-play performance. None of these timing
clocks or GPU waits run in the default launch.


## CPU interpreter overhead experiments

The supplied build commands now select `CITRA_INTERPRETER_GUEST_DEBUG=OFF`.
The core keeps its upstream guest-debugger path when that option is ON; use
`tools/build.py ... --guest-debugger` to request it. With polling omitted, the
ARM/Thumb state bit is synchronized at decoded-block boundaries instead of on
every instruction. Instruction-set transitions end a block, and explicit status
reads and interpreter exits still save the CPU flags.

A 1,024-entry, per-core direct-mapped cache precedes the existing decoded-block
hash map. Each entry stores the complete guest PC, a decoded-data offset and the
shared arena generation. A collision falls back to the original map, and global
invalidation rejects stale entries on every core. The cache contains no host
machine code. `CARA_DISABLE_FAST_DECODE_LOOKUP=1` restores hash lookups for
comparisons without changing interpreter execution.

The ROM-free CPU check now covers colliding cache tags, rewritten code after
invalidation, cross-core resets, ARM/Thumb transitions in one slice and through
single stepping, MRS/status flags and arithmetic loops. `--benchmark` adds
20-million-instruction branch and arithmetic microbenchmarks. The host release
variant without guest polling reduced arithmetic-test time from about 0.134 s
to 0.107 s. In a separate paired run, direct lookup reduced the branch test from
about 0.24–0.26 s to 0.19 s. These are host microbenchmarks, not iPad game FPS.
A template-based debugger-specialization prototype was slower than the original
host build and was rejected before device deployment.


Immediate arithmetic operands now use the same immediate shifter helper directly
instead of an indirect function-pointer call; register shifters retain their
existing path. The helper uses masked rotation counts so a zero-bit rotation
never shifts a 32-bit value by 32. All 4,096 ARM immediate encodings with both
incoming carry values (8,192 cases) are checked against an independent 64-bit
rotation oracle, including N/Z/C and preserved V flags.

A physical-iPad scheduling experiment successfully changed the emulation worker
from default QoS (21) to user-interactive QoS (33). The 24 matching heavy-intro
windows changed only from 30.042533 ms to 29.707080 ms on average, about 1%.
That scheduling experiment was not retained in the default implementation.
