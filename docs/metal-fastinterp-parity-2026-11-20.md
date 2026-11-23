# Metal/FastInterp parity investigation, November 20

## Normal-play history correction, November 21

The95–96% runs labeled normal boot below saved a checkpoint at3000 before
measuring 3120..3600. That is **not uninterrupted normal play**: `System::serialize`
calls `RasterizerClearAll`, which flushes GPU ownership and clears renderer page
watches. Restored98–99% results likewise begin with rebuilt renderer history.
Retain their exact-work comparisons, but withdraw ordinary-play speed claims.

`gadd-v27-normal-no-save` omits that save, uses the same original input/filesystem,
100% guest clock and the then-current reference options, and reproduces original endpoint
pixels and CPU/ticks exactly. It measures23.952 ms/ref (about69.58%) and19.724
thread CPU ms/ref. Background `mediaanalysisd` CPU activity was also observed;
one run does not isolate the history effect's magnitude. A separate instrumented
no-save run nevertheless exposes4,345,921 pending-publication calls during the
480-refresh interval, versus35,041 in a restored diagnostic. Both execute exactly
319680 draws,213700740 raw corners and6720 draw groups. There are no guest flush
calls in either interval. The normal sample contains CPU memory-write → renderer
invalidation stacks.

Source review confirms partial CPU invalidation removes GPU ownership but leaves
page watches until global reset. FastInterp subsequently routes writes on those
pages through coherency callbacks. The bounded watch-release implementation now retains every whole-page,
adjacent-page, resident, pending and deferred-copy dependency. Timing comparisons
must preserve this history. The profiler saves optional end-of-run checkpoints
only after timing/boundary snapshots. The later uninterrupted v29 result below
measures16.713252 ms/ref (99.72%) without an in-run save; it supersedes the v27
throughput figure without rehabilitating the earlier save-at3000 claims.

## Scene verification correction

The `lab-v2-*` and `lab-v3-*` campaigns, and their rebuilt normal-boot seeds
`saved-lab-layout-build` / `saved-lab-shared-candidates`, were misidentified.
Visual inspection shows the **title screen**, because the original menu input
script was omitted while recreating checkpoints. All those folders now carry
`SCENE-CORRECTION.json`. Their internal pixel/CPU comparisons and GPU fixture
proofs remain data, but their timings do **not** measure Gadd's lab. Withdraw
any lab-speed interpretation of those two campaigns. The original
`saved-lab-normal` checkpoint and `real-lab-warm-A1` / clean A3–B2–B3–A4 runs
are the verified lab reference. Identical sampled CPU idle registers/ticks did
not establish scene identity; visual endpoint verification and retained menu
inputs are required when recreating a checkpoint across builds.


Target: Luigi's Mansion: Dark Moon USA (`0004000000055F00`) on the local
Apple M3 Pro, native resolution, FastInterp on every active core, 100% guest
clock and speed cap. This is an ongoing investigation, not full-speed acceptance.
ROMs, screenshots, states and generated captures stay in ignored private work
directories. Mac results do not establish iPad sustained performance.

The current developer runner retains the existing iPad GLES compatibility
policy: PICA shadow rendering is disabled. The native renderer counts declined
unsupported shadow requests (42,942 in the 1,800-refresh `bridge-A1` run) and
throws if shadows are explicitly enabled. The comparisons below preserve that
same policy; they are not evidence of complete PICA shadow support or all-effects
graphics accuracy. No new draw/effect omission is introduced by these experiments.
SDL/CoreAudio output opened at 32,728 Hz stereo; this alone does not establish
underrun-free or subjectively smooth sound.

## Lessons retained from the GLES work

The review covered the performance, hardware-investigation, FastInterp, surface
coherence, vertex transport, shader cache, threading and measurement documents.
The highest-value mechanisms are exact ownership and reuse, not fewer guest
commands or relaxed emulated timing.

| Prior evidence | Consequence for native Metal |
| --- | --- |
| [Luigi GPU alias conversion](gpu-surface-aliases.md): 24.466 / 16.713 / 24.543 ms OFF/ON/OFF, six downloads to zero | Keep GPU ownership and queued alias conversion; measure materializations and fallbacks before adding another cache |
| [Depth coherence](gles-depth-coherence.md): corrected fallback 37.548 to 17.785 ms; 5,960 readbacks to zero | A missing readback is only correct when the bytes remain owned and can be published; preserve untouched CPU bytes and source-range validity |
| [Pipeline prototype](ios-pipeline-cache-prototype.md): full shader hashing per draw regressed Luigi to 69.6 ms versus 17.0 cache-off | Retain exact source admission, then use validated source revisions and complete pipeline recipes on repeated draws |
| [Uniform reuse](hardware-investigation.md): isolated Stories controls 27.135/27.420 versus 25.102/25.258/25.327 ms | Compare complete uniform bytes, including signed zero/NaN bits, and respect storage lease boundaries |
| [Moving vertex order](vertex-upload-reuse.md): 19.055 to 18.085 ms, 91% fewer transported bytes | Measure actual changed payload transport; quiet windows can pay more lookup overhead |
| [M1 integration](m1-performance-integration-2026-11-17.md): larger queue cut batches 72% without a speed gain | Queue/group count alone is not a performance result; require paired throughput and wait evidence |

Used-input pruning, PBO staging, lit transformed-output reuse, broad SIMD lookup,
relative dispatch and increased QoS did not establish useful repeated gains in
their documented configurations. Do not repeat them without a newly measured
discriminator. Historical private raw captures under `/Volumes/Fast` are absent
here; tracked Markdown and the three `docs/evidence` receipts are available.

## Baseline repairs and attribution

The Mac Metal developer runner previously omitted the production interpreter
defaults already selected by the GLES frontends. `apple_fastinterp_policy.h`
now shares ALU operands, tick buckets, chain budget, eight-way cache, RAM bursts
and AL dispatch across the Apple frontends. The existing 32K decoded-cache policy
is selected only with at least 6 GiB physical memory; explicit overrides survive.
This is host interpreter policy, not a clock or instruction-timing change.
The actual FastInterp differential regression passed 22,000 ARM/Thumb cases with
these options, plus the existing fused-loop, memory and context checks.

Public Apple QoS policy is shared separately. Active user emulation requests
user-initiated QoS while preserving an already interactive thread; Mac emulation
holds a scoped user activity to avoid App Nap. The measured Mac thread was
already interactive (33), so this supplies no evidence of a scheduling speedup.
No CPU affinity, undocumented scheduler controls or forced performance cores
are used.

On revision `cce1a67`, detailed title-window diagnostics showed 1,103,683 draw
bindings. Enabling the existing exact pipeline-selection memo reduced aggregate
`pipeline_and_catalog` time from 13.504 to 1.904 seconds and `bind_total` from
23.557 to 12.124 seconds. Endpoint captures matched. These are instrumented
diagnostic totals, not exclusive CPU percentages or a clean gameplay speedup.
Arena waits then grew to 5.784 seconds; 29,731 of 41,935 draw groups reached the
32-draw capacity. Those observations justify separate eight-page and 64-draw
experiments, despite their earlier noisy/rejected results in other workloads.

After sharing the interpreter policy, the clean 1,800-refresh title-window run
`shared-apple-policy-C1` measured **55.210%** weighted speed after refresh 900,
with a 53.338% lowest two-second window. Every requested refresh was presented
and the final capture succeeded. This is a title screen, not active gameplay,
and is substantially short of the target.

## Remaining bridge work

The first frozen-binary discriminator campaign completed with identical endpoint
pixels and retained CPU boundary in all eight runs. Weighted speed after refresh
900 (1,800 total, no scripted inputs):

| Configuration | Speed |
| --- | ---: |
| Selection memo control A1 | 57.991% |
| Eight upload pages | 61.741% |
| 64-draw groups | 64.198% |
| Exact uniform uploads | 59.176% |
| Selection memo control A2 | 59.637% |
| Unique vertex compute | 57.490% |
| Eight pages + 64-draw groups | 63.750% |
| Unique compute + eight pages + 64-draw groups | 61.118% |

These are discriminators, not a repeated promotion matrix. The control drift
does not support a uniform-reuse win. Compute vertex reuse did not improve this
title window, despite less repeated vertex arithmetic in eligible draws; it
stays off. Neither combining switches nor increasing capacity guarantees a gain.
The next binary adds exact warmup-to-end wall timing including final GPU drain,
per-core tick boundaries, source candidate probes and immutable-cache hash,
compare, transport-byte and eviction counters. Diagnostic runs retain their
observer overhead separately.

The actual raw-index capture CPU fixture passed 2,669 cases in each of four fresh
memo/specialization configurations, with identical independent triangle-oracle
digests and 4,176 guest reads in every mode. It covers 8/16-bit and sequential
indices, topology, rebasing, interior mutations at the same address, live changed
attributes/defaults, sparse admission and failed reads. Memoization still reads
and validates complete live index bytes; it does not freeze vertex attributes.
Use `tools/test_metal_raw_index_capture.py` to reproduce it against a built engine.

