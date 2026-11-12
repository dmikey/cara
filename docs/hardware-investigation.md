# Hardware investigation

Last updated: 2026-11-12.

Working record for the static, no-host-JIT iOS renderer. Keep measured behavior,
upstream findings, and proposed explanations separate. Update this document when
an experiment changes the diagnosis; a higher speed counter alone is insufficient.

## Targets and clocks

- Target measured here: physical M4 iPad 11, GLES3, FastInterp and sound enabled.
  The runtime reports 7,817,756,672 bytes of physical memory; this is the OS
  reported value, not a SKU-capacity assertion. Sustained GPU frequency is not
  recorded.
- The guest CPU and the host CPU are different execution architectures. FastInterp
  executes precompiled handlers and caches decoded instruction data. It does not
  generate executable ARM64 code from a game.
- PICA vertex programs translated into GPU shaders are a separate path from host
  CPU JIT. Preserve hardware vertex and fragment processing while reducing their
  CPU staging, conversion and synchronization costs.
- Cara's scheduling reference is `BASE_CLOCK_RATE_ARM11 = 268111856` in
  `emulator/src/core/core_timing.h`. Display refresh accounting uses approximately
  59.834 Hz in `emulator/src/core/hw/gpu.cpp`. These are emulator implementation
  constants, not measurements of the iPad clock.
- The HUD's **guest CPU 50%** is an emulated clock setting. It is neither M4 CPU
  utilization nor GPU utilization. Adaptive mode currently reacts to total
  `RunLoop()` work, including rendering. It can reach its floor while rendering
  remains expensive. Do not identify that floor as the cause of a slowdown without
  an adaptive-off, fixed-clock comparison.
- **100% speed** means native emulated refresh throughput at the selected guest
  clock. It does not establish native guest CPU performance at a reduced clock,
  or 60 unique game-animation frames per second.
- Xenoblade reached approximately full emulation speed in the tested sequence.
  That establishes a useful workload baseline, not a guarantee for every game's
  CPU instruction mix or GPU workload.

## Measurement rules

1. Record title ID, scene, progression, guest clock, adaptive setting, audio,
   renderer, engine revision, pacing and thermal state.
2. Separate opening videos from interactive gameplay. Monster Hunter Stories'
   opening is FMV. The user's entry sequence was **A three times, then hold
   Start**; confirm the resulting screen before taking gameplay measurements. Dialogue
   afterwards also waits for A; a red downward prompt is visible. A direct
   developer-tools screenshot confirmed that this was a dialogue wait, not a
   frozen framebuffer.
3. `guest_cpu_ms_per_frame` wraps guest execution plus synchronous HLE/rendering
   calls. It is not isolated ARM interpreter time. Draw timing includes nested
   upload and submission phases. Never add nested timings together as independent
   costs.
4. Keep profiling readbacks out of normal runs. `CARA_GPU_WAIT_TIMING=1` inserts
   `glFinish()` and changes overlap; its result is the remaining completion wait,
   not total GPU execution time. Earlier uploads/readbacks can already synchronize.
5. Compare equivalent windows and draw/vertex counts. Matching refresh numbers
   alone do not establish deterministic replay or identical game progression.
6. Keep commercial ROMs, saves, captures and raw game buffers private. Commit
   original regression fixtures, scalar results and source provenance.

## Confirmed M4 GLES observations

| Observation | Evidence and interpretation |
|---|---|
| `GL_MAX_TEXTURE_SIZE = 16384` | Read on the physical iPad. Check actual device limits at runtime. |
| Vertex input atlas width is 128 RGBA32F texels | Current implementation choice, not a hardware constraint. |
| Expanded vertex policy is 65,536 | A full sixteen-register layout uses 8,192 rows / 16 MiB. The largest complete triangle list below this cap has 65,535 vertices. |
| Full-register theoretical atlas capacity is 131,072 vertices / 32 MiB | Derived from the measured texture limit and current packing. It is not a validated single-draw policy or a claim about all GPU resources. |
| Large transform-feedback captures returned zero outputs starting at vertex 4,092 | Also reproduced using a synthetic vertex-ID shader. Nonzero draw starts affected capture too. This is an observed capture anomaly, not proof of a hardware vertex-processing limit. |
| Bounded feedback captures validate the full atlas | 3,072 vertices per capture with an explicit logical vertex base; all outputs in 41,967- and 65,535-vertex fixtures are compared with the interpreter. Regular rendering does not use transform feedback. |

The upload pool retains three frame banks with an existing 64 MiB budget per
bank. This is a pool budget, not an upper bound on total process/GPU memory or
in-flight driver allocations.

## GLES input architecture

The iOS GLES path expands triangle indices on the CPU, preserving each triangle's
logical vertex ordinal. Indexed triangles and strips/fans now upload each distinct
source vertex's packed float4 attributes once per draw, plus an R16UI lookup
texture. The fused GPU shader resolves that lookup before fetching attributes.
Ordinary non-indexed triangle lists retain the dense input path.

The fused vertex shader reruns the first vertex of each triangle when needed to
align quaternion signs before interpolation. The new representation preserves
that ordinal, first-vertex behavior, shader evaluation and clipping/depth rules.
It reduces repeated CPU packing and transfer data; it does not implement a native
indexed draw or cache transformed vertex outputs.

A bounded u16 source map uses per-draw epochs, including an explicit wrap reset.
Input storage and index storage both count against the existing three 64 MiB
upload banks. Dense and indexed size classes remain separate. Unsupported draws
retain the existing CPU fallback; pool exhaustion retains orphan uploads.
`CARA_GPU_INDEXED_INPUTS=0` selects the earlier dense representation for diagnostic
comparisons. The indexed path is enabled by default. Raw-byte GPU decoding is a
separate, later step with additional format tests.

## Dark Moon: measured rendering costs

Title `0004000000055F00`. A 41,967-vertex draw exceeded the old 16,384-vertex
policy, producing four CPU vertex fallbacks per refresh. Raising the supported
GPU batch limit removed those fallbacks; CPU vertex shader time fell from about
17.7 ms per refresh to zero. Earlier and later scene observations showed roughly
51.1 ms versus 26.8 ms per refresh; those were not deterministic matched replays.

