# Native Mac GPU depth reinterpretation

`CARA_MAC_GPU_DEPTH_REINTERPRET=1` selects an experimental GPU-only D24S8-to-RGBA8
converter on native Apple desktop OpenGL. It is disabled by default. iOS already
uses this converter; this experiment does not establish an iPad speed gain.

Apple OpenGL 4.1 lacks stencil texturing. The existing fallback reads depth/stencil
into a GPU pixel buffer and samples that buffer; it does not map those bytes to
the CPU. Native M1 gameplay samples nevertheless showed synchronization inside
that `glReadPixels` call. The candidate instead samples depth into RGBA8, then
recovers eight stencil bits using read-only fixed-function tests and additive
color passes. It uses nine draws and a final nearest blit, with no pixel-buffer
read or CPU readback during normal conversion.

The implementation retains the existing GLES converter, extent checks and
source ownership. Native GLSL 330 omits the ES precision preamble, which names an
unsupported image type. Embedded `highp` qualifiers remain legal in GLSL 330.
Desktop color logic operation and clip enables are preserved explicitly,
including raw state that differs from the renderer cache. Other reinterpretation
routes and native RAM upload/download paths remain unchanged.

## Correctness test

```sh
cmake --build /path/to/native-build --target cara-mac-depth-self-test
CARA_MAC_GPU_DEPTH_REINTERPRET=1 /path/to/native-build/bin/cara-mac-depth-self-test
```

The target requires an actual native Mac OpenGL context. Missing context support
is a failure, not a pass. Running with the experiment disabled also fails, so the
old desktop no-op oracle cannot masquerade as GPU execution.

On Apple M1, OpenGL 4.1 Metal 91.7 with stencil texturing unavailable, validation
passed every 16,777,216 D24 code for both D24 and D24S8, all 65,536 D16 codes,
stencil patterns, offset/scaled rectangles, partial uploads, untouched source and
destination regions, and raster-state restoration. An additional 65,536-texel
native integer upload compares the legacy PBO and candidate outputs byte for
byte, including offset/scaled and repeated conversions. This complements the
exhaustive oracle's fragment-written exact depth seeds.

The tests perform explicit readbacks; normal conversion does not. The matched
native gameplay measurements below support a small wall-time reduction, but do
not establish a CPU-time improvement. The candidate remains disabled by default.