Apple's [command-buffer guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/CommandBuffers.html)
recommends few submissions while keeping the GPU occupied, and its
[Mac rendering guidance](https://developer.apple.com/videos/play/wwdc2023/10125/)
explains tile load/store costs and indirect command buffers for CPU encoding
limits. These motivate measurements, not a throughput promise. The portable
backend keeps its macOS 13/iOS 16 API floor; Metal 4 is a possible future
capability-selected path, not a requirement silently imposed on older devices.

1. Per-draw vertex source lookup still validates complete program and swizzle
   arrays while scanning a bounded cache. A recent candidate can avoid preceding
   probes but must keep every exact identity comparison, including after loads.
2. Uniform capture builds all VS/common/light blocks each draw. GPU slice reuse
   can reduce copies and arena pressure but does not remove host construction.
3. Ownership publication can allocate per-record writers and repeatedly poll
   predecessors. Only all-identical draw publications currently collapse.
   Adjacent/overlapping interval union is a possible later mechanism; never fill
   holes, drop failed-producer checks or combine unrelated owners/generations.
4. Immutable vertex texture hits still require live capture, full hashing and
   full byte equality. Index equality and guest page dirtiness are insufficient
   proof that vertex attributes stayed unchanged.

Uniform ranges currently belong to one arena command/page lease. Keeping a
Metal buffer alive does not keep its bytes immutable after that lease ends.
Cross-command slice reuse requires reader completion tracking or independently
owned immutable storage. The shared uniform-reuse opt-in remains scoped to the
current grouped pass and disables deferred vertex batching; measure that tradeoff.
The production encoder GPU test passed 35 golden pixels with exact upload
counters across uniform changes, vertex modes, pass boundaries and recycled
leases, with byte-identical enabled/disabled output.

## The actual slow-scene miss: non-VRAM texture snapshot churn

The scripted normal-boot replay reached the opening sequence and the mansion
exterior by refresh 7,200. The late scene fell to 26–29% speed in the first
instrumented pass. The tracked VRAM cache was working: between refreshes 6,000
and 7,200 it incurred only three new misses. In contrast, non-VRAM decodes grew
from 2,034 to 267,711. Sample/mip resolution and submission pressure dominated
that interval. This was a materially different bottleneck from the title screen.

The existing exact keyed and larger-RAM snapshot paths were not selected. With
both enabled on the same frozen binary, total non-VRAM decodes fell to **418**,
mip-chain builds fell **82,088 to 240**, and completed GPU commands fell
**866,409 to 436,588**. All **3,946,372 draw bindings/published draws** still ran.
Pixel captures at 1,200, 2,400, 3,600, 4,800, 6,000 and 7,200 matched exactly,
as did the final CPU/VFP/per-core-tick boundary. Keyed cache counters recorded
6,462,997 hits, 414 initial key misses, no changed-byte hits and no evictions;
the larger cache recorded 24,114 hits and four misses.

The late scene improved to 60–80% speed in that diagnostic pass. Its observers
and warmup differ from the first pass, including a separate ten-second Instruments
recording in the first run, so these timings are not a clean causal speed claim.
A quiet A/B/B/A replay is the next acceptance step. The counter differences and
exact retained outputs establish the repeated decode mechanism independently.

### Quiet confirmation and shared policy

The same-binary A/B/B/A normal-boot replay then measured exactly refreshes
6,800–7,200, with identical scripted inputs, fixed RTC, FastInterp/guest clock
100%, native resolution and open SDL/CoreAudio output. Renderer cost counters,
sampling, Instruments and intermediate captures were off. Final GPU capture/drain
is included. Both arms used the same other explicit optimizations, including
64-draw groups, index memo/scanning, recent-source lookup and shader policy cache.

| Arm | Snapshot caches | Wall ms/refresh | Speed | p99 boundary interval |
| --- | --- | ---: | ---: | ---: |
| A1 | Off | 54.662 | 30.47% | 59.300 ms |
| B1 | Keyed + large | 20.324 | 81.46% | 26.610 ms |
| B2 | Keyed + large | 20.557 | 81.56% | 26.282 ms |
| A2 | Off | 57.845 | 28.83% | 71.919 ms |

Mean completed-work time fell 56.254 to 20.440 ms/refresh, about 63.7% lower.
Control drift is present, but both candidate timings are far below both controls.
Every arm produced the same endpoint pixels and retained CPU/VFP/per-core ticks.
These are finite matched observations, not statistical confidence intervals,
whole-memory equivalence, or proof of full-speed gameplay.

The shared Apple Metal policy now enables exact keyed snapshots. Larger snapshots
are enabled at 6 GiB physical memory or above, retaining the original fallback
on lower-memory devices and every explicit override. Both CPU and Metal helpers
use one cached query in `common/apple_hardware.h`; no chip-name fork was added.
The common-policy tests passed 3/4/6/8 GiB cases and explicit off/empty/custom
values. This is a locally tested promotion; physical M1/M4/A-series gameplay and
sustained thermal acceptance remain separate validation.

### Current parity target: Dr. Gadd, not a substitute scene

The user reports the **same M4 iPad** runs the Dr. Gadd conversation near 100%
with GLES and around 50% with Metal. That is a backend gap, not a cross-device
comparison. The mansion cache fix above must not be presented as resolving it.
The next frozen build creates a normal guarded checkpoint during the conversation
and snapshots its task-owned NAND/SDMC/sysdata while the guest is stopped between
RunLoop calls. Restored experiments use identical state/file/input hashes and
normal engine/title/CPU/system-mode guards. No state fingerprint is bypassed.

Local source differences to discriminate there:

- Shared GLES shader-generator options for packed fetch reuse and quaternion-only
  reference evaluation are not yet selected by Metal's common policy. They can
  apply to its existing packed-index path without changing guest work.
- Unique-vertex compute needs retesting in this heavier shader scene. Its title
  result was negative; it now respects the same 32/64 draw-group capacity.
- GLES avoids redundant uniform transport and raster/resource state binding;
  Metal's ordinary path still copies every block and reissues encoder bindings.
- Completed immutable fill reuse and two-copy batching have passed actual
  production-lane GPU fixtures but remain explicit pending scene measurements.

The old absolute private M4 report path is absent on this Mac, and its claimed
native-Metal PC230 timing receipt was not found in reachable Git history. The
four remote dispatch experiment branches concern DynCom, not omitted FastInterp
production gains. Do not treat them as evidence of an available faster M4 build.

The actual production `DecodeTexture` GPU fixture passed eight cases in each of
four fresh default/keyed/large/combined processes. It covers 129-entry FIFO churn,
512-key LRU, key identity, mutations in the last used byte and unused source tail,
1 MiB textures, queue changes, and native GPU owners including owners covering
only source padding. GPU-owned inputs bypass RAM snapshot reuse. Both 64 MiB
budgets count raw snapshot plus logical decoded pixels, allocate on demand and
do not bound driver memory, metadata, alignment or in-flight consumers.

Do not replace the full FCRAM comparisons with page markers. The audit found raw
Apple AAC and DSP writes, persistent shared-memory pointers and noncanonical heap
aliases that bypass the canonical rasterizer page barrier; GSP cache-maintenance
services are stubbed. GLES shares some of these marker limitations. A shared
physical-write/alias protocol is required before extending tracked residency to
all FCRAM. Existing normal save-state loading already invalidates all tracked
VRAM residency before restoring RAM; no extra epoch reset was necessary.

The native runner now also retains buffered per-refresh wall intervals. They
measure RunLoop/VBlank boundaries, not physical scanout or unique game frames.
Long-tail timing must be reported alongside average speed when assessing shader
compilation, load stalls and smoothness.

## Correct lab replay: pipeline warmth and work placement

The ordinary saved game now reaches the requested lab conversation (final image:
Gadd and Luigi, “Where to, Luigi?”). `saved-lab-normal` saves a guarded engine
checkpoint at refresh 3000, along with paused filesystem contents. Restoring it
for 600 refreshes reproduces normal refresh 3600 CPU registers/VFP and global
and per-core ticks exactly, excluding the relative host refresh counter. The
normal versus restored endpoint differs in 48 pixels by at most one channel
unit; its cause remains unresolved. Restored-to-restored experiments require
exact endpoint pixels and the retained CPU boundary. This qualification also
applies to the earlier opening checkpoint (29 pixels, maximum two units).

A real runtime failure prevented reaching this scene before: the 64 MiB source
budget charged the same large vertex strings once per material pair. A bounded
exact-string source pool now owns each distinct string once, keeping stable
views and the original unique-byte/pair limits. The normal lab run reports
11,933,208 logical bytes versus 5,275,154 uniquely owned bytes. Source-pool CPU
and actual generated-shader GPU tests pass. The exception path now explicitly
ends open encoders, abandons uncommitted work, rejects guest writes and drains
committed work; actual deferred-packet and pending-copy fault tests pass with
Metal API validation enabled.

Shared pipeline learning now has a bounded explicit full-prewarm mode. Cache
identity includes the actual device and shader-generator signature. In the
first correct warm lab replay (`real-lab-warm-A1`), all 90 recorded recipes were
ready before execution, zero runtime pipeline misses occurred, and refreshes
120–600 averaged 19.641 ms (steady speed windows 84.95%, p99 24.560 ms,
maximum 25.119 ms). This is a single local M3 Pro measurement, not a 100% claim
or an iPad result. The machine is identified by hardware as M3 Pro.

Initial follow-up controls drifted toward 21.04 ms; the first unique-compute
and uniform-reuse speed comparisons therefore cannot establish a causal gain
or loss. A clean repeated control/variant sequence is retained separately.
Diagnostic clocks and sampling are not mixed into quiet timing conclusions.

Actual lab diagnostic replay (`real-lab-diagnostic-D1`, 600 refreshes) records
399,600 raster draws and 8,456 draw passes. Existing immutable vertex uploads
hit 793,961 times versus 5,239 misses; the path already avoids most repeated
transport. With unique compute enabled (`real-lab-unique-dedup-diagnostic-D2`),
365,963 draws use 49,204 compute dispatches, covering 135,194,286 corners with
48,902,903 unique transforms. Cross-draw deduplication finds **zero** hits in
this scene. The additional dispatch/pass/output traffic must be justified by
end-to-end timings; fewer transforms alone do not establish faster execution.
The original raster draws and endpoint pixels/CPU boundary are preserved.

The repeated clean lab sequence (`real-lab-unique-abba.json`) is:

| Run | Unique compute + cross-draw dedup | ms/refresh | p99 ms |
| --- | --- | ---: | ---: |
| A3 | off | 20.553 | 25.162 |
| B2 | on | 21.096 | 25.068 |
| B3 | on | 21.167 | 25.191 |
| A4 | off | 20.694 | 25.917 |

All four restored endpoints match exactly. Retain these mechanisms as diagnostic
options; do not promote them from synthetic transform savings. Controls average
20.623 ms versus 21.132 ms for this variant, about 2.5% more elapsed time.

The title-screen built-source campaign (`lab-v2-*`, misidentified initially; see
scene correction above) must remain separate from the previous binary/checkpoint. Controls measured 22.521 ms; raster reuse 22.670 ms
and direct-export/striped borrowing 22.983 ms, with exact restored endpoints.
Those single samples establish no speed improvement. Packed-layout learning on that title-screen checkpoint
rendered all 600 refreshes with identical endpoint pixels but failed the full
measurement contract: Apple's binary archive serializer rejected an archive
with “missing vertex stage in pipeline no. 54”, and the runner treated failed
optional cache persistence as fatal before writing the CPU boundary/RESULT.
Do not count this as a successful comparison or infer shader correctness from
the endpoint alone. Shared learning persistence is being made nonfatal; recipe
persistence must not depend on successful optional binary-archive serialization.
The older valid archive must remain atomic and usable on failure.

The transfer probe on the prior binary confirms 1,200 direct queued exports
(368,640,000 bytes) and 4,800 borrowed striped inputs avoiding 734,150,400
staging bytes over 600 refreshes, with exact endpoint comparison. Counters show
removed copies, not a demonstrated frame-time gain; production-lane ownership,
padding, overwrite and fault fixtures are still required.

A structural source difference now has a specific candidate implementation:
`pica_metal_draw.mm` submits expanded corners with `drawPrimitives`; its current
shader performs its own index-texture lookup. Simply substituting an indexed
Metal call would double-index incorrectly. GLES already has a mode-6 shader
and pair-indexed submission: each unique ID maps `(source, triangleFirst)`,
preserving quaternion sign while permitting hardware vertex-cache reuse without
a separate compute pass. Native desktop GL also has a geometry-stage route;
neither proves the user's iPad build enabled a particular private option.
The prior local research already tested GLES lit-pair reuse and rejected it:
[hardware investigation](hardware-investigation.md) records 28.005 ms versus
25.068/25.321 ms controls at 80 MiB per bank; removing orphan fallbacks with
112/128 MiB still measured 26.089/26.032 versus 24.713/25.186 controls. It remains
off there. This is **not** a missing proven GLES optimization. Metal's immutable
index-buffer submission avoids that particular EBO/texture bank transport path,
so a backend-specific experiment is justified, but source-level reuse models
alone do not predict a win. Record memo/build/upload costs and actual elapsed
work; reject another slower path even if its predicted shader count is lower.

A bounded Metal pair-index experiment must preserve all varyings/clip outputs,
original corner order and live uniforms, explicitly key its mode, keep immutable
index-resource lifetimes, and fall back on limits or insufficient predicted reuse.

The inclusive host breakdown is retained in `lab-work-breakdown.json`:
5.10 ms/ref draw-binding preparation, 3.31 ms/ref recording/encoding and
2.88 ms/ref native transfer handling, with 1.21 ms/ref submission-capacity wait.
These nested scopes must not be added. Within preparation, vertex capture is
1.41 ms/ref, sampled texture handling 1.21 ms/ref, depth acquisition 0.85 ms/ref
and source lookup 0.59 ms/ref. The same diagnostic records 666 draws,
445,209 expanded corners and 38 native transfer operations per refresh.
FastInterp ExecuteBlock's 1,375 top-of-stack samples out of 3,687 main-thread
samples identify substantial instruction work, but do not explain a renderer
regression when the interpreter is held fixed. Match backend work and guest
state; do not attribute all inclusive interpreter stacks to instruction dispatch.

Next bounded experiments target literal packed-layout decoding (with explicit
variant admission caps), exact raster-state reuse inside one open encoder, and
a diagnostic vertex-stage SIMD feasibility study. None is a production default
or a proven device-wide gain. Vertex SIMD currently compiles for the selected
macOS/iOS AIR targets and creates a pipeline on the local M3, but cross-draw
scope and inactive-lane correctness require proof before production use.

## Shared candidate build and diagnostic coverage

The next FastInterp-only native build includes independently switchable,
default-off candidates. They live in the shared backend rather than device-only
frontend forks:

| Candidate | Explicit option | Validation before scene timing |
| --- | --- | --- |
| Hardware pair-indexed vertex submission | `CARA_METAL_PAIR_INDEXED_VERTICES=1` | Immutable bounded payload memo and actual generated/raster tests; promotion awaits scene timing |
| Vertex-stage SIMD triangle reference reuse | `CARA_METAL_VERTEX_SIMD_REFERENCE=1` | Shared production transformation: 192 actual-stage cases, 19,008 vertices/instances with all 24 canonical values bitwise exact; Apple7+ conservative gate and original-shader fallback |
| Larger raster groups | `CARA_METAL_DRAW_GROUP_LIMIT=128` or `256` | Actual 32/64/128/256 grouped rendering: exact golden pixels/publication order, retained old storage and abort under Metal validation; deferred compute remains capped at 64 |
| Draw-ordinal immutable vertex candidate | `CARA_METAL_VERTEX_TEXTURE_ORDINAL_LOOKUP=1` | Actual OFF/ON readbacks, frame/cancel/rollback/order/tail/padding/eviction cases; all bytes still compared |
| Direct exported/striped transfer consumption | `CARA_METAL_DIRECT_EXPORT_CONSUMPTION=1`, `CARA_METAL_BORROW_STRIPED_SOURCE=1` | 44 production GPU cases across four flag combinations; exact ownership, padding, mutation and fault contracts |
| Open-encoder raster state memo | `CARA_METAL_REUSE_RASTER_STATE=1` | 29 production draws per OFF/ON process, exact state transitions/NaN bits/golden pixels; no scene benefit established |

Wider groups allocate extra publication storage only when explicitly selected;
committed storage is not overwritten by the next group. Existing queue/arena
bounds, alias/read barriers and completion rules stay in force. Numeric choices
are limited to 32/64/128/256; the existing 32/64 default selection is retained.
The legacy draw-stage “seventeen_to_32” bucket name actually means 17+ and may
contain these larger groups; do not infer a 32-draw bound from that label.

Learn-mode persistence now logs optional save failures without stopping gameplay.
Checked recipes can publish independently of binary archive serialization, while
a failed archive replacement preserves previous bytes. The real Apple serializer
failure fixture (filesystem error) validates that contract; strict record/replay
failure checks remain intact. The actual earlier driver “missing vertex stage”
error is retained as the motivating run, not falsely claimed reproduced by the
filesystem-error fixture.

Instrumented runs now retain a bounded `metal-command-timeline.csv` and metadata
with tracked command submit/retire refresh and actual GPU start/end times. The
helper merges overlapping intervals rather than adding them as utilization,
filters by submission refresh, and exposes missing/dropped data. The grouped GPU
fixture validates two real command timestamps for every capacity. The 13 profiler
unit checks cover overlap union, warmup/final exclusion and invalid/incomplete
data. Coverage gaps can identify follow-up regions, but are not by themselves
proof of CPU starvation or GPU occupancy.

## CPU/GPU overlap and compiler follow-up

Do not assume the existing GLES reference has an application graphics worker.
The iOS host sets EAGL multithreading false unless explicitly selected; its prior
physical-device experiment did not establish a gain. The Mac GL command queue
also requires an explicit option. Normal GPU/driver overlap is separate from
an app-owned CPU encoding worker.

A possible Metal CPU-worker seam is a sealed group of immutable draw bindings,
retaining `NativePicaDrawBindings.storage`. Guest authority, cache selection and
RAM access must stay on the producer. A worker could own only its draw encoder,
upload arena and command submission. Current tickets reject uncommitted commands,
and queue/authority checks bind to their creating thread; moving `Record` to a
thread wholesale violates those contracts. Producer publication needs a bounded
sequence acknowledgement before every overlapping guest read/overwrite, dependent
GPU read, transfer, present, save/load or abort. Queued ranges must participate
in those checks, or a late worker can resurrect an overwritten guest owner.
This is a design investigation, not an implemented performance gain.

Another explicit follow-up is the GLSL→SPIR-V→MSL compiler path:
`pica_metal_runtime_shader.cpp` sets `disableOptimizer=true`, and dependency
configuration sets glslang `ENABLE_OPT=OFF`. Actual specialized MSL therefore
still contains decoder loops and branches before Apple's compiler. Literal
inputs/forced inlining may let Metal fold them, but compiled folding has not
been demonstrated. Khronos exposes individual inlining, memory and dead-code
passes in its [SPIRV-Tools optimizer API](https://github.com/KhronosGroup/SPIRV-Tools/blob/main/include/spirv-tools/optimizer.hpp).
A minimal conservative pass experiment would need pinned compatible tools,
unchanged numerical/contraction semantics, exact interface/reflection validation
and special-value GPU equivalence before any scene speed claim. Merely flipping
`disableOptimizer` while building with `ENABLE_OPT=OFF` cannot add those passes.
No optimizer dependency or production policy is changed by this investigation.

## Verified rebuilt lab checkpoint

`gadd-v4-verified-seed` corrects the normal-boot input omission by retaining the
original `saved-lab-normal/inputs.txt` (A at 720, 960 and 1200, with releases)
and original filesystem seed. Visual inspection at refresh 3000 confirms Gadd
and Luigi with “Where to, Luigi?”. Normal refresh 3600 pixels are **byte-for-byte
identical** to the original `saved-lab-normal` endpoint, and all 3600 requested
presentations complete. This functional seed contains capture/checkpoint overhead
and is not a clean speed result.

The profiler now accepts `--scene-reference VERIFIED_RUN`, a coarse cross-build
endpoint guard (99.5% of pixels within three channel units, mean absolute channel
difference at most 0.5). It rejects the erroneous title checkpoint (58.95% distant
pixels, mean difference 26.60) and accepts the corrected normal endpoint with zero
difference. This guard establishes approximate scene identity only; same-binary
optimization comparisons still require exact pixel and retained CPU equality.
Thirteen evidence-helper tests pass, including padding/rounding versus wrong-scene
rejection. The new `gadd-v4-*` experiments use the corrected guarded checkpoint,
the same 100% guest clock/FastInterp, learned caches and the verified reference.

## Correct-scene candidate results

`gadd-v4-results.json` retains the rebuilt same-binary comparisons. Every arm
passes the scene guard; all variants match exact endpoint pixels and retained
CPU/ticks. Quiet controls measure 19.557 /19.969 ms per refresh (85.43 /83.45%
steady windows). Duplicate-publication reuse alone is19.767 ms; with128/256
raster groups it is22.710 /25.037 ms. Ordinal vertex lookup is19.981 ms,
pair-indexed selection20.351 ms and SIMD reference reuse19.861 ms. These do
not establish improvements over the controls; keep them off. In particular,
pair indexing admits **zero** meshes in this lab under its modeled25% savings
guard; its measured overhead is not a test of faster indexed GPU execution.
SIMD is actually compiled for34 programs and used for399,600 draws, but its
correctness and fewer evaluator calls do not produce a convincing elapsed gain.

Actual instrumented tracked-command coverage spans about10.3–10.6 seconds,
with merged command intervals covering about5.1–5.3 seconds; one or two command
timestamps are missing and compile/startup effects remain in diagnostic runs.
These are coverage intervals, not exclusive GPU busy percentages or proof of
CPU starvation. They argue against assuming that adding more GPU stages alone
will solve the host/graphics bridge cost.

The existing completed-fill lease option is a more specific CPU-wait candidate.
`CARA_METAL_FILL_LEASE_NO_RESERVE=1` reuses immutable completed bytes without
reserving a new submission slot when it emits no GPU command. The first run is
18.736 ms (89.52%) with exact output/CPU. A diagnostic observes9,594 backing hits,
726 full-queue lease publications and just one fill wait (0.458 ms total), versus
546 waits/281.884 ms in the earlier D1 diagnostic. Full8-slot/gated-producer GPU
fixtures validate that reads still wait for predecessors, bytes/generations stay
correct across overwrites, no extra tracked command appears and terminal faults
reject further work. Controls pass8 cases each; enabled path passes9 under Metal
validation. These are real ordering tests, not intentionally faulted hardware.

Repeated clean ABBA is19.023 /19.109 /18.843 /19.233 ms (OFF/ON/ON/OFF), all
endpoints exact. Mean improvement is only about0.8%, comparable to control drift;
do not promote a default or claim the first89.5% is a stable causal speed gain.
The receipt is `gadd-v4-fill-abba.json`.

A compiler-policy gap is now under explicit test: older iOS interpreter research
used normal ThinLTO, but the native Metal builder selected generic O3/LTOoff.
Simply enabling CMake IPO gives CXX/link ThinLTO while omitting Metal's Objective-C++
bridge. The builder now has an explicit audited option; complete Objective-C++
coverage is now audited for CXX, Objective-C++ and the executable link.
The compiler-only ABBA is19.530 /19.476 /19.790 /19.742 ms (NoLTO/LTO/LTO/NoLTO),
which establishes no clear performance gain. No architecture-specific ISA target
is enabled; JIT remains an explicit subsequent diagnostic build option. The frozen NoLTO app and
state fingerprint are retained in `work/build-frozen-gadd-v4`. The actual current
NoLTO FastInterp target passes22,000 ARM/Thumb differential cases plus transfer,
SIMD, code-boundary, context and address-space checks with the production CPU flags.

## Measurement contract

`tools/profile_native_metal.py` retains binary/source identity, explicit options,
fixed RTC, isolated app data, input identity, effective CPU policy, QoS, thermal
state, complete requested presentations and steady speed windows. New comparisons
also require identical endpoint pixels (excluding row padding) and a guest
register/VFP/global-tick boundary. This is not complete RAM/service/audio state
or every-frame equivalence. State/checkpoint replay must use normal compatibility
guards; never rewrite an old fingerprint to make it load.

Detailed renderer clocks and host stack sampling belong in separately labeled
diagnostic runs. Nested scopes and overlapping command GPU spans must not be
added as exclusive utilization. A FastInterp ancestor in a sampled stack does
not establish that instruction execution dominates. A fresh user directory is
not a cold Apple driver cache. Require repeated clean controls, matching work
and moving gameplay before promoting flags or claiming 100% speed.

Each new run retains a binary-matched build audit when available and untracked
engine source inputs alongside its tracked patch. Build metadata and the current
checkout snapshot are separate evidence; neither proves that the current
checkout produced a previously built executable.


## November21 CPU/backend control and remaining work

The diagnostic build now supports an explicit `--cpu-backend jit`; FastInterp
remains the default. The profiler refuses JIT without a matching JIT-enabled
build audit. Both backends pass the49-case adapter fixture. Checkpoints retain
their original build/backend guards: the JIT-capable engine needed fresh normal
boots with the original A-button script and filesystem, not edited fingerprints.
The new FastInterp boot reproduces the original lab endpoint exactly. JIT's boot
is visually the same Gadd conversation but differs in pixels; it uses its own
verified reference. The cause of the cross-backend pixel difference is unresolved.

The actual guest is **Old3DS** in both controls, with clock100%, core tick hack0
and timer scales1. This is verified from normal configuration and the packed
checkpoint headers (`gadd-guest-model-header-audit.json`). The Luigi title entry
near core.cpp303 belongs to the accurate-multiplication list, not the separate
New3DS auto-selection list. Four CPU objects do not imply the New3DS model.

The legacy macOS OpenGL diagnostic uses the same JIT-capable ThinLTO engine,
OpenGL4.1 atlas rendering and the actual runtime backend guard. In the normal
3000..3600 lab interval, JIT+OpenGL is16.666875 ms/ref (99.99875%) and11.0276
thread CPU ms/ref; FastInterp+OpenGL is26.425923 ms/ref (63.0694%) and16.71955
thread CPU ms/ref. Both record399,600 draws, with84 corners differing out of
roughly267 million. These are Mac desktop GL controls, not the iPad GLES result.
Receipt: `work/apple-cpu-bridge-checks/opengl-cpu-controls-comparison.json`.

The new native runner records measured emulation-thread and whole-process CPU
clocks as well as elapsed wall time, and exports both backends' available CPU
execution diagnostics at warmup/end. The fresh600-refresh Metal controls after
120 warmup refreshes give:

| CPU backend | Elapsed ms/ref | Emulation-thread CPU ms/ref | Process CPU ms/ref |
| --- | ---: | ---: | ---: |
| FastInterp |19.32584|17.31076|20.97095|
| JIT diagnostic |16.67095|12.17990|15.77702|

Receipts: `gadd-v8-fastinterp-control-A` and `gadd-v8-jit-control-A` under
`work/luigi-metal-cce1a67`. JIT reaches the 60 Hz speed cap; FastInterp does not.
CPU time is an attribution boundary, not an exclusive interpreter profile:
it includes renderer calls made by the emulation thread. Removing GPU waits
alone cannot make17.31 ms of thread CPU fit a16.67 ms refresh budget.

Cross-backend100% clock is not identical CPU work. FastInterp ordinary Thumb
block decode assigns one tick, while Dynarmic uses the shared instruction-cost
table (including multi-tick Thumb cases). FastInterp's shared table in Step()
does not establish that normal block execution uses it. Preserve these policies
for the performance experiment; reconcile any timing bug separately with proof.
JIT comparisons therefore include timing/memory/execution effects, rather than
isolating interpreter dispatch alone.

The private FastInterp cache census finds99.89% outer and99.95% fallback hits,
with no post-warmup clears/invalidation events. Decode sampling estimates about
0.026 ms/ref, including clock overhead, so larger decode caches are not the
supported major target. Hot resident instruction slots include loads, integer
adds, VLDR, VMLA and VMUL. Entry weights are not CPU-time percentages. Existing
VFP SIMD and paired-VLDR options are now being tested against exact same-backend
pixels/registers/ticks before any default changes.

Actual read-plan instrumentation previously stopped at absolute frames720..840,
so restored600-refresh runs incorrectly reported no activity. It now covers all
instrumented frames. The corrected diagnostic observes39,599 hits /360,001 misses
(9.91% hits). A bounded128-entry, full-key-hashed metadata cache is available
behind `CARA_METAL_VERTEX_READ_PLAN_CACHE_128=1`; legacy16 remains unchanged.
All reads still use fresh payload bytes and full key equality. CPU/GPU fixtures
pass; actual game performance is being measured. Base-relative normalization is
only a read-only proposal until the128-entry incidence is known.

Display crop fusion has zero candidate incidence in the measured lab. Its
passing GPU fixtures do not establish a gain here. The buffer-only linear color
candidate targets a different branch, about6 calls/ref; its existing0.193 ms/ref
scope is an upper bound, not the expected savings. No candidate is promoted
based only on fewer copies, a single favorable run, or synthetic fixtures.


The first same-binary VFP arms preserve exact pixels/registers/ticks but do not
establish an improvement: VFP modes1/2/3 are23.288 /20.718 /19.894 ms/ref;
paired VLDR20.051 ms/ref. Subsequent interleaved controls20.133 /20.100 and
combined VFP3+VLDR20.075 show why the first19.326 control cannot support a
small-gain claim. No VFP default changed. Larger read-plan cache quiet23.341
and diagnostic22.283 ms/ref also do not show a gain; its actual hit ratio rises
to38.18% (152,560 /399,600), leaving247,040 misses. A separate relative-base
key experiment is being implemented with all-range overflow prevalidation.

Additional existing flags remain experimental: late reservation20.095,
old uniform reuse20.481, late+uniform19.477, late+borrow/direct-export19.414,
followed by control19.801 ms/ref. All exact comparisons pass, but drift requires
repeats. Buffer-only linear color20.053 establishes no gain. Measurements and
exact comparison fields are retained in `gadd-v8-candidate-results.json`.

Two concrete bridge costs are now being addressed instead of assuming that
larger batches or fewer shader evaluations guarantee a faster frame. Copy2D
queries the same device/format linear alignment before every immutable cache
lookup; a retained-device, three-format answer memo preserves the returned
alignment and existing validation. Existing uniform reuse disables deferred
vertex compute batching. A distinct deferred-batch memo compares full uniform
bytes within one retained command/arena lease and clears on flush/abort, allowing
both mechanisms to coexist. Both changes remain opt-in pending GPU fixtures
and matched quiet game comparisons.


## Actual host-PC profile and v9 bridge checks

A seven-second Xcode Time Profiler capture of a1200-refresh restored FI run
retains raw distinct leaf PCs, the exact pre-rebuild binary UUID/symbol map and
complete ExecuteBlock disassembly. Earlier `sample` text merged multiple PCs
into one function row, so that row could not identify a dispatch stall. The new
capture has6,616 main-thread running samples:3,296 with SVC ancestry and2,824
ExecuteBlock self samples. Of the latter,1,486 land on decoded-instruction field
loads. Leading sites include LdrImm's base-register selector171, shared dispatch
lookup116, AddImm selector99, MovLslImm selector73 and MovImm literal62. The whole
shared dispatch-tail region is166 samples (5.88% of interpreter self,2.51% of
main running samples). Samples do not distinguish cache misses, dependent-load
latency or branch skid; arithmetic instruction samples alone do not attribute
complete VFP-handler cost. This favors a measured code/operand-layout experiment
over declaring dispatch or floating-point arithmetic the sole bottleneck.
Evidence: `gadd-v8-host-pc-profile/{host-pc-summary,host-instruction-summary}.json`
and the retained trace/XML. These are instrumented diagnostics, not speed runs.

The new alignment memo reduces actual game device queries from799,200 to2,
with799,198 cache hits. Relative128 read plans have141,510 hits /258,090 misses
(35.41%), worse incidence than the absolute128 experiment. Deferred uniform
reuse has **zero admissions** in the current baseline because vertex compute
batching is disabled. It passed24 paired raster/compute GPU cases in each of
four reuse/dedup controls, preserving28 dispatches and655 computed draws, but
cannot speed this baseline merely by being compiled. Both options remain off.

Current20B/ThinLTO v9 quiet controls are18.844 /19.105 ms/ref with16.875 /17.119
thread CPU ms/ref. Alignment memo alone19.177 /16.697; LDR_SP_STATIC18.790 /17.032;
alignment+old uniform reuse19.250 /16.880; alignment+LDR_SP_STATIC19.444 /17.263.
These first arms establish no stable elapsed gain. The initial19.123 control and
combined20.861 instrumented diagnostic overlap observed system security scan
activity and are not clean timing evidence. No system service was disabled.
The exact-output receipts are in `gadd-v9-candidate-results.json`.

The v9 app/audit/state fingerprint are frozen at `work/build-frozen-gadd-v9` for
compiler-only ABA comparisons. The next experiment is private FI-only PGO,
keeping the20B layout, current ThinLTO and guest policies. Generation timings
must be marked instrumented; inspect actual compiler profile/branch metadata
before attributing a speed change. Compact16 retains the operand union atoffset4,
adds packed-header extraction and previously enlarged executor code; it is not
a direct fix for the sampled base-selector loads.


## FI-only frontend PGO result

Generation/use now have explicit audited builder options, defaultoff. Only the
FI execution translation unit is instrumented; its profiling runtime propagates
to final core consumers. The profiler isolates raw output inside the run and
marks generation automatically instrumented. The actual Xcode-matched merge
contains ExecuteBlock2,803 counters and11,724,794 entries. Actual used ThinLTO
IR has957 data-weighted conditional branches, but its single computed-goto
indirectbr has no destination weights. The next-symbol executor span grows
49,340→62,872 bytes, so neither smaller code nor better dispatch follows from
profile consumption. Full IR/binary/metadata evidence is retained in
`gadd-v10-pgo-ir-audit`; the training run `gadd-v10-control-A` is instrumented,
despite its historical output name.

Clean compiler ABBA (`gadd-v11-*`) is19.032 /18.600 /18.955 /18.945 ms/ref
(NoPGO/FE/FE/NoPGO), thread CPU16.683 /16.405 /16.669 /17.113. Every endpoint
pixel and retained CPU/tick hash matches across compiler variants, and repeated
same-binary guards pass. Mean elapsed benefit is about1.1%, with enough spread
that this is not a major or proven cross-device gain. The frozen FE app is
`work/build-frozen-gadd-fe-pgo`. No shared execution default changed. An IR-level
PGO follow-up is justified by LLVM's actual indirectbr instrumentation regression
test, and must use a fresh separately identified profile rather than the FE one.

IR-level PGO is now separately supported (`--fastinterp-pgo-kind ir`), with
fresh IR training, exact-toolchain merge and profile-kind validation. Its actual
used ExecuteBlock IR has491 weighted computed-goto destinations (aggregate,
not per-predecessor transitions),790 weighted conditional sites and35 switches.
The executor span grows further to71,376 bytes. Clean ABBA `gadd-v13-*` gives
19.068 /18.745 /18.914 /19.767 ms/ref; all endpoint pixels/CPU/ticks match. The
last control also has higher thread CPU17.890 versus16.699 initially. Thus the
roughly3% mean comparison is affected by drift; IR's18.745/18.914 is not a clear
improvement over FE's18.600/18.955. Neither compiler experiment establishes100%
or warrants a shared default. Actual IR/profile/binary proof is retained in
`gadd-v12-ir-pgo-ir-audit`; generation `gadd-v12-control-A` is instrumented.

A different measured source-cache lead is now under contract review: successful
VS lookups compare the complete16KiB code plus16KiB swizzle arrays on every draw.
The full diagnostic has1,463,912 candidate probes,303,600 recent hits,95,979
fallback hits and21 generations. Successful comparisons alone account for at
least13.09GB/600 refreshes. The0.578 ms/ref VS source scope includes other work,
so it is an upper bound, not promised savings. Downstream GLSL revision/selection
memoization already works; this is not repeated50KB string hashing.

The identity proposal is lazy, globally nonrepeating and nonserialized. Writers
invalidate identities via the existing MarkDirty contract; reset/rawZero produce
zero identities; loading invalidates before reading any archive bytes; copies
retain identity only with identical copied arrays. Full comparison remains cold
or whenever identities change, and all configuration checks remain. A separate
validation mode reruns the complete comparison to detect missing write barriers.
The source changes need lifecycle/writer tests and a fresh normal checkpoint
because normal engine fingerprint guards must remain intact.

Depth-residency follow-up remains unimplemented. Generated D24 output can prove
canonical float representations for valid integer inputs, but source provenance
alone does not yet prove nonfinite/W-buffering/raster-depth domain safety.
Existing DepthExportProof authenticates routing, not canonical post-draw floats.
Keep checked imports/exports and all ownership/failure rules. The documented
48-pixel checkpoint discrepancy remains unresolved; no retained evidence proves
it was caused by a retained-depth experiment or depth quantization.

## Held-out ROM startup smoke checks

`work/other-rom-metal-smoke-2026-11-21/results.json` retains normal600-refresh
FastInterp/native-Metal startup attempts for every other Downloads `.3ds` file,
on the frozen IR-PGO app. These are compatibility probes, not full-game testing
or clean performance evidence (CPU fixture compilation overlapped).

| ROM | Observed result |
| --- | --- |
| Cooking Mama5 |600 presentations/capture complete; visually verified Office Create logo and Cooking Mama image |
| Driver: Renegade |Stops at16: framebuffer/configuration,480x400 framebuffer versus240x320 viewport |
| Ever Oasis |Stops at27: unsupported512x400→480x400 display transfer, flags`0x01002204` |
| Monster Hunter Stories |Stops at175: RGBA8 sampled color alias256x512 at`0x180d4800` |

These three failures are real Metal compatibility gaps and must not be labeled
verified-working games. No shader/transfer operation was skipped to turn a
failure into a passing run. Runtime logs and binary-matched audit are retained.

The async-depth investigation corrected another attribution: the joined path
already keeps exact depth export/blit/import on GPU buffers. Its demonstrated
remaining stall is synchronous CPU inspection of error flags after command
completion, not another assumed CPU byte round trip. Optional asynchronous
receipts must retain both error buffers on their own ticket and every dependent
writer/predecessor snapshot; GPU completion alone cannot mean valid guest data.
Queue retirement, CPU read/export, checked drain and capture/final success must
consume failures. The iOS frontend now checks completion/capture inside a Metal
catch before reporting benchmark success; full iPhoneOS27.2/arm64/iOS16 syntax
validation passes. This is not an iOS link/device test. Normal Mac terminal
RESULT was already protected by VideoCore shutdown drain; the earlier concern
that Abort alone protected shutdown was corrected after tracing that route.

## Verified identity and asynchronous bridge results

The complete renderer-facing rebuild changes the guarded checkpoint signature;
`gadd-v14-verified-seed` was created by a normal3600-refresh boot using the
original filesystem and exact A-button script. Its final image is byte-identical
to the original normal lab reference. No fingerprint was rewritten.

Shader identity removes repeated program/swizzle comparisons after exact initial
validation. The validation-enabled600-refresh game diagnostic observes399,558
fast candidates and **zero invalidation failures**; validation deliberately still
compares their bytes. Async checked depth admits1,200 imports and passes1,200
checks with zero failures. GPU fixtures cover pending reads, both error buffers,
all256 stencil values, FIFO dependencies, invalid depth, abort and an already
committed command that fails before obtaining a queue slot. That error path now
waits for the unslotted command before returning failure.

Same-binary restored controls (`gadd-v14-*`) are18.899 /18.706 ms/ref; identity
alone17.999, asynchronous depth18.257 and combined17.987. These are actual exact
pixel/CPU/tick comparisons, not GPU-operation-count projections. A fresh normal
boot with both options is22.738 ms/ref and16.304 thread CPU ms/ref, versus the
earlier normal23.713 /17.163. The much faster restored result is therefore not
a complete normal-play performance claim.

Matched diagnostic480-refresh windows show exactly14 draw commands each refresh
in both normal and restored cases; final CPU/tick/VFP boundaries match after
removing refresh numbering. Normal draw-command duration sums are21.354 ms/ref
versus13.995 restored; combined GPU interval unions15.867 versus10.729. These
are interval coverage, not exclusive stage execution. Normal is stable across
120-frame quarters while restored intervals fluctuate. Exact render-target
dimension/format/usage guards rule out an oversized historical backing as an
accepted production path; per-command counts alone still do not prove identical
vertex counts, shader inputs or fragment work.

`tools/profile_apple_gpu_states.py` records bounded system-wide IOReport residency
using a private macOS API **only in developer tooling, never the application**.
Active-weighted MHz are not instantaneous or process-specific clocks. Raw states
and the device's nonmonotonic frequency table are retained; unmapped active
states yield no inferred frequency. The actual pair reports normal/restored
whole-run575.11 /570.60 MHz, and trailing593.20 /575.72; this does not establish
an explanation for the earlier1.5× command-duration gap. The observed normal run
is faster with sampling (17.809 versus22.738 ms/ref without), so observer/UI/
resource-history effects remain confounds. The sampler's subscription teardown
matches Apple/macmon release paths; persistent AGX power changes are unproven.
The restored long run fails the endpoint scene guard and is not an exact-work
comparison. Do not use it to claim sustained scene parity.

## Bounded queue and reservation experiments

`CARA_METAL_SUBMISSION_WINDOW=8|16|32` retains default8. All ticket, command and
validation predecessor bounds cover the selected window; no check is elided.
GPU fixtures pass three wrap rounds at each size, first/last-slot failures,
receipt propagation, orphan-command cleanup and terminal abort; malformed
values fail explicitly. Max32 ticket metadata is99,840 bytes versus24,960
previously; additional pending transfer resources can be much larger (worst-case
hundreds of MiB). It is an experiment, not a low-memory-device default.

Restored window8 controls17.781 /17.614 ms; window1617.429,3217.526,
32+eight-pages17.570,16+eight-pages17.613. These do not justify a broad promotion.
Host thermal/low-power/QoS/window-focus snapshots are now retained at warmup and
finish; observed controls are nominal thermal, low-powerfalse, QoS33, unfocused
but neither hidden nor minimized. Host display sync remains enabled. Disabling
it did not establish a baseline win and is not used to claim full speed.

A more specific ordering improvement moves color reservation **after** exact
RGBA authority lookup: metadata recovery emits no command and needs no new slot.
`CARA_METAL_COLOR_LEASE_NO_RESERVE=1` preserves initial retirement, pending-draw
publication, source status and tracked-queue checks.24 GPU cases cover completed
and tracked-pending source recovery, a real subsequent blocking read, exact bytes,
partial/missing owner imports, late-record publication and terminal failure.
No real GPU driver fault was injected.

`gadd-v19-*` exact restored controls17.732 /17.731 ms; color alone17.653,
color+late-draw17.333, color+late+eight-pages17.130. Larger windows did not help
that combination. The8-page/late/color combination needs repeats in normal play.
With that bundle, alignment memo plus existing ALU pairs reaches16.806 ms/ref
(about99.17%) and15.572 thread CPU ms/ref (`gadd-v21-alu-A`); controls with
alignment but without ALU pairs are17.131 /17.117. Borrowed exports, VFP pairing
and secondary links do not improve that arm. All same-binary endpoint guards
pass. This remains a developer reference-profile replay result, **not a stable
100% normal-boot or shipping-default result**.

Completed predecessor status is now being tested as a further opt-in. Published
snapshots are immutable; only all-command/all-receipt terminal success can be
memoized, and pending is never cached. Receipts serialize first inspections and
use atomic terminal state, with GPU waits outside their mutex. Concurrent GPU
fixtures (nine readers), all32 slots and existing async/window regressions pass.
A new normal checkpoint is required after its header change; performance is not
yet established.

## Normal-play follow-up and focus-control limitation

The new build (`5886a560f5d5147943b364ede5a1ef6d026d6936d7ac67b211bb66cfef98055a`)
retains the full reference bundle, ALU pairs and IR PGO. The normal-boot
`gadd-v22-verified-seed` interval3120..3600 is17.432 ms/ref (95.61%), with
16.103 thread CPU ms/ref. `gadd-v24-normal-foreground`, additionally enabling
completed-predecessor memoization, is17.310 ms/ref (96.28%), with16.135 thread
CPU ms/ref. Both exactly match the original normal-play endpoint pixels and
guest boundary. These two observations alone do not isolate a memoization win.

Same-binary restored controls are16.829 ms/ref; completed-predecessor memoization
reduces thread CPU by roughly0.15 ms in repeats but does not establish an elapsed
win. Larger submission windows16/32 remain slower in that combination. Keep
their default at8.

The `gadd-v23-*` focus experiment did **not** achieve foreground activation:
the public AppKit helper returned activationRequestSent=false. All three arms
remain unfocused, with elapsed16.803 /17.008 /16.738 ms/ref. The subsequent
`gadd-v24-normal-foreground` process also exited before desktop activation;
both saved host snapshots report window_input_focus=false. Its external label
describes an intended condition only. Do not interpret either directory name
as proof of foreground scheduling or attribute timing differences to focus.

Normal-play v24 refresh intervals have median16.717 ms, p95 20.807 ms and
p99 21.652 ms. Restored v22 control has median15.837 ms and p99 21.868 ms.
These quantiles exclude final capture/drain; interval elapsed above includes it.
The remaining work therefore includes tail latency, not only average throughput.
Raw timing review and observed focus are retained in
`work/luigi-metal-cce1a67/gadd-v24-normal-foreground/timing-review.json`.

## Unique-transform admission: correct, still slower

`CARA_METAL_UNIQUE_TRANSFORM_ADMISSION=1` changes the deferred gather admission
limit to transformed unique vertices rather than raster corners. Output remains
bounded at8192 transforms/draw; gathered raster input retains its65536-corner
limit. Other ownership, uniform, depth-clear and grouping gates remain intact.
The option is default-off. Fixed-size rejection/histogram/group-size counters
are exported only with native cost profiling; their warmup/final differences
describe the measured interval. Peak output bytes are a lifetime maximum.

Eight validation configurations (admission, deferred uniform memo, transform
dedup off/on) pass35 paired GPU cases each:280 pairs, exact pixels, uniforms and
quaternion checks under Metal API and GPU validation. Real8193-corner streams,
8192/8193-unique boundaries, strip/fan/list ordering and the64 MiB output ceiling
are covered. A long-fan fixture initially used crossing vertex geometry and
failed its baseline coverage oracle; fixing its perimeter order retained the
strict oracle and required no production change.

Same-binary `gadd-v25-*` clean restored controls are17.387 /17.263 ms/ref;
existing unique compute17.942 and wider admission17.842. Thread CPU is
16.000 /15.936 in controls versus16.907 /16.882 with compute. Every comparison
passes exact endpoint pixels, guest boundary, timing policy and input identity.
This does **not** justify enabling unique compute by default.

Instrumented interval120..600 has319680 draws and213700740 raster corners in
both arms. Wider admission removes1920 legacy-corner rejections and admits
313920 versus312000 requests. It does not alter group counts:19200 singleton
groups,17280 two-draw groups,8640 four-draw groups, plus the larger buckets.
Lifetime peak output is1741824 bytes in both. These counters identify real
batch fragmentation and CPU overhead for follow-up; they are not evidence of
elapsed gains. Receipts: `work/luigi-metal-cce1a67/gadd-v25-comparison.json` and
`work/portable-metal-checks/unique-transform-admission/results.json`.

The current ALU-pairs FastInterp executable also passes the22000 ARM/Thumb
differential cases, fused loops, page/context transitions, tracked VFP fallbacks
and SIMD trace slices. This guards interpreter semantics, not full-game memory
equivalence.

The current IR profile was trained by `gadd-v12-control-A` with no explicit
FastInterp overrides; the later best arm enables ALU pairs. A fresh profile
trained with the actual selected handler policy is therefore a justified next
control, not an established gain. Any new dispatch/handler changes require a
fresh compatible profile. Keep strict stale/unprofiled-profile compiler errors;
do not suppress them to reuse old `ExecuteBlock` weights. The last GPU-isolation
IR binary and matching audit are frozen in `work/build-frozen-gadd-v25`.

## Mixed compute and raster packets

Follow-up `CARA_METAL_DEFER_RASTER_FALLBACKS=1` can retain known raw raster-only
packets between computed packets. It does not move work across command,
attachment-backing, feedback, ownership, depth-clear or visibility boundaries.
The existing multi-program compute encoder was already present: shader program
changes alone were not the cause of a batch flush. Real v25 admission counters
instead identify invalid-unique and transform-limit fallbacks as candidates.
Sixteen configurations pass44 paired GPU cases each (704 total), including
mixed packet order, changed source/index/uniform/quaternion data, the actual
DrawGroup feedback rejection,65-packet capacity and64 MiB output limit. The flag
stays off pending elapsed-time comparisons; see `docs/research-references.md`.

The next IR generation build includes the new presentation guard, shared
metadata defaults and default-off retained-slot LDR pairs. Its normal3600-
refresh training boot (`gadd-v26-training-ldr-on`, ALU and LDR pairs enabled)
exactly reproduces original pixels and guest-boundary hashes. Its compiler
instrumentation and cold shader warmup make elapsed values unsuitable for
performance claims. Two additional matched restored training runs select LDR
pairs off/on so the merged profile can cover both candidate paths. Actual
optimized-build comparisons follow; no speed gain is implied by training.

## Presentation validation boundary

A source review found that the asynchronous depth-import option fenced CPU
readback but could reach `MetalLCD::PresentNative` with an unchecked shader
validation receipt. A shader error flag is not a Metal command-buffer error.
The shared lane now waits/checks outstanding validation receipts before a
drawable or capture becomes visible; later unrelated commands remain pending.
Internal-only coalesced refreshes retain overlap. The actual production Present
path is exercised with real gated GPU depth imports and an intercepted final
LCD call: valid-pending, invalid drawable/capture, completed and coalesced cases
pass. Exact helper tests cover both error buffers and malformed/mismatched
receipts. Receipt: `work/portable-metal-checks/presentation-validation/observed-result.json`.

Earlier asynchronous-option performance results predate this fix and are not
current acceptance evidence. The fenced v29 grouped comparison below includes
the option, and the accepted 42-default source policy enables it together with
checked-depth join. Ordinary-default v30 acceptance now passes in the local
window, as recorded below.

## Production policy boundary

The source policy has been promoted from 24 to 42 unconditional flags, with
additional large snapshots and eight upload pages enabled on devices with at
least 6 GiB physical memory. The existing iOS call still requests eight pages
explicitly, including on lower-memory devices. This is a **source-policy
promotion built and accepted as v30 in the local test window**.
The v29 measurements below used explicit options on the same pre-promotion
binary; v30 subsequently verifies ordinary renderer defaults without those
optimization overrides. That acceptance does not establish full-title or
cross-device 100% performance.

The 42 flags cover the measured reference's queue, ownership, completed-fill,
copy-batch, vertex read-plan/staging/source-policy, checked-depth join/fusion,
async validation and borrowed-RAM optimizations. Ordinal vertex lookup is added
on its separate matched CPU evidence (about 0.23 ms/ref); it was not in the grouped
full-tuned arm. Learning/prewarm remain independently selected controls and must
be recorded in comparisons. Unique/deferred compute, untracked uploads, SIMD,
PC230 specialization and latency-critical host activity remain experimental.

Explicit zero, empty and custom environment overrides remain authoritative.
The updated policy test passes Mac/iPad and 3/4/6/8 GiB cases. Index metadata is
bounded to 5 MiB; completed-fill backing to 16 MiB. Eight 2 MiB upload pages use 16 MiB
instead of 6 MiB, an additional lazy 10 MiB per lane. Larger staging retention is
also bounded by admitted input sizes. No wider submission window is promoted.
The asynchronous path retains the presentation validation fence above; the
reservation changes preserve predecessor/status checks and reserve slots before
actual new commands. Validated ALU pairing remains shared by GLES/Metal without
changing guest clock or timing. Local M3 Pro evidence does not establish M1/M4/
A-series speed, low-memory iPad pressure or sustained thermal behavior.

## Verified callback/cache reductions and remaining policy gap

Matched480-refresh diagnostic snapshots show index rescans142533→847, with
memo hits177147→318833 (55.75%→99.735%). The1920 large-stream scans outside the
legacy memo's32KiB limit disappear. Vertex preparation falls1.145→0.586 ms/ref;
timers are instrumented observations, not exclusive CPU totals.

Watch retirement releases nine stale FCRAM pages before3120. No further releases
occur during3120..3600;313 FCRAM watches remain protected. Publication calls
fall4345921→35041, exactly matching the restored interval. Memory-write /
invalidation sample ancestry falls to zero. Both diagnostics retain319680 draws
and213700740 raw corners. Receipts are in
`gadd-v28-normal-watch-assoc-diagnostic/matched-index-memo-deltas.json` and
`copy-scan-leaf-attribution.json`.

Clean v28 uninterrupted controls are18.076 ms/ref (16.860 thread CPU) and watch
retirement17.481 (16.250). Watch+associative has a22.405 /16.242 outlier and an
externally labeled repeat17.213 /15.797. The attempted desktop activation failed;
that repeat remained unfocused and is not foreground evidence. These v28 runs alone did not establish normal-play stability; the uninterrupted
v29 follow-up below is separate evidence, not a reinterpretation of the outlier.

`gadd-v29-normal-seed` is a newly verified functional Gadd checkpoint at global 3000,
saved only after elapsed/capture/CPU snapshots. Its120-refresh timing is not
acceptance. Correct global 3120..3600 restored comparisons now yield:

| v29 arm | Completed ms/ref | Thread CPU ms/ref |
| --- | ---: | ---: |
| Tuned reference control A / B |16.734 /16.674|14.699 /14.633|
| Borrowed RAM snapshot comparison |16.661|14.219|
| Optional host latency-critical activity |16.658|14.604|
| Borrowed comparison plus activity |16.660|14.261|
| Ordinary shared 24 defaults + learning/prewarm |21.215|16.899|

All same-build pixel, CPU/tick, input and clock guards pass. Borrowed comparison
avoids the scratch copy only on a fully byte-validated RAM cache hit;100 production
GPU cases cover fallback/mutation/ownership/lifetime behavior. The later grouped
comparisons support its source-policy promotion; the later v30 run verifies
ordinary-default acceptance in the local window. LatencyCritical has no established CPU-frequency benefit and
remains a separate host experiment. Physical M1/M4/A-series and full iOS linking
remain unverified; six changed native/interpreter/iOS entry sources pass arm64 iOS16 syntax against
the actual iPhoneOS27.2 SDK (`work/portable-metal-checks/ios-v29-syntax/results.json`).


## Uninterrupted v29 normal play and 42-default promotion candidate

`gadd-v29-normal-control-A` boots normally with the original inputs/filesystem,
FastInterp, 100% guest clock and the explicit tuned configuration, including
ordinal lookup and borrowed snapshot comparison. There is no restore and no
save before or during the measured global 3120..3600 interval; an optional
checkpoint is saved only after timing, capture and boundary snapshots.
Completed-work wall time including final GPU/capture drain is **16.713252 ms/ref
(99.72% of 60 Hz)**, with 14.763763 thread CPU ms/ref. Endpoint pixels and retained
CPU/ticks exactly match the original verified normal endpoint. This establishes
this local window, not uninterrupted 100% gameplay across the title or other
Apple hardware.

The otherwise matched `gadd-v29-normal-latency-A` measures 16.691765 ms/ref and
14.659872 thread CPU ms/ref, with exact same-build endpoint guards. The 0.021487 ms
wall-time difference from a single pair does not independently establish a
latency-critical benefit. That host experiment remains outside shared defaults.
Normal endpoints and restored replay endpoints are separate comparisons; do not
merge their hashes or histories.

The grouped restored campaign uses global 3120..3600-equivalent work on one v29
binary, with learning/prewarm held explicitly enabled and completed-work timing
including final GPU/capture drain:

| v29 restored policy arm | Completed ms/ref | Thread CPU ms/ref |
| --- | ---: | ---: |
| Shared 24 control A / B |21.214799 /21.272099|16.898528 /16.886856|
| Queue/ownership/fill/copy bundle |17.678809|15.667353|
| Vertex bundle |20.550465|15.903470|
| Checked-depth bundle |20.803325|16.836384|
| Queue + vertex |17.503319|14.901701|
| Queue + vertex + depth |17.045710|14.802525|
| Previous + fenced async + borrowed comparison |16.680386|14.451586|

All arms pass exact same-build pixels, CPU/tick boundary, input and clock guards.
These are grouped measurements, not isolated gains for every individual flag.
Receipts are `work/luigi-metal-cce1a67/gadd-v29-restored-production-*/summary.json`;
the uninterrupted receipts are `gadd-v29-normal-control-A` and
`gadd-v29-normal-latency-A`. Source 42 incorporates these mechanisms plus the
separately measured ordinal lookup. The v30 ordinary-default acceptance below
now verifies the promoted configuration; no release-wide or cross-device 100%
claim follows.


## v30 shared 42 ordinary-default acceptance

The promoted policy is now built and passes local ordinary-default correctness
and performance acceptance. Binary SHA256:
`878b935d2da2e8cfa73c740de395531f19808339fc2dac04cc5e4025257c3af5`.
The only explicit Metal overrides are pipeline learning and prewarm-all, both 1;
there are no explicit renderer optimization overrides. Explicit FastInterp ALU
pairs 1 equals its shared default. These learning conditions remain part of the
result and are not a cold-start guarantee.

| v30 arm | Completed ms/ref | Thread CPU ms/ref |
| --- | ---: | ---: |
| Uninterrupted normal boot, global 3120..3600 |16.8380328125|14.9178464417|
| Uninterrupted normal repeat B, same interval |16.8557552958|14.9889483500|
| Uninterrupted normal repeat C, same interval |16.8618915792|14.9892621521|
| Restored control A, local 120..600 |16.7918703125|14.4886408000|
| Restored control B, local 120..600 |16.7205920146|14.3311170146|

The normal run has no restore or save before/during its measured 480 refreshes;
its optional save is after timing, capture and boundary snapshots. Completed-work
elapsed speed is about 98.983% of 60 Hz; the separately weighted speed-window
summary is 99.14562%. Refresh p99 is 21.5965 ms. Timing includes the final GPU/capture
drain. The exact normal endpoint pixel hash is
`ce5401000d9528f6dc6900405e7f3a05496ed9033e0cd00eb1fee8a1f8e63d71` and retained
CPU/tick boundary hash is
`dce6d0e2f5947e50701001c0f86da874964d0c69476b696200c50d2fdf079356`, both matching
the original verified normal endpoint. Restored B passes every strict
same-build comparison with A, including binary, checkpoint, filesystem, input,
pixels and CPU/tick guards. Normal and restored endpoints remain separate tests.

Receipts: `work/luigi-metal-cce1a67/gadd-v30-normal-control-A/summary.json`,
`gadd-v30-restored-control-A/summary.json` and
`gadd-v30-restored-control-B/summary.json`. This is **near 100% in a local M3 Pro
Gadd conversation window**, not exact sustained 100% across Luigi gameplay,
M1/M4 hardware or iPad/iPhone. Latency-critical activity, untracked uploads and
compute experiments remain outside the accepted shared 42 policy.

Normal B and C omit even the explicit FastInterp ALU-pair override and pass all
strict comparisons against normal A. Their shared defaults produce the same
pixels, CPU/ticks, filesystem/input identity and runtime policy. The three clean
normal controls establish a repeatable near-99% result, rather than exact 100%.

The fresh `gadd-v30-normal-diagnostic` also passes all strict guards, but enables
renderer clocks and host sampling: its18.825552 ms/ref is diagnostic overhead,
not a clean speed result. Binding time is3.485959 ms/ref inclusive: color1.042994,
depth0.284861, vertex capture0.576932 and sampled textures0.753530. Queue-pressure
wait totals1.391984 ms/ref, including draw0.440282, copy0.135510 and sample0.814398;
these overlap inclusive binding scopes and must not be added together. All477120
keyed RAM texture resolutions hit, with zero misses, changes or evictions and
6,134,046,720 compared bytes over480 refreshes. Miss-copy optimization therefore
has no demonstrated benefit in this window. Command timestamp retention is
incomplete (65 missing); interval coverage is not exclusive GPU utilization.

The final shared-policy sources pass six actual iPhoneOS27.2 SDK syntax checks
targeting arm64 iOS16, including the iOS frontend, interpreter, renderer, binding,
draw lane and uploads. This is syntax verification, not a complete iOS link or
physical-device test. Receipt: `work/portable-metal-checks/ios-v30-syntax/results.json`.
Profiler helper tests22/22 and native build/deployment/LTO/PGO checks24/24 pass.

## Other downloaded ROMs on the shared 42 build

The same v30 binary and FastInterp/shared Metal defaults were used for independent
normal boots,600-refresh limits and no input scripts. Learning is disabled for
these compatibility smokes; their timings are not gameplay speed acceptance.

| ROM | Result |
| --- | --- |
| Cooking Mama 5 USA |600 refreshes/presentations and capture succeed; exact same pixels as the earlier boot smoke |
| Driver Renegade USA |Stops at16: framebuffer depth format2 (D24), outside the current D24S8 admission |
| Ever Oasis USA |Stops at27: unsupported512x400 to480x400 display transfer, flags0x01002204 |
| Monster Hunter Stories USA |Stops at175: RGBA8 sampled texture aliases color storage at0x180d4800,256x512 |

These are compatibility gaps, not successful game verification. No unsupported
operation was silently skipped. Full run manifests and stop messages are retained
in `work/other-rom-metal-v30-2026-11-21/results.json`.

## v31 residual CPU experiment and final default check

The fresh v30 sampler has3,400 main-thread samples:1,603 interpreter leaves
outside SVC/graphics,1,145 other graphics leaves,336 wait/RPC,168 profiling-clock
leaves and148 other host/service leaves. These are disjoint sampled-stack groups,
not measured exclusive wall-time percentages. `ExecuteBlock` itself accounts
for1,560 leaves, but1,559 are collapsed across many host PCs; the sample text
cannot establish a dominant guest opcode or dispatch cost. Listed PCs do map
to an ARM unconditional immediate word load and shared dispatch, without their
individual frequencies. Small assembly windows and decoded-label evidence are
retained in `work/apple-cpu-bridge-checks/v30-pc-audit`.

One bounded lead was117 sampled CPU leaves in immutable surface-writer status
checks. The new default-off terminal-success memo passes four actual-GPU/API-
validated configurations, including pending work, GPU-written receipt failures,
authored command failures and concurrent readers. Its restored OFF/ON/ON/OFF
sequence shows no independent throughput gain; it stays off. See
[completion-memo proof and null result](metal-completed-writer-memo.md).

The final v31 default build retains the shared42 policy. A fresh uninterrupted
normal boot measures16.902863 ms/ref including final drain/report I/O,
14.949752 thread CPU ms/ref,99.20338% weighted speed and21.140666 ms refresh p99.
Original normal pixels and CPU/ticks match exactly. This is still near99%, not
exact sustained100%. Binary SHA256:
`501fd9f3efdb63979442330eefbbd188270430dcd6942b22fe9929f14205046a`.
Receipt: `work/luigi-metal-cce1a67/gadd-v31-normal-control-A`.

All four other-ROM boot checks were repeated on v31 with the same result as v30:
Cooking Mama reaches600; Driver/Ever Oasis/Stories stop at16/27/175 with the same
unsupported cases. Six current iOS SDK syntax checks also pass. Receipts:
`work/other-rom-metal-v31-2026-11-21/results.json` and
`work/portable-metal-checks/ios-v31-syntax/results.json`. No M1/M4/A-series physical
device, full iOS link or whole-game smoothness acceptance is implied.

### Raw CPU-PC trace closes the merged-sample gap

A target-only three-second Time Profiler capture succeeds locally (17 MiB),
without enabling renderer clocks. The restored replay passes every strict
same-build endpoint/state/input guard. All3,083 Running main-thread samples are
on performance cores6–11; none are on efficiency cores. This is direct evidence
for this short local trace, not a device-family scheduler guarantee.

There are1,552 interpreter SELF samples across342 distinct PCs. Every tagged
interpreter leaf address joins the raw sampled PC with the exporter’s +1 address
convention. The ARM immediate-load fast body has135 samples, including101 at
its dispatch entry and18 at its page-table load. Two shared dispatch table-load
sites have84. These samples do not establish cache misses or exclusively measure
dispatch latency. Nearly half of interpreter samples occur at handler entries;
frontend/branch costs and decoded-data latency require separate discrimination.
The fused compare/branch path also spends samples on the existing live chain-
budget and self-chain guards; removing timing/event checks is not justified.

Trace/export paths: `work/fastinterp-cpu-v31-3s.trace`,
`work/fastinterp-cpu-v31-3s-{samples,profile}.xml`. Analysis, exact UUID and assembly
joins: `work/apple-cpu-bridge-checks/v31-pc-audit/`. Exported TOC omits incidental
inherited environment data; the private raw trace remains local. The first
orchestration attempt waited for an inactive renderer-counter file and never
attached; its `TRACE-NOT-ATTACHED.json` explicitly preserves that limitation.

## Last-mile normal-boot comparison, November 21

A fresh JIT/OpenGL control from the original in-game filesystem seed and A
script reached the verified Gadd dialogue. Over refreshes 3120–3600 it measured
16.682403 ms/ref (99.906%) and 11.950393 thread CPU ms/ref, with the GPU complete
at both boundaries and the same OpenGL endpoint pixels as the original control.
The control uses the prior audited desktop OpenGL binary. Both backends use
Old3DS and a 100% guest clock. OpenGL and Metal use separate capture and timing
implementations, including final capture/report I/O in the Metal interval.

| Same-scene Metal normal boot | Completed ms/ref | Refresh p99 ms |
| --- | ---: | ---: |
| v31, standard 8 slots | 17.053353 | 21.650542 |
| v31, 16 slots A | 17.652019 | 23.410167 |
| v31, 16 slots B | 17.463788 | 22.481542 |
| v32, standard A | 17.474156 | 22.302292 |
| v32, late color import A | 17.482839 | 21.968125 |
| v32, late color import B | 17.630599 | 22.212250 |
| v32, standard B | 17.533100 | 21.931300 |

Metal endpoints match the original Metal pixels and retained CPU/tick boundary.
Elapsed and p99 values include a range of desktop load and window focus. The
8-slot run and the second 16-slot run both had focus at the measured boundaries.
The v32 arms were unfocused. These runs support retaining the smaller queue
and leave steady 100% unresolved.

The raw PC trace motivates removing load and dispatch work, but the existing
load/store fusion and ADD-to-compare dispatch fusion do not deliver it here.
Restored control elapsed times were 16.795233 and 16.751507 ms/ref. Four load/store
pairs measured 16.798408, ADD-to-compare 17.416439, and both 16.780761. Every
comparison reproduced the same restored endpoint. No CPU tuning was promoted.
An instrumented repeat counted 3,415,162 ADD-to-compare invocations, 1,928
four-pair load/store hits and zero four-pair fallbacks across the 480 refreshes,
all on guest core 0. The load/store fusion has little coverage here. The
dispatch fusion executes frequently and still lacks a clean measured gain.

The color-import prototype moved reservation after encoding and before commit.
Six GPU configurations passed API validation, including the full-queue import
case. In the instrumented restored pair, queue-pressure waits were 0.973146
ms/ref (965 waits) before and 1.059815 ms/ref (1,105 waits) with late reservation.
The wait cost attributed to sample/attachment requests was 0.519138 versus
0.528433 ms/ref. Instrumented results explain the queue behavior under the
profiler. The normal runs also show no improvement, so the prototype was removed.
Existing raster-state reuse (17.180538 ms/ref) and direct/striped transfer reuse
(17.159696) also failed to beat the 17.133476 restored control.

Receipts are under `work/luigi-metal-cce1a67`, in the
`gadd-lastmile-jit-opengl-normal`, `gadd-v31-normal-queue*`,
`gadd-v31-restored-lastmile-*` and `gadd-v32-*` directories. The reconstructed
v32 experiment remains reproducible from the retained source patch and
frozen binary. See the [Azahar comparison](metal-upstream-research-2026-11-20.md#azahar-stereo-skip-and-shader-compilation).

The post-removal release rebuild reproduced the v31 binary SHA exactly. Its
final uninterrupted normal replay measured 17.403927 completed ms/ref
(95.76383%), 16.033550 emulation-thread CPU ms/ref and a 22.049709 ms refresh
p99. All same-build endpoint and run guards passed. The earlier near-99% result
and this slower repeat use identical binaries, which makes the run-to-run
variation part of the remaining acceptance problem. Receipt:
`work/luigi-metal-cce1a67/gadd-v31-normal-lastmile-final/summary.json`.

## Cached input retirement and scheduling split

The v33 experiment `CARA_METAL_RETIRE_CACHED_INPUTS_AT_RECORD=1` skips repeated
nonblocking retirement polls when returning existing attachment or exact RAM
snapshot leases. Record still checks the queue before draw encoding. Publication
still polls on a cache miss or pending copy batch, and the guest-read, reservation
and presentation guards remain in effect. Compare the ownership boundaries in
[Azahar's Vulkan scheduler and completion code](metal-upstream-research-2026-11-20.md#azahar-completion-progress-and-caras-input-cache).

The restored OFF/ON/ON/OFF sequence measured 17.141054, 17.004963,
16.934400 and 17.320292 completed ms/ref. Thread CPU was 16.004455,
15.611678, 15.622820 and 16.302575 ms/ref. All exact comparisons passed.
Adding the existing immediate SP-base load specialization measured 17.084378
with 15.886325 thread CPU ms/ref. It did not improve this sequence.

Both instrumented arms submitted 6,720 draw groups with 2,880 at capacity.
Draw commands at the head of the queue accounted for 95–96% of measured
blocking wait time. The candidate changed that total from 0.990340 to
0.938602 ms/ref. Depth acquisition fell from 0.540485 to 0.300252 ms/ref
while the tracked draw-command p99 remained near 2.35 ms. Instrumentation
overhead affects these runs. The per-command intervals measure command
duration, and overlapping commands can share the same device time.

The retained FastInterp block census gives a narrower alternative to the
four-load/store fusion. In 124 matched, positively weighted ARM block rows,
the static pattern of a word load immediately followed by a compare consuming
the loaded register and then a conditional branch accounts for 8,820,480
entry-weighted slots. Immediate-offset loads account for 5,691,840 of those.
These are bounded resident-block estimates from the top 128 rows, not
executed instruction counts. This identifies a direct dispatch edge to test
while preserving the existing slow-memory callback and compare/branch path.
No new fusion is enabled by this analysis. Receipt:
`gadd-v31-restored-lastmile-census/load-compare-branch-adjacency.json`.

The fresh JIT/OpenGL control averaged 16.675890 ms/ref (99.94469%) and
11.463102 thread CPU ms/ref. The matching JIT/Metal control averaged
16.685101 ms/ref (99.88952%), 11.491787 thread CPU ms/ref, with a 16.910250 ms
refresh p99. Both ran from the normal savedata seed to the verified Gadd
dialogue with a 100% guest clock and cap over refreshes 3120–3600. The Metal
control visually matches the scene. Endpoint equivalence across CPU backends
or renderers is outside this comparison. The JIT control demonstrates that
native Metal can complete the lab at the cap with substantially more host CPU
headroom. Its thread CPU metric includes renderer work and therefore cannot
attribute the entire difference to interpreter dispatch.

The Metal API fixture exercised four configurations and seven cases per
configuration for the retirement candidate. An authored producer failure
after a resident attachment lookup is rejected by Record before encoding and
the subsequent guest read still fails. The 13-case texture snapshot fixture
also passes with the candidate and the shared borrowed/keyed/large settings.

| Normal boot arm | Completed ms/ref | Thread CPU ms/ref | Refresh p99 ms |
| --- | ---: | ---: | ---: |
| Stock A |17.515836|16.310632|22.026583|
| Cached retirement B |17.307895|16.203167|22.268250|
| Cached retirement C |17.454938|16.322605|21.965917|
| Stock D |22.670814|16.444880|25.181917|
| Fresh stock repeat E |17.217240|16.334208|21.961833|

Every FastInterp arm matches the reference Metal pixels and CPU/tick boundary.
Stock D had a mid-run slowdown with comparable thread CPU, input focus, QoS
and reported thermal state. Cause is unresolved and a new stock run did not
reproduce it. Across stock A/E and cached B/C, the completed timings average
17.366538 versus 17.381416 ms/ref, within control variation. Thread CPU averages
16.322420 versus 16.262886 ms/ref. The CPU gain found on the restored checkpoint
does not translate to an established normal-boot throughput or pacing gain.
Retain the tested option for profiling with the default off. A current iPhoneOS
SDK syntax check passes for the changed Metal draw lane.
`work/luigi-metal-cce1a67/gadd-v33-normal-comparison.json` retains all arms.

## Direct ARM load to compare dispatch

`CARA_FASTINTERP_LDR_CMP_BRANCH=1` selects eligible unconditional immediate
word loads whose original successor is a retained fused compare/branch
consuming the loaded register. It selects one entry for immediate compares
and another for register compares. A direct same-page read writes the original
destination register, charges its original ticks, advances the original PC,
then enters the existing compare/branch handler. Page crossing, tracked and
MMIO reads take the original scalar handler and its ordinary dispatch.
The comparison, branch condition, block boundaries and chain budget remain
governed by the original instructions.

Release validation passes 6,144 differential cases in both settings, including
both dispatch banks, page crossings, MMIO callback counts and tracked memory.
Both modes produce digest `3266525559543265123`. The enabled fixture admits
3,072 immediate and 3,072 register edges. The 22,000-case ARM/Thumb suite also
passes with the edge enabled. Release PGO uses equal-length OFF and ON training
replays with exact endpoint guards and the strict profile checks.

The local build hit ENOSPC in Apple's static archive indexing and the final
disk link. The repaired build used an indexed Darwin archive from LLVM ar and
the same Xcode ThinLTO link commands with temporary outputs on a local RAM
volume. The signed executable passed the ordinary bundle and compiler-policy
audit. The changed link destination and the disabled linker-generated ad-hoc
signature were confined to the scratch build. The audit applies the normal
entitled signature before emulation.

In the restored release sequence, completed ms/ref were 16.766229,
17.037726, 17.520522 and 17.288268 for OFF/ON/ON/OFF. The matching thread CPU
times were 15.933732, 15.903441, 16.787357 and 16.380403. All exact guards
pass, and the samples do not establish a gain. The clean replays preserve
their measured window and final GPU/capture drain. Runtime timings from the
instrumented training binary are excluded.

For the normal-boot comparison, `CARA_NATIVE_REFRESH_CADENCE=1` records OpenGL
RunLoop refresh timestamps in memory and writes the CSV after collection.
Metal retains the equivalent cadence report. The slowest 120-refresh window
uses the minimum of four consecutive 120-refresh groups in global 3120–3600.
These are emulation cadence measurements. The OpenGL completed interval uses
GPU-finished snapshots, and the Metal interval includes its final GPU drain
and capture/report I/O.

The first enabled normal run lost its final timing CSV to ENOSPC and was
excluded. A byte-verified transparent compression of generated static
archives restored space while retaining their paths and logical contents.
The replacement replay and the second enabled replay meet the exact Metal
pixel and CPU/tick guards. See the benchmark summaries and the archive
compression receipt under `work/luigi-metal-cce1a67`.

| Normal boot, Gadd lab | Mean ms/ref | Speed | Slowest 120-refresh window | Refresh p99 | Refreshes over 20 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| JIT + OpenGL | 16.678 | 99.93% | 99.73% | 19.72 ms | 3 |
| FastInterp + Metal, edge B | 17.359 | 96.01% | 95.65% | 21.76 ms | 43 |
| FastInterp + Metal, edge C | 17.516 | 95.15% | 94.32% | 22.17 ms | 47 |
| FastInterp + Metal, stock D | 17.676 | 94.29% | 90.88% | 22.69 ms | 60 |
| FastInterp + Metal, stock A | 22.568 | 73.85% | 73.30% | 25.22 ms | 480 |

Both enabled runs match the same-binary Metal pixels and guest boundary.
The fresh JIT/OpenGL control reproduced the historical OpenGL image. It
completes with the same guest clock and lab dialogue. Cross-renderer pixels
and the runtime CPU engines are distinct. The first stock arm again reproduced
the slow presentation/queue pattern from an earlier campaign, while thread
CPU remained 16.35 ms/ref, close to the closing stock arm's 16.38. The cause
of that stall remains unresolved. The enabled mean is modestly faster than
the closing stock arm, and the restored bracket does not establish an
independent gain. The option stays off by default.

The restored block profile identifies 1,538,880 immediate-compare edges and
4,152,960 register-compare edges by entry-weighting matching decoded slots in
the retained top-128 blocks. These are not dynamic dispatch counts. The raw
frame cadence and final timing receipts are summarized in
`work/luigi-metal-cce1a67/gadd-v34-normal-comparison.json`.

Paired three-second Time Profiler captures also pass the restored replay's
exact guards. Normalized tagged samples land on both direct entry addresses
with the flag enabled (eight immediate and four register entries). The OFF
trace has none at those addresses. ExecuteBlock leaf weight is 48.93% of
running main-thread sample weight OFF and 49.08% ON. A short sampling pair
establishes handler use and does not resolve a change in instruction cost.
Both exports initially hit temporary-storage failures and were recovered
from the retained traces. The table extracts and time profile summary
remain under `work/luigi-metal-cce1a67/gadd-v34-*`. The signed Metal app,
OpenGL runner and FastInterp test binary were copied back to the workspace
and matched byte hashes before the temporary RAM volume was detached.

## Slow normal-boot Metal queue capture

On the Apple M3 Pro, the original Luigi's Mansion: Dark Moon savedata and
A-button script boot normally into the Gadd lab. The measurement covers
refreshes 3120–3600 at the original guest clock. The direct FastInterp
load/compare/branch option was omitted. Both clean Metal runs reproduced
the exact Metal pixels and sampled guest CPU/tick boundary. The fresh
JIT/OpenGL run reproduced the previous OpenGL endpoint image and recorded
GPU-complete timing boundaries. Refresh intervals mark RunLoop/VBlank
progress. They are independent of the display's physical scanout and the
number of unique game frames.

| Normal boot | Completed ms/refresh | Speed | Slowest 120-refresh window | Refresh p99 | Over 20 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| JIT + OpenGL | 16.667 | 100.00% | 99.98% | 21.08 ms | 8/480 |
| FastInterp + Metal, clean slow boot | 22.216 | 75.02% | 74.98% | 23.87 ms | 480/480 |
| FastInterp + Metal, clean faster boot | 17.234 | 96.71% | 95.88% | 22.25 ms | 44/480 |
| FastInterp + Metal, display sync disabled | 21.304 | 78.23% | 73.94% | 24.57 ms | 391/480 |

The clean Metal boots use the same executable. Their emulation-thread CPU
time is 15.810 and 15.711 ms/refresh while wall time differs by 4.982 ms.
An external five-second stack sample started after the slow regime was
visible and ended before the lab timing window. Of 3,904 main-thread
samples, 991 were in Metal command-buffer completion waits, including 483
from color attachment acquisition, 340 from draw recording/publication,
and 137 from texture-copy publication. Two were in `nextDrawable`.
The slow regime persisted after the sample ended. A separate clean replay
also slowed with display sync disabled.

Detailed renderer instrumentation changed the timing distribution. Its
two runs each recorded 319,680 draws across 6,720 draw groups in the lab.
They measured 0.752 and 0.947 ms/refresh of queue pressure waits, mostly
on the oldest draw commands. Those figures apply to the instrumented
runs. The slow clean sample identifies the blocked call sites. GPU
execution time requires the command timestamps.

The opt-in `CARA_METAL_QUEUE_WAIT_TIMELINE=1` records bounded events only
when retirement waits on a pending command. It can run through
`tools/profile_native_metal.py --metal-option CARA_METAL_QUEUE_WAIT_TIMELINE=1`
without the per-draw timers or automatic stack sample. The wait CSV uses
the refresh observed before VBlank. The diagnostics report drops
separately and record unavailable GPU timestamps as `-1`. GPU command
spans may overlap. The column for the part of the wait after GPU
completion uses the [host mach time reported by Metal](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpuendtime)
and includes completion delivery and host wakeup.

Moving draw commit ahead of queue reservation was tested on the same FIFO.
It preserved the normal endpoint and moved pressure waits from 1.329 to
1.372 ms/refresh while elapsed moved from 17.345 to 17.466 ms/refresh.
Both runs used the first version of the lightweight queue timeline.
The trial was removed. Draw publication again follows the existing queue
reservation and validation path.

With the host/GPU timestamp split enabled, the stock and display-sync-off
diagnostic boots spent 0.977 and 1.033 ms/refresh in queue waits. Of those
totals, 0.305 and 0.285 ms/refresh occurred after GPU completion. Draw
tickets were the oldest producers in 1,083 of 1,169 and 1,035 of 1,059
waits, respectively. Both diagnostic boots stayed near 95% in the lab.
These samples quantify the faster regime. The slow clean boot's
completion-delivery versus GPU-execution split remains unmeasured.

The exact 128-draw group trial completed in 17.745 ms/refresh (93.92%),
with a 92.65% slowest 120-refresh window and a 22.59 ms p99. The default
64-draw group remains in use. API-validation submission-window fixtures
passed at 8, 16, and 32 slots with the queue timeline off and on, including
FIFO wrap, checked receipts, rejected writes, and unslotted-command cleanup.

The existing exact draw-publication deduplication switch measured
17.324 ms/refresh (96.20%), with a 95.47% slowest window and a 21.97 ms p99.
It passed the exact replay guards. The faster clean control measured
17.234 ms/refresh, so the switch remains experimental. The direct
FastInterp load/compare/branch remains off. The steady 100% parity target
is still open.

The paired frame-time receipts and queue-window breakdowns are in
`work/luigi-metal-cce1a67/gadd-v35-normal-comparison.json`. The slow
sample is in `gadd-v35-normal-quiet-B/slow-host-sample.txt` under that
directory. All 22 benchmark-harness tests pass.

## Captured slow boots and GPU frequency residency

A byte-identical rebuild of the queue-trace executable caught two slow
Dark Moon normal boots on the M3 Pro. All FastInterp+Metal endpoints in
this campaign match the original lab pixels and sampled guest CPU/tick
boundary. The direct FastInterp load/compare/branch path stayed off.
Measurements cover refreshes 3120–3600 at the original guest clock.

| Queue trace arm | Mean ms/refresh | Speed | Slowest 120-refresh window | Refresh p99 | Over 20 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Faster stock B | 17.421 | 95.67% | 95.23% | 22.56 ms | 25/480 |
| Slow stock C | 22.636 | 73.63% | 71.53% | 28.55 ms | 450/480 |
| Slow stock D | 24.028 | 69.36% | 68.03% | 33.77 ms | 447/480 |
| Latency-critical activity | 17.261 | 96.56% | 94.90% | 22.36 ms | 42/480 |
| AppKit fullscreen, Game Mode on | 22.402 | 74.40% | 73.15% | 23.54 ms | 480/480 |

Stock C spent 5.336 ms/refresh waiting for the oldest tracked command,
compared with 0.361 in stock B. The part overlapping that command's GPU
start/end interval rose from 0.249 to 4.431 ms/refresh. Post-GPU wait rose
from 0.087 to 0.860 ms and wait before GPU start changed from 0.024 to
0.044 ms. Emulation-thread CPU time rose from 17.059 to 17.331 ms/refresh.
Median GPU spans among waited-on 40-draw commands rose from 1.405 to
1.959 ms, and 64-draw commands rose from 1.793 to 2.181 ms. These cohorts
include only commands that blocked the host.

Five half-second system-wide IOReport residency samples collected after
the slowdown appeared in stock C spent all active GPU time in P1, the
338 MHz state, at 87.8–95.1% active residency. The samples finished before
the lab timing window. Stock D independently showed all active time at
338 MHz in its first two lab samples, with 92.0% and 93.3% active residency.
The faster latency-critical arm showed 648, 1198, 1012, and 622 MHz
active-weighted averages inside the lab. The external sampler can perturb
timing, and the brief samples cannot establish what makes the governor
hold P1. The same-binary queue traces establish that most excess host
wait overlaps GPU execution.

For the fullscreen trial, a scratch build used native AppKit fullscreen
and the documented [Game Mode bundle key](https://developer.apple.com/documentation/bundleresources/information-property-list/lssupportsgamemode).
The macOS policy log recorded Game Mode on. All four lab samples still
reported P1 at 338 MHz. The scratch fullscreen change and bundle metadata
were excluded from the runtime. The latency-critical result is within
the prior fast stock range, so its default stays off.

The profiling helper now reports each queue wait's overlap with the
oldest command's GPU span separately from the time before GPU start and
after GPU completion. The split uses [Metal's host-domain GPU timestamps](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpuendtime).
The data retain dropped counts and unattributed time for older or
incomplete captures. GPU residency sampling remains developer tooling.

The shared packed-fetch reuse and quaternion-slice shader options preserved
the exact lab endpoint. The run slowed midway through the lab, averaging
20.659 ms/refresh (80.68%) with a 75.95% slowest window and a 25.09 ms p99.
Both shader options remain experimental.

Two fresh JIT/OpenGL controls reproduced the historical OpenGL pixels
with GPU-complete timing boundaries. They averaged 17.243 and 17.172
ms/refresh (96.66% and 97.06%). Their slowest 120-refresh windows were
94.61% and 89.65%, with p99 values of 24.91 and 24.85 ms; 33 and 36
refreshes exceeded 20 ms. The immediately preceding campaign's control
was 16.667 ms/refresh with a 99.98% slowest window and a 21.08 ms p99.
The current host conditions also affected the OpenGL baseline.

The lab target remains open. The raw GPU residency samples, Game Mode
policy log, and per-run cadence and queue summaries are retained under
`work/luigi-metal-cce1a67/gadd-v36-*`. The queue split passed all 24
profiling-helper tests and was checked against the actual slow, fast,
and legacy queue traces.

## CPU submission rate and GPU utilization on M3 Pro

The Activity Monitor row supplied during the third normal boot identified
Cara Metal and showed 116.2% CPU and 49.1% GPU. Its capture time falls
between the refresh 1288 and 1398 progress reports, before the later
lab timing interval. CPU percentages aggregate time across threads.
The later device, renderer, and tiler counters are system-wide samples
from the AGX accelerator, distinct from Activity Monitor's Cara row
and from GPU-clock active residency.

| Gadd lab, native Metal | Speed | Slowest 120-refresh window | Refresh p99 | Emulation-thread CPU | GPU device utilization |
| --- | ---: | ---: | ---: | ---: | ---: |
| FastInterp, 8 slots, C | 93.61% | 92.06% | 22.12 ms | 17.00 ms/ref | 67% median |
| FastInterp, 8 slots, D | 93.88% | 92.10% | 21.96 ms | 17.41 ms/ref | 65% median |
| FastInterp, 16 slots | 95.60% | 93.18% | 22.51 ms | 16.51 ms/ref | 63% median |
| JIT diagnostic, 8 slots | 99.83% | 99.98% | 17.06 ms | 11.70 ms/ref | 67.5% median |

The 16-slot trial completed with exact FastInterp endpoint pixels and
the original guest CPU/tick signature. It lowered sampled queue waiting
to 0.104 ms/refresh from 0.343–0.963 in the four 8-slot controls. Its
tail timing did not improve. Older normal and restored tests had mixed
results with the larger ring, and it remains experimental.

The JIT diagnostic shows the same `Where to, Luigi?` dialogue and exact
sampled CPU/tick signature. Its final image differs in 3.93% of pixels by
more than the scene guard's three-channel-unit threshold, so the run
failed that guard. The comparison shows about 5 ms/refresh less host
CPU time on the same Metal backend. The differing pixels limit it to
a diagnostic comparison. Aggregate JIT process CPU
time was 15.51 ms per 16.70 ms elapsed, consistent with the user's reading
in the 90s.

The GPU policy probe records raw controller, software-state, power-target,
thermal and throttle counters with `--policy-states`. The four continuously
sampled stock runs and two delayed-sampling runs stayed out of the
sustained 338 MHz slowdown. Sampled active controller time was predominantly
`PERF` in the stock runs. Neither delayed run met the pre-lab trigger
for a controlled idle pulse, so no pulse was applied. A similar
[M3 Pro report from another emulator](https://github.com/seedds/nxnative/issues/2604)
observed the raw controller state `SE` during low-clock periods. Cara's
older slow traces lacked that channel, and attributing the slow boots
to `SE` here requires a new capture.

Packed vertices and raw indices already go to Metal. The vertex shader
expands the triangle slots and runs the translated PICA vertex program.
Metal shaders also decode sampled textures and perform tiled surface
transfers. In the earlier instrumented lab window, the exact-byte index
scan cache hit 318,833 times with 847 misses. Draw uploads cost 0.745
ms/refresh of inclusive host time, color plus depth acquisition 0.886
ms, uniform snapshotting 0.189 ms, and sampler planning 0.026 ms.
The display-transfer scope is inclusive of work such as pending draw
publication. These nested measurements are unsuitable for addition.

Apple's GPU-encoded indirect command buffers can take repetitive draw
encoding off the host once compatible draws have immutable input ranges,
valid pipelines, and resource lifetimes established. The current guest
authority resolves CPU RAM, previous GPU producers and attachment aliases
before a draw can be queued. Directly sharing mutable guest RAM would
require write ordering and lifetime guarantees even on unified memory.
Moving such a decision to a GPU pass that the CPU must immediately read
would introduce another synchronization point.
[Metal indirect command encoding](https://developer.apple.com/documentation/metal/encoding-indirect-command-buffers-on-the-gpu)
[Metal CPU/GPU synchronization](https://developer.apple.com/documentation/metal/synchronizing-cpu-and-gpu-work)

In the follow-up sampler experiment, the second disabled control
reproduced 73.00% in the lab. Sixteen telemetry samples taken after
the slowdown began show 338 MHz and 89.0–98.3% active residency.
All active controller time was `SE` and all active software-state
time was `SW_P1`. The same samples report `NO_CLTM`, `PMU_RC=NONE`,
a 100% power target, and zero total throttle-counter increments.
Additional samples immediately before the lab agree. These raw
labels identify the controller mode that coincided with the slowdown.
The trigger for entering `SE` remains unresolved. A pre-lab idle pulse
was gated out once the first eligible live inspection crossed into the
measurement window. A subsequent dedicated boot stayed fast and did not
trigger the idle pulse.

The sampler-plan memo trial reached 93.73%. Its two disabled runs were
in the slow regime, so the experiment does not establish a speed gain.
Sampler planning's 0.026 ms/refresh measured scope leaves little room for
a material gain. The test-only sampler memo was removed.

The existing grouped uniform-upload reuse option reached 96.29% and
16.13 ms of emulation-thread CPU time per refresh, versus 95.09% and
16.55 ms in the later fast control on the same experimental binary.
The p99 was 22.18 ms versus 21.89 ms. This single pair supports further
testing of copy reuse and leaves the default policy in place. The existing
uniform reuse GPU fixture passed both modes and produced byte-identical
golden output under Metal API validation. Performance and power receipts
are retained under `work/luigi-metal-cce1a67/gadd-v37-*`.

## AC power and host rendering overlap

macOS reported AC power before, during, and after all three runs, and
recognized a 67 W adapter. Both power profiles had Low Power Mode off.
The AC executable is byte-identical to the v37 battery executable.
All runs booted Dark Moon normally with FastInterp, stock Metal
settings, the same savedata and input script, and the queue trace on.
All three matched the exact FastInterp pixels and sampled guest
CPU/tick boundary at refresh 3600.

| Gadd lab | Median sampled active GPU clock | Speed | Slowest 120-refresh window | Refresh p99 |
| --- | ---: | ---: | ---: | ---: |
| AC A | 599 MHz | 94.72% | 94.08% | 22.74 ms |
| AC B | 608 MHz | 93.83% | 93.00% | 22.56 ms |
| AC C | 662 MHz | 93.18% | 92.50% | 22.44 ms |
| Four faster v37 battery runs | 604–878 MHz | 93.61–94.36% | 92.06–93.54% | 21.96–22.41 ms |

The clocks are medians of 350 ms active-weighted samples taken inside
the observed lab boundaries. Across 279 samples after the startup
sampling trigger, including 70 in the lab, the AC runs never showed a
sample at or below 370 MHz with at least 80% active residency. The
faster battery runs also stayed clear of that state. Other recent
battery boots reproduced the sustained 338 MHz `SE` regime. The present
AC runs show similar normal clocks and frame times. They leave the
effect of power source on the intermittent `SE` transition unresolved.
Receipts are in `work/luigi-metal-cce1a67/gadd-v38-ac-clock-comparison.json`.

The public Metal SDK exposes no runtime GPU clock setter for this
application. Xcode's Metal debugger can induce a performance state for
profiling. Apple's published High Power Mode support for the 2023 M3
generation lists the M3 Max MacBook Pro. The local device is an M3 Pro.
[Metal profiling](https://developer.apple.com/documentation/xcode/optimizing-gpu-performance)
[Apple power modes](https://support.apple.com/en-euro/101613)

The native PICA draw callback synchronously calls binding preparation
and `lane.Record` on the emulation thread. Committed Metal command
buffers already execute asynchronously. The core schedules guest CPUs
sequentially even in its multicore guest mode. For the host overlap
experiment, retain the guest's current timing and move ordered Metal
upload and encoding work to one worker, using immutable draw packets
and explicit ownership. Track the worker's submission progress
separately from GPU completion. The producer must account for queued
writes before resuming the guest and join for dependent guest reads,
overwrites, presentation, or lifecycle events where needed.

Use the host time when a compatible draw group is ready, the command
commit time, GPU start/end timestamps, and the committed queue depth
to classify idle intervals. An empty ready queue while the GPU is idle
is evidence of producer starvation. Committed work awaiting GPU
execution can indicate ordering, resource dependencies, or scheduling.
The existing queue-pressure trace records waits on occupied slots.
In the v37 FastInterp comparison, moving from 8 to 16 slots reduced
waiting from 0.818 to 0.104 ms/refresh, with device utilization at
63% instead of 67% and p99 at 22.51 ms instead of 22.12 ms. A deeper
queue alone has weak evidence for improving the lab's pacing.

The earlier ordered OpenGL worker improved elapsed Luigi comparisons
by 4.1–7.5% on M1. A Metal worker needs its own measurements of
enqueue-to-commit delay, useful overlap, bytes copied, and the number
and duration of required joins. Apple's command buffer API supports
parallel host encoding with explicit enqueue order, and the existing
Azahar scheduler separates worker drain and device completion.
[Prior Cara worker measurements](host-render-queue.md)
[Metal enqueue order](https://developer.apple.com/documentation/metal/mtlcommandbuffer/enqueue%28%29)
[Azahar scheduler notes](metal-upstream-research-2026-11-20.md#azahar-completion-progress-and-caras-input-cache)

## iOS release compiler comparison

The current release job builds the generic arm64 GLES3 app with FastInterp and
audits it for JIT linkage before packaging. Native Metal has a separate
developer build. Both iOS configurations use the shared Apple FastInterp
runtime policy. The direct load-to-compare/branch experiment stays off.

`tools/build.py` now accepts the FastInterp-only profile generation and use
options already supported by the native Metal builder. This lets a physical
device comparison use the same iOS build entry point as the release job.
Keep training and timing builds in separate build directories, use the
matching Xcode `llvm-profdata`, and use the matching instrumentation kind:

```sh
python3 tools/build.py ios --renderer gles3 --fastinterp-pgo-kind ir \
  --fastinterp-pgo-use /path/to/matching.profdata --build-dir work/ios-gles3-fi-pgo
python3 tools/audit_no_jit.py ios --renderer gles3 --build-dir work/ios-gles3-fi-pgo
```

The iPhoneOS 27.2 compiler accepted the existing Mac Gadd IR profile with both
profile coverage and staleness warnings treated as errors. This verifies
profile consumption on the iOS translation unit. The profile was trained on
macOS, and no iPad was available for runtime measurement. Keep PGO off in
the release build until repeatable normal-boot Gadd timings and held-out game
checks on physical iPads justify enabling it. Include frame-time p99 and the
slowest 120-refresh window.

On the M3 Pro, both native Metal executables included the same optional macOS
JIT linkage and ran FastInterp. Four normal boots used the same input replay,
savedata, pipeline cache inputs, and refresh 3120–3600 measurement window.
They requested prewarming. The stock no-JIT run logs show the supplied archive
was incompatible with that build's cache identity and no recipes were loaded.
All four matched the endpoint pixels and sampled guest CPU/tick boundary.
The JIT/OpenGL control reproduced the previous OpenGL endpoint image.

| Normal boot to Gadd's lab | Speed | Slowest 120 refreshes | Refresh p99 | Emulation-thread CPU |
| --- | ---: | ---: | ---: | ---: |
| FastInterp + Metal, stock A1 | 92.76% | 91.37% | 22.39 ms | 16.86 ms/refresh |
| FastInterp + Metal, IR PGO B1 | 65.69% | 63.13% | 35.53 ms | 17.59 ms/refresh |
| FastInterp + Metal, IR PGO B2 | 65.02% | 58.80% | 37.04 ms | 16.71 ms/refresh |
| FastInterp + Metal, stock A2 | 95.12% | 94.42% | 21.86 ms | 15.94 ms/refresh |
| FastInterp-only Metal build, stock | 96.01% | 95.84% | 22.02 ms | 15.71 ms/refresh |
| JIT + OpenGL | 99.99% | 99.58% | 20.07 ms | 10.35 ms/refresh |

Four late-lab samples in B1 found 338 MHz at 92.66–98.34% GPU active
residency with active controller time entirely in `SE`. The Mac was on AC.
The trigger for entering this state remains unresolved. The release policy
keeps PGO off. Timing and
build receipts are retained in `work/apple-build-validation-2026-11-21`.
The separately built macOS no-JIT Metal app reached the same lab endpoint
with exact tested pixels and guest state. Isolating the performance effect
of JIT linkage would require repeated, matched runs.

On that no-JIT binary, the stock/reuse/reuse/stock timing sequence tested
`CARA_METAL_REUSE_UNIFORM_UPLOADS` with FastInterp. All endpoint guards
passed on the same binary. The option reduced emulation-thread CPU time
by 0.22 ms per refresh on average. The second reuse run's slowest window
fell below both stock controls.
The option stays off by default pending physical-device and game coverage.

| Grouped uniform uploads | Speed | Slowest 120 refreshes | Refresh p99 | Emulation-thread CPU |
| --- | ---: | ---: | ---: | ---: |
| Stock A1 | 96.01% | 95.84% | 22.02 ms | 15.71 ms/refresh |
| Reuse B1 | 96.99% | 96.36% | 21.85 ms | 15.42 ms/refresh |
| Reuse B2 | 97.22% | 95.35% | 21.20 ms | 15.37 ms/refresh |
| Stock A2 | 96.65% | 96.10% | 21.74 ms | 15.52 ms/refresh |

The four other Downloads ROMs were also rechecked with the stock no-JIT
build. Cooking Mama 5 completed its 600-refresh startup capture. Driver:
Renegade, Ever Oasis, and Monster Hunter Stories stopped at the same
framebuffer, transfer, and sampled-color-alias boundaries listed above.
The new receipts are in
`work/apple-build-validation-2026-11-21/other-rom-smokes`.

## Rolling lab windows and fragment buffer bindings

The profiler now retains the slowest rolling 120-refresh window and the
slowest 10% of individual refreshes. In trace runs,
`CARA_METAL_REFRESH_CPU_TIMELINE=1` samples emulation-thread CPU time once
per measured refresh. Queue events recorded before VBlank are assigned to
the completed refresh. CPU time and queue-event time can overlap.

Across the three earlier traced AC boots, the slowest 48 refreshes waited
3.40–4.54 ms each on the oldest command. Whole-lab means were 0.79–1.07 ms.
In the new slow boot, all 12 late-lab telemetry samples were at 338 MHz,
with 90.4–95.1% active GPU residency. The worst rolling window (3289–3408)
averaged 26.75 ms wall, 20.85 ms thread CPU, and 5.94 ms in tracked queue
waits. Over the whole lab, 4.17 of the 5.84 ms average wait overlapped the
oldest command's GPU execution. A faster trace averaged
17.94 ms wall and 17.12 ms thread CPU in its worst rolling window, with
0.83 ms in tracked queue waits.

The experimental `CARA_METAL_REUSE_FRAGMENT_BUFFER_BINDINGS=1` memo is scoped to one open
encoder and retained command. It skips an identical binding, uses
Metal's offset-only setter for a different slice of the same buffer, and
invalidates slots when their bindings are cleared. GPU validation matched
37 golden pixels for every combination with grouped uniform reuse,
including buffer offset changes, cleared slots, pass closure, arena
rotation, and unretained commands. The shared draw code passed an
iPhoneOS SDK syntax check.
[Apple's buffer binding guide](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/BufferBindings.html)

The 16-slot diagnostic cut the average oldest-command wait in the slowest
decile from 4.27 to 0.58 ms. The decile's mean wall time nevertheless rose
from 21.24 to 21.69 ms. The worst rolling 120-refresh window still required
16.76 ms of thread CPU per refresh. The slowest individual refreshes had
time outside both the thread CPU and the traced oldest-command waits.

The quiet runs below booted normally with grouped uniform reuse. Every arm
matched the same binary's reference pixels and sampled guest CPU/tick
state. The matched archive runs loaded 90 recipes with 57 archive hits
and zero misses. The speed column uses the lab's completed interval,
including the final capture drain, relative to a 60 Hz target.

| Quiet run | Speed | Worst rolling 120 mean | Refresh p99 | Emulation-thread CPU |
| --- | ---: | ---: | ---: | ---: |
| Cold control | 93.07% | 18.35 ms | 22.77 ms | 17.22 ms/refresh |
| Cold fragment reuse A | 94.03% | 18.15 ms | 22.46 ms | 16.81 ms/refresh |
| Cold fragment reuse B | 94.70% | 17.75 ms | 22.16 ms | 16.72 ms/refresh |
| Warm control | 93.59% | 18.17 ms | 22.72 ms | 16.64 ms/refresh |
| Warm fragment reuse | 93.56% | 18.07 ms | 22.51 ms | 16.88 ms/refresh |
| Warm fragment reuse, 16 slots | 93.29% | 18.73 ms | 23.68 ms | 17.16 ms/refresh |

The fragment-binding experiment stays opt-in. The warm comparison showed
near-equal throughput and higher emulation-thread CPU with reuse. The
submission window retains its existing default. Profile receipts are in
`work/gadd-lab-tail-2026-11-21`.

In an instrumented warm 16-slot run, the upload arena recorded zero
blocking waits. Submission pressure was 0.023 ms per refresh and the
inclusive presentation scope was 0.505 ms per refresh. The stack sample
found synchronous depth producer validation, depth export and depth
import waits outside the oldest-command trace. Four sampled
`nextDrawable` frames of the call tree were associated with drawable
acquisition. The sample ran during the lab and perturbed its timing.
This identifies call sites for a lighter follow-up trace. It does not
assign the quiet-run tail to a particular site.

## Queued depth export from tracked producers

`CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT=1` with async checked import
can queue a certified depth export while its producer is still in flight.
Admission requires the same owning Metal queue, an earlier tracked producer,
the matching texture and generation, and coverage of the requested guest bytes.
A single exact export supplies the import buffer directly. Mixed sources retain
the ordered blit and fallback rules.

The import ticket retains the original pending writer and both GPU error
buffers, including the producer's predecessor checks. A completed import
command alone cannot certify its output. Queue retirement and guest-visible
boundaries consume the receipt. The existing CPU-backed and incompatible
source paths retain their checks.

GPU validation passed with synchronous import, async import using a completed
producer, and async import from a tracked pending producer. The gated test
verifies acquisition can proceed before the producer finishes, the guest read
still blocks until validation, and the D24S8 bytes round-trip exactly.
Independent error-buffer tests also verify that an earlier failed receipt
rejects a later successful GPU command.

The normal boots into Gadd's lab used the same save and input sequence with
the measurement covering refreshes 3121–3600. The Metal arms used matching
learned pipelines and exact Metal endpoint guards. The JIT/OpenGL control
matched the earlier OpenGL endpoint. The slowest window is the maximum
rolling 120-refresh mean.

| Normal boot | Completed speed | Worst 120 refreshes | Mean in worst window | Refresh p99 |
| --- | ---: | --- | ---: | ---: |
| JIT + OpenGL | 99.87% | 3250–3369 | 16.80 ms | 21.51 ms |
| FastInterp + Metal, control A | 94.68% | 3230–3349 | 17.94 ms | 22.77 ms |
| FastInterp + Metal, queued depth B | 94.72% | 3139–3258 | 18.21 ms | 22.04 ms |
| FastInterp + Metal, queued depth B repeat | 95.93% | 3183–3302 | 17.59 ms | 22.27 ms |
| FastInterp + Metal, control repeat | 72.44% | 3138–3257 | 23.32 ms | 28.00 ms |
| FastInterp + Metal, 16-slot control | 90.04% | 3176–3295 | 19.04 ms | 23.61 ms |
| FastInterp + Metal, 16 slots with queued depth | 99.37% | 3447–3566 | 16.91 ms | 19.36 ms |
| FastInterp + Metal, 16 slots with queued depth repeat | 99.31% | 3217–3336 | 16.88 ms | 19.04 ms |
| FastInterp + Metal, 32-slot control | 91.94% | 3364–3483 | 18.61 ms | 24.68 ms |
| FastInterp + Metal, 32 slots with queued depth | 99.97% | 3316–3435 | 16.77 ms | 17.67 ms |
| FastInterp + Metal, 32 slots with queued depth repeat | 100.26% | 3415–3534 | 16.77 ms | 17.82 ms |

The slow control's ten late-lab samples all recorded 338 MHz, with system-wide
GPU active residency at 89.8–96.9%. Active controller time was in `SE`.
The reported thermal-limit and lost-performance channels showed `NO_CLTM`
and `NONE`, the GPU power target was 100%, and the total throttle counter
delta was zero in every sample. The Mac was on AC with Low Power Mode off.
The slow control spent 16.60 ms/refresh of emulation-thread CPU time,
compared with 16.80 ms in the other control, while wall time rose by
5.41 ms/refresh. This argues against simple CPU starvation or idle downclock
throughout that interval. Active residency is not execution-unit utilization,
and these channels cannot identify the controller's trigger or rule out a
shared power or bandwidth limit.
[Apple GPU performance state guidance](https://developer.apple.com/videos/play/wwdc2021/10157/)

The BETA15-Lab article compares clock-normalized throughput and energy
efficiency on an M5 Max Steel Nomad workload. Its measurements motivate
comparing Cara timing at matched performance states. They do not identify
the M3 Pro controller transition during this emulator workload.
[BETA15-Lab measurements](https://note.com/beta15_lab/n/n7867895deff7)

Both eight-slot diagnostic arms admitted zero pending depth exports and
recorded zero CPU depth-export fallbacks. With 16 slots, the queued-depth
diagnostic admitted 348 pending exports in 480 refreshes, recorded zero
CPU depth-export fallbacks, and passed all 960 async depth receipts.
The quiet 16-slot candidate boots used 16.67–16.69 ms/ref of thread CPU
versus 17.72 ms/ref in their control. Their worst rolling windows exceed
the 60 Hz budget by 0.21–0.25 ms/ref. Adding completed-writer memoization
or raster-state reuse to this configuration left the worst rolling means
at 16.94 and 16.98 ms respectively.

The two 32-slot queued-depth runs both had a slightly lower worst-window
mean than the JIT/OpenGL reference and a lower refresh p99. Both worst windows
remain roughly 0.10 ms over the strict 60 Hz budget. The new depth setting
and wider queue stay experimental. The fresh OpenGL replay used the existing
JIT diagnostic binary. Timing, cache, endpoint and validation receipts are in
`work/gadd-depth-2026-11-21`.