Comparable boot windows ending at refreshes 720 through 1,200, all at a 50% guest
clock, then measured:

| Candidate | Refresh time | Vertex preparation | Upload | Approximate speed |
|---|---:|---:|---:|---:|
| Large GPU batches | 27.16 ms | 10.03 ms | 3.93 ms | 61.5% |
| Direct physical RAM lookup | 25.05 ms | 7.14 ms | 3.95 ms | 66.7% |
| Direct lookup + packed inputs, final safe cache | 24.16 ms | 6.18 ms | 3.97 ms | 69.2% |

Each 60-refresh window had 32,720 GPU draws and about 17.785 million expanded
vertices. Surface downloads still cost about 4–4.6 ms per refresh. Vertex counts
varied slightly, so these are comparable observations, not exact replay proof.
The user confirmed correct scenery, lighting and sound on the packed-input build.

The final RAM cache stores an atomic region ID and resolves immutable descriptors
that borrow owner fields. Backing sizes are validated and descriptors rebuilt
when loading a state. Do not cache raw backing pointers across state restoration,
or share a mutable pointer/start/end tuple between renderer workers. Four-worker
lookup, bounds, aliasing and save-state regressions cover this path.

A separate forced-completion diagnostic added about 1.45 ms of end-of-refresh
GPU waiting. Do not subtract that from total refresh time and call the remainder
CPU-only execution. The diagnostic was disabled again afterward.