References: [GLSL 3.30 precision qualifiers](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.3.30.pdf)
and [OpenGL stencil texture behavior](https://wikis.khronos.org/opengl/Stencil_Image_Format).

## Matched native gameplay measurement (2026-11-13)

Eight sequential runs used one immutable private executable on Apple M1
(Macmini9,1), OpenGL 4.1 Metal 91.7. This was a private `2565b2b` source basis,
**not latest main**, with the reviewed VFP vector/bank-zero repair, native SDL and
checked GL 4.1 copy support, LCD upload repair, Apple shader switch lowering,
VLDR-pair candidate, Mac depth candidate and private diagnostic controls. The
production-equivalent changes correspond to `f9f9a34`, `a49c473`, `fdf7ff0`,
`e93b133`, `e334ac8` and `fb09`. Native presentation used swap interval zero and
Cara's unchanged 100% core frame cap. The Release build retained optimization;
it did not use LTO.

The executable SHA-256 was
`805638fc154044d4fb5539049734175dabc7de40aea2acaee5edbc0238f54d04`;
its state fingerprint was
`1414da400d4eba11aabfe7dad3dcfc3dfd9545b26faa1d16c5005366fc3434ae`.
A freshly reached Monster Hunter Stories Secret Path checkpoint was created
under that executable at VideoCore frame 9750 / guest tick 43568197269. State
loads retained the normal fingerprint checks. Each fresh process loaded this
same checkpoint, warmed up for 300 refreshes, then measured 900 refreshes
(relative frame interval [300,1200)). Identical movement inputs were left
[360,480), right [600,720) and up [840,960). The final capture at 1201 followed
the measured interval. Cold shader/surface reconstruction was excluded equally;
the converter program was constructed before measurement.

Every run retained Legacy 3DS mode, FastInterp, CPU and shader JIT disabled,
100% guest clock, 100% cap and SDL sound. All four timers reported scale 1,
effective clock 100%, downcount hack shift 0 and divisor 1. The core cache was
32,768 entries / eight ways; normal AL/RAM/tick/chain specializations remained
identical. Native SDL audio opened a stereo 32,728 Hz device through CoreAudio.
This confirms the device opened, not physical listening or smooth output.

Private diagnostic mode retained input handling, read-only boundary snapshots
and captured register state. Per-instruction guest-PC profiling was disabled,
and timing/counter scopes were explicitly disabled. These are not fully
uninstrumented runs. The clock deltas exclude the beginning snapshot's file I/O
and the final screenshot readback. Wall time includes driver waits; thread CPU
time measures only the emulation thread, not all process or GPU work. Neither
column is a direct playback-FPS measurement.

| Run order | VLDR pairs | Mac GPU depth | GL state reuse | Wall ms/refresh | Emulation-thread CPU ms/refresh |
|---|---:|---:|---:|---:|---:|
| Baseline A | 0 | 0 | 0 | 74.0785 | 50.6052 |
| VLDR only | 1 | 0 | 0 | 73.4705 | 50.1621 |
| GPU only A | 0 | 1 | 0 | 69.6200 | 50.3031 |
| Stacked | 1 | 1 | 0 | 69.5162 | 50.1984 |
| Baseline B | 0 | 0 | 0 | 72.6327 | 50.4979 |
| GPU only B | 0 | 1 | 0 | 70.4175 | 50.7429 |
| Baseline C | 0 | 0 | 0 | 73.8822 | 51.4077 |
| GL state reuse exploration | 0 | 0 | 1 | 73.0217 | 49.5294 |

For GPU only B, the mean of its adjacent baselines B/C was 73.2575 ms wall and
50.9528 ms thread CPU per refresh. The candidate was lower by **2.8400 ms wall
(3.88%)** and **0.2099 ms CPU (0.41%)**. The GPU-only two-run means were 70.0187
ms wall / 50.5230 ms CPU; the three baseline means were 73.5311 / 50.8369.
The repeated wall reduction is consistent with less native driver
synchronization. The small CPU difference does not establish a CPU gain. This
short series is not a promotion decision, and the stacked result does not prove
additive benefit. VLDR's single point is within observed baseline CPU drift.
The GL state reuse result is exploratory, unreplicated and not promoted; iOS
already enables that option.

All eight runs matched final full RGBA bytes (SHA-256
`40e9cc7ab350e5da1d67cb7f21bb16efa14a869866c7be7e33d4073f8c10d78a`),
all 16 captured ARM registers, CPSR, FPSCR and 64 raw VFP words on each core,
timer configuration and final actual `GetTicks()` value **48934882999**. The
endpoint remained actual Secret Path gameplay. This is captured state and pixel
parity against the native baseline, not a full RAM/banked/CP15-state proof or
hardware-rendering correctness claim. Legacy-mode `GetGlobalTicks()` remained
zero and was not used as the progression check.

Draw-count parity was **not measured** because counters were disabled. Audio
underrun counts were unavailable; no zero-underrun or physical audio-smoothness
claim follows. The existing desktop PBO fallback transfers GPU bytes into a
sampler buffer without CPU mapping; its `glReadPixels` call is not evidence of
CPU byte conversion. This native experiment is not an iPad measurement: the
GLES path already uses the GPU converter, and the desktop driver path differs.

Raw local evidence is archived under the profiling output's `combined/` tree:
`binary-receipt.json`, `checkpoint-save-receipt.json`, per-run `launch.json`,
`result.json`, boundary profiles, capture bytes and `native.log`, plus
`matrix-repeat-verified.json` and `matrix-gl-state-results.json`. The artifact
root is environment-specific and is not a repository input or bundled game asset.
