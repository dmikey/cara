# Graphics performance prior art

Primary-source review: 2026-11-13. These implementations motivate experiments;
they do not establish performance gains for Cara. Preserve 100% guest CPU clock,
audio, FastInterp without JIT, and the selected GLES renderer during comparisons.

## Measured opportunity

The instrumented Stories capture is preserved under
`/Volumes/Fast/CaraBuild/profiles/resumed-draw-diagnostic/`. Analyze
`draws-emulator.log` using `tools/summarize_draw_cost.py --session 2`.
Session 1 has no usable interval. Session 2 snapshots 12–17 cover 327,680 draws
with no sampled clock errors:

| Draw path / submitted vertices | Calls | Sampled submission CPU | Mean CPU/call |
| --- | ---: | ---: | ---: |
| Accelerated / ≤64 | 22.52% | 19.63% | 3.230 µs |
| Accelerated / 65–256 | 6.82% | 3.42% | 1.860 µs |
| Accelerated / 257–1024 | 15.65% | 8.24% | 1.953 µs |
| Accelerated / 1025–4096 | 30.15% | 25.81% | 3.171 µs |
| Accelerated / >4096 | 21.16% | 41.44% | 7.260 µs |
| Software / ≤256 | 3.70% | 1.46% | 1.264–1.683 µs |

Large accelerated draws (>1024 vertices) represent 67.25% of sampled submission
CPU and 94.60% of submitted vertices. Small draws (≤256, including software)
represent 24.50% of submission CPU. Bucket contents differ in shader and state;
this does not prove causal scaling with vertex count.

The separate frame-timing file's last 900 refreshes, excluding the initial 300,
average 3.622 ms submission, 2.354 ms input/index subimage upload, and 0.641 ms
reuse comparison per refresh. Actual uploads average 302.93 calls and 2.962 MB,
including 0.779 MB indices. Overall diagnostic time is 23.792 ms/refresh.
These are instrumented host timings, not GPU execution or a speed comparison.
Samples retain clock overhead and have no variance estimates. Snapshot and frame
boundaries differ, so applying bucket shares to frame means is approximate.

## Applicable implementations and limits