Engine revision for the final safe-cache result:
`ede7c0fb7f53babe8c11f43fd5de3a334e793dc0`.
[All three GitHub jobs passed](https://github.com/dmikey/cara/actions/runs/43151562488).
Private scalar evidence is retained in `outputs/evidence/cara-render-input-validation.json`.

## Monster Hunter Stories: current leads

Title `00040000001BC500`, confirmed in the loader/session log.

- The non-profiled session lowered adaptive guest CPU from 100% to 50% after
  sustained overload. This establishes controller behavior, not isolated CPU load.
- That log contained 1,853 `Missing pixel_format reinterpreter: 17 -> 0` warnings.
  In Cara's surface enum this is **D24S8 depth/stencil to RGBA8**.
- `FormatReinterpreterOpenGL` returns early for `CARA_IOS_GLES`, so the desktop
  conversion shaders are not registered on iOS. They cannot simply be enabled:
  their stencil sampling / texture-buffer or texture-view assumptions must be
  adapted to the actual GLES implementation.
- The fallback attempts surface validation through RAM. GLES depth flushes have
  a separate restriction, so a warning alone does not prove a CPU readback or its
  cost, and can also indicate a correctness problem. Trace the selected path,
  conversion frequency, bytes and timing in actual gameplay.
- Opening-FMV timings are not a substitute for that gameplay profile. The user
  entered the game after the profiling restart; capture that session's windows.

Interactive gameplay was captured after the user advanced past FMV. The last
8 windows in this baseline averaged 32.05 ms per refresh, with 6.00 ms preparing
vertices and 16.32 ms inside vertex-upload GL calls. CPU vertex interpretation
cost only 0.109 ms; surface-download calls were zero in these windows. These
nested timings do not isolate driver CPU execution from GPU synchronization.

The three 64 MiB upload banks were all full (192 MiB retained pool storage).
Per refresh, about 284 GPU draws produced 138 orphan-upload fallbacks and 160
storage allocations. This is a concrete allocation-churn lead, separate from the
missing format conversion. A controlled trial uses the existing
`CARA_VERTEX_UPLOAD_BANK_MIB=128` setting (up to 384 MiB retained pool storage),
while recording allocation counts, upload duration, game workload, footprint and
thermal state. A faster counter without comparable gameplay is not sufficient.

The 128 MiB-bank trial reduced sampled upload time to 8.49 ms, with about 28
orphan fallbacks per refresh instead of 138. Retained pool storage reached
384 MiB and sampled process footprint was about 1.08 GiB. These were not exact
matched frames: one window ended at 70% guest clock and vertex workload differed.

The opening scene was initially described as frozen: the image stopped while the
HUD, guest PCs and draw submissions continued. It reproduced with 64 MiB banks,
a full guest clock, and both FastInterp and DynCom. **The user then confirmed that
pressing A advances the scene in both engines. This was an input/dialogue wait,
not a reproduced emulator hang.** Do not use it as evidence that either CPU backend,
cache capacity, or underclocking caused a freeze. Verify prompts and game input
before diagnosing a hardware deadlock.

The fresh 64 MiB / 100% guest-clock input-wait control averaged 100.88 ms per
refresh: 18.74 ms preparation, 59.55 ms upload, 0.705 ms CPU vertex shading,
approximately 890 GPU draws and 2.213 million expanded vertices per refresh.
These numbers show that the stationary scene can still submit substantial GPU
work, but they are not the requested moving-gameplay baseline. A fixed-clock
FastInterp comparison in an advancing 3D scene is required next.

The upstream [Stories equipment-upgrade report](https://github.com/azahar-emu/azahar/issues/195)
was closed as stale without a confirmed fix. Its trigger differs from this opening
scene and is not the input wait observed here.
[Azahar #2272](https://github.com/azahar-emu/azahar/pull/2272) implements
`APT::GetAppletProgramInfo` and fixes applet framebuffer capture; the unimplemented
APT call occurs in Cara's log, but no evidence connects it to the measured slowdown.

Upstream also has a
[D24S8-to-RGBA8 GPU shader](https://github.com/azahar-emu/azahar/blob/master/src/video_core/host_shaders/format_reinterpreter/d24s8_to_rgba8.frag).
It samples depth and stencil separately. The legacy equivalent exists in Cara,
but its desktop stencil-texturing assumptions are not a portable GLES3 iOS fix.
Adapt and validate that operation rather than enabling unsupported API calls.

Earlier runtime reports lacked an embedded engine revision and CPU-backend name.
The focused upload diagnostic now records the selected backend, SCM revision,
engine state/build fingerprint, host physical RAM and upload-bank budget. The SCM
revision can describe the base of a modified checkout; preserve the actual build
fingerprint and launch settings instead of inferring installed-code identity from
checkout HEAD alone.

### Focused upload diagnostics

`CARA_FRAME_TIMING=1` records total upload bytes, orphan-upload bytes, actual
`glTexImage2D` / `glTexSubImage2D` call counts, and separate elapsed scopes for
orphan upload, storage allocation with no input data, and pooled subimage updates.
These scopes are nested inside total upload time and can include driver/GPU waits.
They do not alone establish CPU busy time or total GPU execution time.

`CARA_VERTEX_REUSE_TRACE=1` additionally counts distinct source vertex indices
within each accepted input preparation. It records expanded versus unique counts
and their packed-attribute byte totals. This probe adds CPU work; use it to choose
an input-layout experiment, then turn it off for performance comparisons. A
unique-input upload would also need an index stream, row padding and a validated
GPU lookup; the byte difference is an opportunity estimate, not a measured speedup.

At the user's confirmed advancing 3D scene with FastInterp, 64 MiB banks and a
100% guest clock, eight windows averaged 72.94 ms per refresh: 41.06 ms upload,
12.75 ms vertex preparation, 6.39 ms draw submission and 0.213 ms CPU vertex
interpretation. About 24,895 of 33,704 GPU draws per 60-refresh window used the
orphan path. Scenes and camera changed, so this is a diagnosis of that sample,
not a matched performance comparison against the earlier adaptive-clock runs.

### Verified Cheval-scene reuse sample

A direct device screenshot identified the requested Cheval dialogue scene. Five
consecutive 60-refresh windows had 25,400 GPU draws each, a 100% guest clock,
FastInterp selected, 64 MiB banks and no thermal warning. With the reuse probe on:

- Mean refresh: 59.24 ms; preparation: 12.05 ms; total upload: 29.57 ms.
- Orphan-storage upload: 26.67 ms; pooled subimage update: 2.87 ms; fresh empty
  storage allocation: 0.004 ms. All are elapsed GL-call scopes, not CPU samples.
- Actual attribute transfer: 9,298,329,600 bytes per 60-refresh window, or about
  **148 MiB per refresh**.
- Expanded inputs: 65,565,600 vertices per window; distinct source vertices:
  10,695,200, or **16.3% unique**. Invalid probe draws: zero.
- Packed unique attributes plus a modeled 32-bit index stream would be about
  **28 MiB per refresh**, before new row padding: approximately 81% fewer transfer
  bytes. This is a layout estimate, not a measured implementation or FPS gain.

The upload path, not CPU shader interpretation, dominates this sample. Reusing
unique inputs is a more fundamental target than continually increasing retained
pool memory. Preserve first-vertex quaternion alignment and the expanded draw
ordinal when testing the new representation. Compare performance with the probe
disabled after correctness validation.

### Indexed-input implementation: physical-device validation

The implementation was first tested with `CARA_GPU_INDEXED_INPUTS=1`, with the
reuse probe disabled. The user confirmed that Stories is playable with adaptive
guest CPU and that scenery, music and voices remain correct. This is a specific
scene/device observation, not full-game compatibility or fixed-clock full speed.

The initial adaptive sample ended at a 50% guest clock. Five recent windows
included one 60% window and four 50% windows: mean refresh time 20.09 ms,
preparation 4.07 ms and upload 6.26 ms. There were no orphan fallbacks or fresh
storage allocations in those windows, with the original 192 MiB total pool budget.
Mixed guest clocks and different draw counts prevent a matched timing comparison
against the earlier 100% dense sample. The HUD capture showed 88% speed at a 50%
guest clock; do not describe it as 100% guest CPU execution.

A subsequent adaptive-off sample in the same dialogue had a much larger workload:
about 2.29 million expanded vertices per refresh. Five windows at a 100% guest
clock averaged 60.82 ms, including 13.39 ms preparation and 24.28 ms upload.
Transfer volume averaged approximately 45.2 MiB per refresh, including the index
texture. Some orphan fallbacks returned. This establishes that reducing duplicate
inputs does not eliminate overload at full guest clock; it is not an exact replay
of the earlier 1.09-million-vertex sample. Adaptive mode was restored afterward.

The M4 GPU oracle passed dense fixtures and indexed 41,967-vertex / 257-source
and 65,535-vertex / 65,535-source fixtures, including permuted records, quaternion
alignment, colors, clipping and packed inputs. Native tests cover repeat reuse,
reset, the full u16 index domain and epoch wrap. The ARM/Thumb differential and
old/New-3DS save-state regressions passed, as did the iOS no-host-JIT symbol audit.

Private raw reports are retained under `/private/tmp/cara-stories-indexed-*`;
commercial screenshots and game data are not committed. A future speedup claim
needs both representations measured with the reuse probe off, identical clock
policy and a restored checkpoint or otherwise equivalent draw workload.

### Packed-byte decoding experiment

The iOS frontend now enables packed inputs by default;
`CARA_GPU_PACKED_INPUTS=0` restores indexed float inputs for comparisons. It merges overlapping
physical byte ranges, copies interleaved source bytes without per-vertex conversion,
and supplies offsets, strides, formats and component counts to the GPU. The shader
reads an RGBA32UI texture and decodes signed byte, unsigned byte, signed short and
float32 values. Default attributes are constant records. Expanded triangle ordinals
and quaternion alignment remain unchanged.

Sparse ranges are bounded to 8 MiB and cannot exceed decoded unique-input storage
plus the constant records. Unsupported or excessive ranges use the indexed-float
path. Float and integer textures occupy separate pool classes. The iOS frontend uses
80 MiB per bank when packed inputs are enabled and reported physical RAM is at
least 6 GiB; lower-memory devices retain 64 MiB. Explicit launch overrides take
precedence. Host CPU executable code is never generated.

`CARA_PACKED_VERIFY=1` compares staged inputs with VertexLoader for every unique
source vertex of each accepted draw. It adds significant CPU work and must be off
for timing. The physical M4 passed an independent 3,072-vertex format/alignment
oracle and the large indexed shader fixtures through the packed decoder. A rendered
Stories sample reached 366,426 verified draws with zero mismatches. The user
confirmed the scene looked correct before capturing the comparison checkpoint.

`CARA_PROFILE_CHECKPOINT=1` exposes a dedicated capture action; it writes
`profile-comparison.carastate` without using normal save slots. `CARA_PROFILE_LOAD=1`
restores that checkpoint at launch using the usual title/build/backend validation.
Compare both representations in the same binary at fixed 100% guest clock, with
verification disabled and equivalent warm-up/measurement windows. A bounded run
pauses at its end; that deliberate pause is not a game freeze.

Prior art: the inherited desktop renderer and
[Azahar OpenGL vertex setup](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_opengl/gl_rasterizer.cpp)
pass packed attributes to native GPU vertex fetch. Cara's GLES adaptation uses
shader byte fetching to preserve access to the triangle's first vertex. This is
separate from PICA shader decompilation, which was already accelerated.

#### Matched Stories checkpoint results

The checkpoint's actual screen, verified by screenshots, is Lilia's **“Hey you two!
Over here!”** dialogue in the rendered opening, not the requested Cheval line.
All following runs restore the same checkpoint in one engine build, use a 100%
guest clock, and measure refreshes 301–1200 after 300 warm-up refreshes. The initial
series muted audio output while continuing DSP and audio callbacks; audible-output
confirmation is recorded in the private evidence report. No decoder shadow checks
run during the timing comparison.

| Path / per-bank cap | Mean refresh time | Input preparation | Upload calls | Upload fallbacks per refresh |
|---|---:|---:|---:|---:|
| Indexed float / 64 MiB, two runs | 49.85 / 49.91 ms | 11.78 / 11.80 ms | 19.88 / 19.85 ms | 40.67 |
| Packed GPU / 64 MiB, two runs | 41.29 / 41.56 ms | 5.62 / 5.68 ms | 15.77 / 15.87 ms | 3.20 |
| Packed GPU / 128 MiB, two runs | 39.78 / 40.58 ms | 5.50 / 5.72 ms | 15.39 / 15.57 ms | 0 |
| Packed GPU / 96 MiB | 40.30 ms | 5.61 ms | 15.49 ms | 0 |
| Indexed float / 64 MiB, screenshot control | 49.42 ms | See private report | See private report | 40.67 |
| Packed GPU / 80 MiB, screenshot control | 40.73 ms | 5.74 ms | 15.65 ms | 0 |

Each measured series had identical draw/vertex workloads: averages of 47,203.33
GPU draws and 118,640,268.4 expanded vertices per 60-refresh window. Input transfer
fell from 2,555,966,498 to 899,113,745 bytes per window (64.8% less). The two initial
64 MiB comparisons show approximately 20% higher refresh throughput; the selected
80 MiB screenshot pair improves throughput by 21.3%. This heavy fixed-clock scene
still runs below full speed. Refresh throughput is not unique animation FPS.

The full RGB screenshots at refresh 1200 are byte-identical between indexed float
and packed GPU decoding. This is a strong check for the captured frame, alongside
the GPU oracle and live input verification, not full-game compatibility proof.
Larger pool caps retained about 207 MiB total in this scene versus the former
192 MiB cap; 80 MiB per bank removes fallbacks without adopting the 128 MiB limit.
Small differences between 80/96/128 MiB runs do not establish a reliable speed
ranking, so the smallest tested cap that eliminates fallbacks is selected.

`tools/benchmark_gpu_inputs.py` automates checkpoint backup, process restart,
fixed-clock replay, warm-up exclusion, report collection, optional screenshots,
workload matching and restoration of adaptive play. Outputs must remain private;
engine compatibility checks are retained. The benchmark engine fingerprint is
`49815e4f2669e0432549caa0fcd35edf742e966c6a1f2417dcc67e7902dbeb5c`.
Frontend default changes do not change that engine fingerprint or invalidate this
checkpoint. Engine changes may still require recreating it; do not override checks.

#### Audible replay and scripted progression

The final audible-output checkpoint pair measured **49.61 ms indexed float versus
40.85 ms packed GPU**, a 21.4% throughput improvement at 100% guest clock. Both
reported active speaker output, nonzero audio samples and no audio-backend error.
Their RGB screenshots at refresh 1200 are identical. This does not prove
subjectively glitch-free audio at reduced emulation speed.

A separate pair replayed A presses at refreshes 300, 600 and 900, then measured
refreshes 301–1800 through later dialogue/camera changes. It measured **38.45 versus
32.49 ms**, an 18.3% throughput improvement. Draw and expanded-vertex counts match,
and the final RGB screenshots are identical. This extends the check beyond the
initial stationary dialogue, but does not cover every area or battle.

#### Candidate architectural limits

Input deduplication does not cache transformed GPU vertices. In the initial
checkpoint, 118.64 million expanded inputs correspond to 19.94 million distinct
source records per 60-refresh window. The fused shader additionally reruns the
first triangle vertex for the other two vertices. GPU transform reuse is therefore
a promising next experiment, but these counts alone do not establish GPU execution
time or a whole-frame speedup. A pre-transform pass must preserve output precision,
quaternion alignment, clipping and guest memory coherence; its extra submissions
and render-target switches may cost more than it saves on small draws.

`CARA_HOST_CPU_TIMING=1` samples the emulation thread's CPU clock and whole-process
user+system CPU time once per timing window. These are separate from elapsed phase
scopes: thread CPU includes interpreter, renderer and driver work on that thread;
process CPU sums work across all app threads and can exceed elapsed time. A gap
between thread CPU and elapsed time indicates off-CPU time, not uniquely GPU work.
The GPU fixture report also records draw-buffer and color-attachment limits for
planning a GLES pre-transform experiment. Keep these measurements separate from
Instruments sampling, which remains unavailable in the earlier USB test.

### Resident vertex-input experiment

`CARA_VERTEX_UPLOAD_REUSE=1` retains exact CPU byte shadows with each pooled
packed-input/index texture. Before uploading, it compares the current complete
payload against that texture's shadow and skips only matching writes. Shader
uniforms, layout, draw order and transforms still update normally. This does not
cache transformed vertices or depend on guest RAM dirty tracking. Reallocation,
new textures and orphan fallback force fresh uploads; pool eviction releases both
shadows. Padding is initialized so irrelevant bytes cannot cause false misses.
Shadow payload sizes cannot exceed the corresponding texture storage; vector
capacity may retain allocation growth. Pool entry and dimension limits bound
this additional host memory, which is not included in the GPU pool counter.

The preceding packed-decoder comparison measured 49.559 ms elapsed / 44.980 ms
emulation-thread CPU / 48.080 ms process CPU for indexed floats, versus
40.840 / 36.273 / 37.992 ms for packed bytes. Thread CPU includes renderer and
GL driver work as well as ARM interpretation; these are not GPU-utilization
measurements or a sampled call graph. This motivates testing fewer driver uploads.

An initial resident-input comparison was rejected: the user was pressing A,
and the two runs reached different dialogue/camera moments. Its apparent speed
improvement is not a matched result. Benchmarks now use explicit scripted input
and suppress skin game inputs while measuring. The menu remains available, and
normal touch controls return when normal play is relaunched.

For deterministic advancing-scene comparisons, use
`tools/benchmark_gpu_inputs.py --press-a-frames 300,600,900 --frames 1800
--host-cpu-timing --screenshots` with packed 80 MiB configurations differing only
in `reuse`. Each A press lasts six emulated refreshes. Compare draw/vertex counts,
engine fingerprints, final pixels, audio status, memory and thermal state before
selecting a default. Never count an FMV or mismatched camera as the same workload.

Engine changes still invalidate old build-specific checkpoints. The diagnostic
`CARA_PROFILE_SAVE_FRAME` can capture a fresh compatible checkpoint automatically;
`CARA_TEST_HOLD_START_FRAME` holds Start for 120 refreshes. Both default off.
For this build, a boot run with A at 300/600/900 and Start at 1200 first captured
at frame 2400; a second run from that intermediate checkpoint with the same
schedule reached the rendered opening and captured again at 2400. Verify the
actual screenshot and checkpoint file before timing. Do not bypass compatibility
checks or publish the private checkpoint.

The isolated, scripted 1,800-refresh comparison (first 300 excluded, sound on,
100% guest CPU) matched all draw/vertex counts and final full RGB pixels:

| Path | Elapsed per refresh | Upload | CPU comparison/copy | Emulation-thread CPU |
|---|---:|---:|---:|---:|
| Packed inputs, upload every use | 40.632 ms | 14.818 ms | 0 ms | 35.654 ms |
| Packed inputs, exact upload reuse | 25.278 ms | 0.416 ms | 0.536 ms | 22.408 ms |

This is 60.7% higher throughput in the measured advancing opening sequence,
not a claim of locked 60 FPS or full-game compatibility. Uploaded bytes fell from
853.65 MB to 26.38 MB per 60-refresh window (96.9% fewer). Each window averaged
44,888.2 GPU draws and 113,006,607.24 expanded vertices in both paths. Process
footprint at the end rose from 1,019,561,496 to 1,116,161,608 bytes (about 92 MiB);
thermal state remained nominal. Audio was active during both runs. CPU staging
still happens: this avoids redundant uploads, not all CPU vertex preparation.

The iOS frontend enables reuse on devices reporting at least 6 GiB physical RAM
when packed inputs are enabled. Smaller devices retain the no-shadow path until
measured. `CARA_VERTEX_UPLOAD_REUSE=0` explicitly disables it for comparisons.
The core remains opt-in through the same environment setting.

### Grass gameplay replay

The Secret Path grass scene is a separate, user-captured gameplay checkpoint,
not the opening dialogue. The private Slot 1 file was copied and verified before
restarting. An initial launch was rejected by the title check because the recent
bookmark pointed to Dark Moon; subsequent runs pin the existing Stories filename
with `--rom 'Monster Hunter Stories (USA).3ds'`.

The frontend now reapplies its selected CPU policy immediately after loading a
profiling state. Otherwise a state captured with adaptive CPU at 50% could retain
that scaling while the newly initialized HUD says 100%. The earlier opening
checkpoints were captured at 100%; this correction is required for the grass
checkpoint. Save-state compatibility checks remain unchanged.

`--circle-segments 300:600:0:1,600:900:0:-1,900:1200:0:1` scripts walking, with neutral
input between segments. Segments must be ordered, nonoverlapping and within the
run; axes must be finite and in [-1,1]. The screenshot confirms movement into
the Secret Path clearing. With first 300 refreshes excluded and sound enabled:

| Grass gameplay, 100% guest clock | Elapsed | Emulation-thread CPU | Input preparation | Upload |
|---|---:|---:|---:|---:|
| Packed inputs, upload every use | 48.480 ms | 43.686 ms | 6.357 ms | 18.486 ms |
| Packed inputs, exact upload reuse | 33.539 ms | 28.987 ms | 6.483 ms | 2.379 ms |

Both runs matched 54,913.6 GPU draws and 137,045,135.2 expanded vertices per
60-refresh window, with identical final full RGB screenshots. Reuse avoided
869.85 MB of the 1,042.57 MB upload payload per window, but movement changed more
inputs than the stationary scene. Thermal state remained nominal; both audio
reports were unmuted with last_error=0. The 44.6% throughput gain does not reach
native refresh speed. Most remaining elapsed time is still CPU work on the
emulation thread; guest_cpu_ms includes synchronous renderer/service calls and
must not be presented as pure ARM interpretation time.

The next candidate is exact-byte-validated reuse of prepared triangle/index
streams. This targets the measured preparation cost while leaving guest clock,
shader evaluation, effects and draw order intact. It is not implemented in this
measurement. Keep the grass checkpoint and the opening checkpoint separately.

### Device tooling

CoreDevice screenshot capture works without adding GPU readback to Cara:

```sh
xcrun devicectl device capture screenshot --device DEVICE --destination /private/tmp/cara-frame.png
```

Use a known screenshot to identify FMV, rendered cutscenes, dialogue waits and
controllable gameplay. Keep captures private.

After USB-C connection, Instruments discovery listed the iPad online, but both
Time Profiler and CPU Profiler recordings timed out waiting for the device to
boot. No native CPU sample trace was collected. CoreDevice process inspection,
file transfer and screenshots continued to work. Do not describe the built-in
phase timings as a sampled call graph. Recheck the transport before future native
CPU/GPU profiling and target the exact Cara PID rather than every `cara-ios`
process, since comparison apps can share the executable name.

A candidate GPU implementation is to copy stencil into a separate scratch
attachment, reconstruct its eight bits through stencil-tested color passes, and
sample/encode the depth component in a fragment shader. Validate exact bytes,
channel order, all stencil values, depth boundaries, subrectangles, scaling and
GL state restoration before enabling it. This is a proposal, not a shipped fix.

## Upstream hardware work to evaluate

| Work | Status and relevance |
|---|---|
| [Azahar #2095](https://github.com/azahar-emu/azahar/pull/2095), commit `5ddbaeae237954d1f9c1843e6cf75d8b160c2601` | Merged upstream, not ported. Fixes GPU interrupt queue handling and models completion delays from hardware measurements. Upstream specifically reports improved Dark Moon behavior without the old render-thread delay workaround. Timing estimates are experimental; compatibility improvement is not proof of an M4 FPS gain. |
| [Azahar #2318](https://github.com/azahar-emu/azahar/pull/2318), commit `c688076ac96491bd0d390fc8e6d8325fd3fb420f` | Merged upstream, not ported. LUTs, batching and SIMD for command processing; upstream reports 0.5–1 ms per frame in its workload. Measure Cara independently. |
| [Azahar #1587](https://github.com/azahar-emu/azahar/pull/1587), commit `47cd41cc58a4671147ad5f7727fc7ccb0b194826` | Adapted for Cara's RAM ownership and state lifecycle. Upstream reported small gains; Cara's vertex-heavy scene benefited more. |
| [Azahar #2376](https://github.com/azahar-emu/azahar/pull/2376) | FastInterp is already bundled. The earlier upstream comparison found no FastInterp changes beyond Cara's pinned 2127.0-rc2 source. Recheck before a future update. |
| [Azahar #2316](https://github.com/azahar-emu/azahar/pull/2316) | Closed, unmerged early-depth proposal mentioning Dark Moon. Do not treat its claimed benefit as an accepted fix or remove fragment depth writes without validating PICA depth behavior. |

Cara currently signals 3D/transfer completion synchronously in its older GSP/GPU
implementation. Porting #2095 requires a guest-time event model, queue capacity /
dropped-vblank behavior, operation timing, and serialization of pending events.
Use scheduled guest ticks rather than host sleeps. Keep an immediate-versus-timed
comparison option while testing; cover all four CPU timers and reduced guest
clocks. Preserve native peripheral timing when adapting the CPU budget.

### Prepared index-stream reuse

`CARA_VERTEX_INDEX_CACHE=1` caches assembled triangle IDs, their relative R16UI
stream, bounds and unique count. Keys include physical index address, source
count, index width, topology and nonindexed vertex offset. Every hit compares the
full current source index bytes. Vertex attributes remain live, and transformed
shader output is never cached. Changed bytes invalidate immediately; hash lookup
is only for metadata and collisions still compare complete keys and bytes.

An LRU limits retained vector capacities to 32 MiB and 2,048 entries, with bounded
metadata overhead. Cache rejection preserves the uncached input vector. Native
checks cover changed bytes at the same address, changed metadata, shorter input,
all 65,536 u16 values, unrepresentable ranges, capacity accounting and eviction.
The renderer validates the source range and uses unaligned-safe u16 reads.

In the new-engine opening checkpoint (`0b3ecec92f2f6b8b083811fff67f56d5b88b8d3d5be14e1fb890304fbb8184a3`),
100% guest CPU and sound on, the 1,200-refresh comparison excluding the first 300
matched draw/vertex counts, transfer bytes and final full RGB pixels:

| Prepared index cache | Elapsed | Input preparation | Emulation-thread CPU |
|---|---:|---:|---:|
| Off, resident uploads retained | 26.505 ms | 6.748 ms | 22.939 ms |
| On | 19.494 ms | 0.709 ms | 16.928 ms |

The measured 36.0% throughput increase uses about 2.57 MiB of retained index data
in this scene. The warm window has 100% index-cache hits. Clock, raster, 22,000
ARM/Thumb differential cases, and old/New3DS save-state checks passed.

A new compatible Secret Path checkpoint was recreated automatically through the
opening prompts. The user-captured old-engine Slot 1 file remains backed up;
compatibility checks were not bypassed. Walking uses the same 300..600 forward,
600..900 backward, 900..1200 forward schedule, 100% guest clock and audible output:

| Prepared index cache, walking through grass | Elapsed | Input preparation | Emulation-thread CPU |
|---|---:|---:|---:|
| Off, resident uploads retained | 33.634 ms | 6.777 ms | 29.119 ms |
| On | 26.877 ms | 0.779 ms | 23.236 ms |

This is 25.1% higher throughput. Both paths match draw/vertex counts, uploaded
bytes and final full RGB pixels. The new checkpoint differs from the earlier
user-captured checkpoint, so compare this on/off pair rather than treating the
older grass timings as its baseline. Cache hit rate is about 99.94%, with about
3 MiB retained. Both audio reports are unmuted with no reported output error;
thermal state remained nominal. This is still below native refresh speed.

The iOS frontend enables the cache with packed inputs on devices reporting at
least 6 GiB physical RAM; an explicit `CARA_VERTEX_INDEX_CACHE=0` selects the
uncached path. The core remains opt-in. Retain the bounded fallback and verify
future dynamic-index workloads; do not extrapolate these hit rates to every game.

## Architectural prior art

Prioritize submission overhead and repeated vertex work, alongside a CPU polling
investigation. These are candidates with different correctness and measurement
requirements, not transferable performance multipliers:

- [PPSSPP graphics settings](https://www.ppsspp.org/docs/settings/graphics/)
  describes command buffering for CPU/GPU overlap and CPU skinning that can make
  draw combining easier. Combine only draws with equivalent shader, uniforms,
  textures, target, viewport, depth/stencil/blend behavior and primitive order;
  guest reads/writes and surface ownership transitions must flush pending work.
  Measure total frame time and latency, not merely fewer API calls.
- The same PPSSPP page records removal of its old vertex cache in 1.17: optimized
  decoding made its benefit small or negative, alongside graphics bugs. Cara's
  upload reuse is separately measured and compares exact bytes. The new index
  experiment compares source index bytes and metadata, retains no transformed
  outputs, and must include comparison cost and moving-scene misses in results.
- [On-the-fly Vertex Reuse](https://arxiv.org/abs/1805.08893) investigates local
  batching and reuse in parallel GPU geometry pipelines. Cara already avoids
  duplicate input uploads, but its expanded triangle shader still executes the
  PICA program repeatedly, including quaternion-reference vertices. Prototype
  reuse within a draw with all program/uniform inputs fixed; preserve PICA
  rounding, clipping, quaternion orientation and output packing. Reported paper
  gains are not Cara forecasts. Extra passes, storage and submissions may erase
  the benefit on a CPU-bound GLES path. The tested iPad exposes four GLES draw
  buffers/color attachments, so a single 22-float output pass cannot simply use
  six RGBA attachments.
- [Dolphin's idle-skipping history](https://dolphin-emu.org/blog/2019/05/07/dolphin-progress-report-february-march-april-2019/)
  motivates detecting guest polling loops, and also records false-positive bugs
  in older detection. Cara already idles a core when its kernel has no runnable
  thread. Repeated PCs in FastInterp alone do not establish an idle loop. First
  measure block/tick concentration and classify loads, stores, flags, MMIO side
  effects, interrupt masking, other cores and the earliest wake event. Any skip
  must preserve those effects and scheduler deadlines; do not add blanket busy-
  loop suppression or infer an ARM pattern from a PowerPC implementation.
- Dolphin's actual GPU texture decoder is in
  [TextureCacheBase::DecodeTextureOnGPU](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureCacheBase.cpp),
  using uploaded tiled data/palettes and compute, with a CPU fallback. Its
  VertexManager supplies upload buffers. Cara needs format-specific exact-byte
  tests and transfer/dispatch accounting before extending this beyond vertices.
- [Dolphin ubershaders](https://dolphin-emu.org/blog/2017/07/30/ubershaders/)
  address specialized-shader compilation stutter. That is useful for first-use
  stalls, but the measured warm grass replay has negligible compilation cost;
  it does not explain this sustained slowdown.

## Next experiments

1. Profile Stories in advancing 3D scenes and controllable gameplay; distinguish CPU vertex execution,
   attribute staging, format conversion, surface downloads, shader first use and
   ordinary command submission. Do not extrapolate Dark Moon's profile.
2. Test a GLES3 D24S8-to-RGBA8 GPU path with an original ROM-free byte oracle,
   then compare the same gameplay scene with it disabled and enabled.
3. Evaluate the upstream completion-timing/interrupt-queue model for Dark Moon,
   separately from conversion and clock policy changes.
4. Evaluate command-list batching using original command fixtures before a game
   comparison.
5. Prototype GPU decoding of original vertex bytes and indices to reduce the
   remaining CPU float expansion and upload volume. Retain a bounded, correct
   fallback for unsupported layouts; test signed formats, default attributes,
   duplicate register mappings, index limits and quaternion alignment.
6. Trace why individual surfaces cross to RAM. Avoid unnecessary transfers only
   when ownership/coherence proves the guest does not need their bytes. Never
   substitute stale data, drop depth/stencil contents or hide conversion failures.

Open questions: exact Stories conversion cost; whether completion timing changes
its workload; total GPU execution time versus CPU-driver stalls; M4 thermal and
memory behavior during sustained gameplay; and which costs remain at a fixed
100% guest CPU clock. Record negative experiments too.

## iPad persistent shader-cache capability (2026-11-12)

The iOS frontend still sets `use_shader_cache = false`. The upstream cache
contains program binaries, generated GLSL and shader-reference tables; simply
enabling it would use the old per-title format without Cara engine/driver
identity and save only during renderer destruction.

A physical M4 iPad GLES3 probe returned `GL_NUM_PROGRAM_BINARY_FORMATS = 0`.
The GPU fixture suite passed in that same run. This device/context cannot
export/reload program binaries through GLES; Android binaries cannot solve that.
The probe was a temporary frontend diagnostic on engine commit `742b539`,
with unchanged engine fingerprint and the normal final build restored afterward.
Reproduce on a current EAGL context with:

```cpp
GLint formats = 0;
glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
```

A future disk cache can retain Cara-generated source plus valid program-pair
references and prewarm them before gameplay. It must key by engine/layout and
renderer identity, bound and validate reads, write atomically, and persist before
iOS suspension rather than depending on a clean shutdown. This moves compile
work to loading; it does not eliminate compilation or explain the measured warm
grass slowdown. No persistent shader cache has been enabled in this change.


## GPU surface aliases and submission experiments (2026-11-12)

Dark Moon's opening repeatedly views the same RGBA8 bytes with different tiling
and stride. The source is a native-resolution 256×416 tiled surface at
`0x18000000`; the requested linear view is 128×384 at `0x18004000`. Normal cache
copy matching rejects the layout mismatch, and iOS has no applicable format
reinterpreter. The fallback downloaded 196,608 bytes twice per refresh before
uploading them again. A separate diagnostic `glFinish` probe attributed most of
that readback time to waiting for GPU completion, rather than copying the bytes.
That probe changes synchronization and is not a performance-comparison run.

The new GLES remapper computes the source texel from each destination texel's
physical guest byte address, including Morton order and the GL vertical flip.
It keeps eligible RGBA8 aliases on the GPU. Eligibility requires native scale,
dense rows, distinct textures, complete source containment and valid source bytes;
unsupported layouts and formats use the original fallback. Dirty ownership stays
with the authoritative source, and ordinary CPU/GPU writes invalidate aliases.
It neither substitutes stale pixels nor skips guest-visible memory writes.

Matched M4 iPad replay, 1,200 refreshes with the first 300 excluded, audible output,
FastInterp and 100% guest clock throughout:

| Dark Moon opening | Mean refresh time | Emulation-thread CPU | Downloads per 60 refreshes |
|---|---:|---:|---:|
| Existing path | 27.096 ms | 14.040 ms | 120 |
| GPU alias remap | 14.531 ms | 12.664 ms | 0 |
| Remap plus unchanged-uniform reuse and quaternion-only reference shader | 14.225 ms | 12.625 ms | 0 |
| Existing path, repeated after candidates | 27.430 ms | 14.195 ms | 120 |

The isolated remap improves throughput by 86.5% in this scene. Draw/vertex counts
and final raw 400×480 RGBA bytes match exactly. Raw framebuffer capture happens
after the measured run, before renderer destruction, avoiding UIKit/display color
conversion. This is a refresh-time result, not a claim that every game or scene
runs at 60 rendered frames per second. The remap run still exceeded the native
refresh budget in 72 of 900 measured refreshes; the uniform/reference variant did
so in 34 of 900. Thermal state remained nominal.

The on-device coherence suite covers all four tiled/linear layout pairs, the
observed nonzero byte offset, partial rectangles and unchanged neighbors, CPU
writes rejecting stale sources, and GPU writes invalidating a previously valid
alias. Generated original bytes are the reference; CPU RAM and staging buffers
are poisoned so an accidental old-data/readback path cannot pass. The same suite
round-trips every 16-bit value in RGB565, RGB5A1 and RGBA4 through GPU storage in
both layouts. Portable GLES color readback now uses RGBA/unsigned-byte plus exact
repacking for formats whose requested read pair is not guaranteed.

A lit transformed-vertex experiment was correct but slower: 27.041→33.025 ms
with identical workload and pixels. It stays disabled. GLES3 requires an extra
transform-feedback-to-texture transfer for this prototype's per-triangle output
lookup; a future Metal buffer path can test reuse without that transfer. Reducing
state calls, implicit buffer flushes and RAM bursts did not establish a reliable
scene-level win in the first sweep. They remain diagnostic options rather than
assumed improvements.


The optional FastInterp SIMD handlers combine independent scalar VMUL/VMLA work
inside precompiled ARM64 code. They do not emit executable instructions at runtime.
Fusion runs after decoding and retains every original slot, block boundary and
chain limit. An earlier prototype shortened decoded blocks and changed scheduler
return points despite matching instruction results; a mixed-budget trace test now
requires identical PC, registers, ticks, block ranges and execution counts.
Native tests cover floating-point rounding/flush/default-NaN modes, exceptional
values, operand dependencies, code invalidation, page crossings and tracked-memory
fallbacks. SIMD modes remain opt-in pending matched iPad game results.

The resident-block census found ordinary scans, copies and matrix/quaternion work,
not a proven interrupt-polling loop. Cara already advances idle cores to the next
scheduled event when no guest thread is runnable. No speculative idle-loop skipping
has been introduced. Resident execution counts are not statistical CPU time samples;
`tools/profile_cpu_stacks.py` provides a separate Instruments attach/export path.

## Uniform reuse and interpreter code layout (2026-11-12)

A new matched Stories grass replay used the shipping 80 MiB upload banks,
prepared indices, resident input reuse, native scale, audible output and 100%
guest clock. Each run used 1,200 refreshes with the first 300 excluded.

| Configuration | Mean refresh time |
|---|---:|
| Baseline, first / middle / last | 27.361 / 26.357 / 27.421 ms |
| Uniform reuse | 25.102 ms |
| Uniform reuse, independent repeats | 25.258 / 25.327 ms |
| Repeated baseline | 27.472 ms |
| State reuse alone | 27.142 ms |
| Quaternion reference slice alone | 27.462 ms |
| Packed fetch reuse alone | 27.155 ms |
| Combined graphics options, first / repeat | 24.672 / 24.849 ms |

Every completed configuration matched final raw framebuffer bytes and draw/vertex
counts. Uniform reuse is the clear isolated gain, approximately 8–9% higher
throughput against the repeated baseline. The small additional combined gain does
not justify enabling all experimental shader paths. Only uniform reuse becomes a
new iOS default; explicit `CARA_VS_UNIFORM_REUSE=0` retains the comparison path.

The cache compares meaningful boolean/integer/float fields exactly, including
floating-point bit patterns, and is invalidated when stream storage wraps. A
physical-iPad oracle now exercises production uploads and indexed GPU buffer
readback: unchanged values, individual mutations, signed zero, distinct NaNs,
fragment/light independence, accelerated wrap, and nonaccelerated wrap followed by
accelerated recovery. Run it before guest execution with
`CARA_UNIFORM_SELF_TEST=1`; `uniform-reuse-self-test.json` records the result.
The oracle restores guest inputs and forces subsequent blocks dirty.

Uniform-only `glBufferSubData` staging passed byte/range tests but caused severe
synchronous upload stalls in a diagnostic scene capture. It was rejected and
removed from the selected engine. Existing unsynchronized mapping remains.

An independent compiler experiment isolated ThinLTO removal from LLVM dispatch
tail merging. The interpreter object was confirmed native when testing the
machine-code flag. Normal ThinLTO had 159 indirect dispatch sites in a 42,084-byte
function; no-LTO had 158 sites / 41,896 bytes; no-tail-merge had 540 sites / 56,220
bytes. This changed code layout without emitting runtime code. Device controls:

| Compiler configuration | Baseline / uniform reuse |
|---|---:|
| No-LTO control | 26.552 / 25.110 ms |
| No-LTO plus no-tail-merge | 27.091 / 25.272 ms |

Outputs matched, and native instruction/timing tests passed, but no gain justified
promotion. Both compiler controls default off; normal ThinLTO remains selected.
Compiler-only comparisons retain an ABI-compatible checkpoint and preserve each
executable's SHA/UUID separately, since a source fingerprint alone does not
identify compiler switches.

Phase diagnostics now sample thread CPU and wall time for draw submission,
texture orphan/subimage upload, stream map/unmap and complete uniform upload.
`tools/summarize_phase_cpu.py` reports raw totals, sample counts and clock failures;
ratios use sums across complete windows. Off-CPU time includes blocking and host
scheduling and must not be labelled GPU execution time. These sampled diagnostic
runs are separate from uninstrumented speed comparisons.
