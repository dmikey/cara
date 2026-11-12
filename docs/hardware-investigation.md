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

The iOS GLES path expands indexed triangles/strips/fans on the CPU and uploads
packed float4 attributes for every expanded triangle vertex. A small within-draw
CPU decode cache avoids some repeated attribute reads, but repeated indices still
produce repeated uploaded records. The final draw is `glDrawArrays`, so this path
also does not use an indexed draw's native post-transform vertex reuse.

The fused vertex shader reruns the first vertex of each triangle when needed to
align quaternion signs before interpolation. That preserves an aspect of PICA's
fixed geometry stage on GLES3, but makes changing back to a simple indexed
vertex-array draw insufficient by itself. Preserve the triangle's expanded
ordinal and first-vertex quaternion behavior when optimizing.

A bounded next design is an expanded index texture plus a unique-attribute
texture. The shader can look up an index before fetching inputs while retaining
its current triangle/quaternion semantics. This reduces duplicate transfer data;
it does not automatically cache transformed vertex outputs. The reuse probe is
intended to quantify whether that design is worthwhile before implementing it.
Raw-byte GPU decoding is a separate, later step with additional format tests.

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