- [Azahar GL rasterizer](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_opengl/gl_rasterizer.cpp#L417-L431)
  uses a fixed geometry shader for lit-draw quaternion correction and a trivial
  geometry shader for unlit draws. Its conventional vertex/index buffers cannot
  directly replace Cara's lit GLES3 input textures: triangle-first quaternion
  orientation must remain correct without geometry shaders.
- [Panda3DS accelerated vertex upload](https://github.com/wheremyfoodat/Panda3DS/blob/master/src/core/renderer_gl/renderer_gl.cpp#L1111-L1197)
  streams original attribute-loader ranges through vertex buffers, configures
  native byte/short/float attributes, and rebases indices when base-vertex drawing
  is unavailable. This is a concrete reference for an unlit native-attribute
  experiment; it does not prove Cara's lit-path correctness or Apple GLES gains.
  Host GLSL generation is separate from guest CPU JIT.
- [PPSSPP deferred primitives](https://github.com/hrydgard/ppsspp/blob/master/GPU/Common/DrawEngineCommon.cpp#L1285-L1305)
  track index bounds and decode growth and flush on incompatible topology,
  capacity or clipping state. Its [GL push buffers](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLMemory.cpp#L77-L107)
  accumulate uploads before recorded draws consume known buffer offsets. This
  supports investigating bounded packets with immutable inputs and explicit
  hazards. Current [iOS App Store configuration](https://github.com/hrydgard/ppsspp/blob/master/ppsspp_config.h)
  disables GL and [selects Vulkan](https://github.com/hrydgard/ppsspp/blob/master/Core/Config.cpp);
  its current iOS results are not evidence for Apple's GLES transport.
- [Dolphin persistent staging](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/OGL/OGLTexture.cpp#L108-L115)
  requires buffer storage, image-load/store barriers and sync support. Ordinary
  texture loading also retains CPU-pointer uploads. This is not justification
  to repeat Cara's rejected PBO experiment.
- [Azahar #2755](https://github.com/azahar-emu/azahar/pull/2755), merged November 3,
  changes an earlier GPU backlog limiter from one VBlank to one game frame to
  prevent non-60-fps games being forced through a 1/60-second GPU window. Audit
  synchronization policy before porting: no equivalent limiter has been
  identified in Cara; iOS `glFinish` is diagnostic-only under
  `CARA_GPU_WAIT_TIMING`.
- [Azahar #2627](https://github.com/azahar-emu/azahar/pull/2627), merged October 12,
  fixes a Vulkan upload skip that checked only the color-table dirty bit and
  omitted alpha-map updates. Partial uploads must account for every represented
  byte/table and storage invalidation.

Apple's archived [GLES design guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/OpenGLES_ProgrammingGuide/OpenGLESApplicationDesign/OpenGLESApplicationDesign.html)
recommends buffering to avoid resource update/use collisions and avoiding
unnecessary queries and synchronization. Its [concurrency guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/OpenGLES_ProgrammingGuide/ConcurrencyandOpenGLES/ConcurrencyandOpenGLES.html)
requires context ownership and buffered shared resources. Neither establishes
that extra staging improves current Apple drivers.

## Bounded next investigation

Measure input-only, index-only and simultaneous upload misses, separating
storage changes and fallbacks. For resident equal-sized shadows, measure changed
rows and the first-to-last changed-row span; sample upload CPU by byte size and
miss type. A possible follow-up is one contiguous changed-row update per
already-required upload, retaining current pools, exact shadows and fallback.
Admit implementation only if byte-dependent costs and unchanged rows suggest
at least 0.5 ms/refresh recoverable work. Combining input/index transport needs
substantial simultaneous misses; total update count does not establish that.

Before promotion, require byte-exact GPU readback across mutations and storage
wrap, surface-coherence tests, matching moving-replay pixels/workload, and
bracketed uninstrumented device comparisons with audio and thermal reporting.

Guest underclocking changes graphics-command production and memory writes as
well as instruction rate. It cannot isolate interpreter cost. Compare at fixed
guest clock with matched replay counts for instructions/ticks, draws, vertices,
uploads, guest-visible writes and audio progress before attributing a gain.


## CPU interpreter prior art and bounded candidates

Read-only CPU review: 2026-11-13. The source-backed mechanisms below use
precompiled handlers and decoded data; they require no generated executable code.
They motivate experiments, not a measured Cara speedup.

- [PPSSPP's IR interpreter](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/IR/IRInterpreter.cpp)
  uses threaded dispatch and warns that Clang can merge handler tails into a
  shared indirect branch. Its authors recommend inspecting the emitted branch
  sites. [Interpreter optimization passes](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/IR/IRPassSimplify.cpp)
  include constant propagation, copy/temp elimination, FP-move simplification
  and memory classification. These support bounded integer propagation and
  address-generation/load fusion, with state barriers preserved.
- [Dolphin's cached interpreter](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreter.cpp)
  emits an idle check for analyzer-classified idle loops and calls the guest
  timing system's idle operation only when the actual next PC returns to the
  idle start. This is a model for proven scheduler-aware idle recognition,
  not permission to skip arbitrary hot loops.
- [V8 DrumBrake's handlers](https://github.com/v8/v8/blob/main/src/wasm/interpreter/wasm-interpreter.cc)
  use mandatory tail calls carrying an integer and FP value, select static
  superinstructions, and use [register modes](https://github.com/v8/v8/blob/main/src/wasm/interpreter/wasm-interpreter.h)
  to reduce intermediate memory traffic. A register-cached integer subset is
  a possible later experiment; changing handler ABI, helper spills, exceptions
  and FP state is substantially riskier than local fusion.
- [Panda3DS](https://github.com/wheremyfoodat/Panda3DS) identifies Dynarmic as its
  CPU engine and [documents idle skipping](https://github.com/wheremyfoodat/Panda3DS/releases).
  This review did not establish a separate portable jitless CPU mechanism there,
  or a verified new Folium CPU optimization suitable for direct adoption.

### Aggregate findings from the captured decoded blocks

The frame-1200 `draws-fastinterp-blocks.json` under the diagnostic directory is
explicitly a resident block census, not time samples. It omits evicted blocks and
its entry counts do not establish instruction cost, idle time or recoverable
milliseconds. Only aggregate interpreted findings are retained here.

Among the 128 listed core-0 blocks, six end with an ARM branch back to their own
start; all six contain guest stores or counter updates. They implement pipelined
copy, fill, masking, gather/scatter or mixed integer/FP work. No pure polling or
idle loop was established in this sample. Frequently entered increment/compare
blocks also branch into larger loop bodies, rather than merely waiting.

| Candidate | Census coverage clue | Required constraints |
| --- | --- | --- |
| Load plus existing CMP/branch fusion | 13 syntactic sites; 15,461,356 entry-weighted occurrences | Start with unconditional ordinary-register loads; retain the memory helper, address/destination alias semantics, comparison flags and branch result. |
| Existing ADD/CMP-immediate/branch option | 8 immediate-CMP syntactic sites; 15,511,153 entry-weighted occurrences | Already implemented as `CARA_FASTINTERP_ADD_CMP_IMM`; measure this option rather than counting it as new work. |
| Static copy-iteration fusion | The most-entered listed block is a pipelined 32-byte copy iteration | Preserve stores before loads, conditional final loads, base writeback, overlap behavior and final register/flag state. Do not substitute unchecked host memcpy. |

These counts are potential coverage, not fully admitted fusion counts or
predicted speedups. Any candidate must retain original tick costs, decoded source
coverage/invalidation, original block boundaries and interrupt/scheduler budget
checks. Memory helpers must preserve MMIO, coherence and fallback behavior.
Dependent VFP accumulation also appears in hot blocks; independent SIMD grouping
cannot cover it without additional arithmetic and FP-state proof.

### Dispatch evidence and promotion limits

Read-only disassembly of the pre-pair iOS binary inspected during this review
found 157 indirect `br` sites inside `ExecuteBlock`; an older symbolized profile
binary had 158. Dispatch was not globally collapsed to one branch. However, the
four operand-specialized ADD/MOV handlers shared a writeback and advance tail,
so partial merging was real. Branch counts alone do not establish a bottleneck.
The earlier no-tail-merge/no-LTO experiment produced no stable win and remains
rejected; this source warning does not justify repeating it without new evidence.
These observations describe those inspected binaries, not later rebuilt apps.

Prioritize measured load/CMP/branch or copy-iteration coverage before a broad
handler rewrite. Require differential register/flag/tick tests, alias and
second-word invalidation checks, helper fallbacks, mixed budgets and block-boundary
oracles, then matched 100%-clock physical-device comparisons. Near-full speed at
50% guest clock does not prove a twofold interpreter unlock: it can also reduce
graphics commands and guest memory writes. No idle skipping is justified by the
current census alone.
