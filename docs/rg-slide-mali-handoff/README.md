# RG Slide Mali performance handoff

This handoff covers the measured Android GLES Atlas stack and shared
fragment generator changes. Re-establish the benchmark baseline when
integrating changes from main.

- [Renderer and profiling options](../rg-slide-rendering.md)
- [Single resident vertex check, layout-uniform reuse and sampled driver call costs](resident-vertex-handoff.md)
- [Cold and warm shader census and trace data](shader-census.md)
- [GLES validation, Apple parity audit and separate-stage measurements](gles-validation.md)
- [Active lit indexing and fog transfer measurements](lit-index-followup.md)
- [Android ARM64 build at 53770ae](https://github.com/dmikey/cara/actions/runs/45142152732)

The trace files record experiment markers, GLES shader events, and 120-refresh
timing windows. Run `python3 docs/rg-slide-mali-handoff/summarize-census.py docs/rg-slide-mali-handoff/census.log.txt`
to summarize the shader census. The hardware counter capture is for the FBO
probe alone. Use a locally owned ROM and isolated profiles for the comparison.

## Resume warm profiling

Use the RG Slide T820/Mali-G57 r40p0 on JIT + GLES Atlas, Performance Mode,
1×, adaptive frame skip disabled, and the guest clock fixed at 100%. Boot
normally through title and save selection into Gadd’s lab. Keep the ROM,
display mode, shader cache seed, and settings matched across runs.

At fixed clock, the earlier exclusive CPU measurements in Gadd were
18.59 ms per refresh in PICA command lists and host graphics work,
5.49 ms in guest CPU execution and memory, 2.30 ms in SVC/HLE, and
0.80 ms elsewhere. Total was 27.18 CPU-ms per refresh. The latest
warm control HUD showed 27 guest FPS at 58% SPD. The target is 100% SPD,
roughly a 16.7 ms system-refresh budget, and a smooth game-specific
output cadence. Guest FPS and system refreshes are separate measurements.

1. The unlit indexed path cut Mali position requests per 1,000 primitives
   by 28% and tiler-active cycles per input primitive by about 30%. The
   active lit Shader-topology probe also reduced position requests, with
   higher emulation-thread CPU time and no established frame-time gain.
   Keep that probe opt-in and verify color and depth if revisiting it.
2. Profile live guest index validation in `PrepareIOSVertices`. Prove
   invalidation across direct JIT writes, DMA/transfers, aliasing, and state
   restore before relying on guest-memory generations in place of byte
   comparison. Upload identity and source validity are separate.
3. Test captured draw preparation on a worker with immutable inputs.
   Preserve draw order and guest-visible completion across reads, fills,
   copies, and transfers. Compare emulation-thread CPU time as well as p99,
   worst refreshes, actual game FPS, and Mali vertex/tiler/fragment activity.
4. If investigating FBO reuse again, match GPU counter captures for both
   arms. The current probe saved about 0.16 ms per refresh in submission
   with both arms at 58% SPD. Keep the paired-FBO probe opt-in.

## Resume cold profiling

The regular GLES program path spent 104.57 s in link/status and 3.41 s
in stage compile/status on the emulation thread after save selection.
Known lit programs account for 89.18 s of that link time. The existing
single shared EGL worker builds only optional indexed variants. A pixel-
and depth-correct lit fallback for regular draws remains open. Reuse
the shared stage cache, fragment generator, and worker. Verify actual
overlap on Mali before expanding the worker count, and compare cold and
warm output and timings. The corrected opt-in GLES 3.2 separate-stage
path reached the title and lab and reduced the measured cold stalls.
Vertex-stage link accounts for most remaining stage cost. Unused TEV
operands did not affect any of the 95 matched Dark Moon shader keys.

Both prototypes and the shader census remain opt-in where applicable.
Keep Apple and Android using the same semantics in the shared renderer.


## Which results can change the renderer

Keep performance experiments in isolated benchmark builds. A change that
ships should preserve guest output and timing and select a validated path
by capability. A probe that fails to show a reproducible improvement can
be removed from the integration candidate while its measurements remain
here. Compare resident and allocation costs as well as p99, maximum
refresh time and scene progress.

A previous live MMJ comparison on the same Slide measured 59.822 system
refreshes/s and 20.719 game submissions/s with its shipped Luigi FMV and
texture-copy overrides. Turning the FMV override off raised submissions
to 33.409/s while refreshes fell to 33.699/s. With texture-copy skipping
also off, it measured 25.246 refreshes/s and 25.052 submissions/s. The
settings also changed guest hardware mode and shadow behavior, so those
rates do not establish a matched MMJ-to-Cara speed difference. Match guest
draws, timing, audio, renderer settings and visible progress before
attributing higher displayed speed to a faster renderer.
[Live MMJ measurements](https://github.com/dmikey/cara/blob/f41280d/docs/android-draw-processing.md#mmj-baseline-correction-and-live-transfer-probes)

## What to move across the CPU and GPU boundary

- **Required feedback copies.** The pinned MMJ source snapshots framebuffer
  feedback with GLES 3.2 image copy. Cara's Apple-compatible Atlas path uses
  an FBO blit for that call site. The older Slide image-copy experiment
  tested ordinary surface copies, with a 2.6% gain in its repeated Gadd
  comparison. Measure feedback separately with the existing copy trace
  before extending a fast path. Keep the snapshot and fall back for
  unmatched formats or incompatible resources.
  [MMJ feedback source](https://github.com/weihuoya/citra/blob/302b0c3bc4312bc96489c11584a97664894b343f/src/video_core/renderer_opengl/gl_rasterizer_cache.cpp#L775-L781)
  · [Slide image-copy comparison](https://github.com/dmikey/cara/blob/f41280d/docs/android-draw-processing.md#gles-32-image-copy-investigation)
- **Draw processing and submission.** PPSSPP moves ordered GLES work to
  its GL thread and joins when emulation consumes a GPU result. Cara's
  presentation context does not own PICA draw submission. For a bounded
  worker, measure time spent capturing draw state, queue-full waits and
  guest-visible dependency joins. Keep state and resource lifetimes valid
  across command lists, CPU reads, transfers, and save restore. The
  measured 18.59 CPU-ms graphics bucket still exceeds a 16.7 ms budget
  even under ideal overlap with the guest CPU.
  [PPSSPP threaded GLES release](https://www.ppsspp.org/news/release-1.6/)
- **ARM64 dispatch.** MMJ's public CPU integration uses direct page tables.
  Azahar's return-stack buffer is a concrete way to avoid some generic
  dispatch in Cara. A hit must preserve the existing halt and tick guards,
  the complete ARM/Thumb location descriptor and code invalidation rules.
  Across six recorded warm Gadd Perfetto traces, 91.4–98.4% of the
  emulation thread's scheduled CPU time was on CPU 7. The Slide reports
  CPU 7 with a 2.704 GHz maximum and CPUs 0–6 at or below 2.301 GHz.
  [Azahar ARM64 return-stack buffer](https://github.com/azahar-emu/dynarmic/commit/c326f9b02f63d3635ad360953f4b2b85b2073d78)

## Zelda executable and shader attribution

The RG Slide's USA A Link Between Worlds cartridge contains 6,455,296
uncompressed ExeFS code bytes at a guest text base of `0x00100000`.
Reading only that section produced SHA-1
`e6ab7d66ee62e879248b0660aaf38d22202e604b`, matching the pinned decompilation.
Its `funcs.json` contains 25,986 function starts. Use this address reference
locally to label guest samples. Public Dark Moon EU recompilation addresses
do not match this lab's USA title and should not be used for it. No matching
USA Stories map was identified. For those games use the local ExeFS and
loaded CRO ranges.
[Matching ALBW reference](https://github.com/7akeem0/albw-decomp-ai/blob/9b615ded7fdfa0f7ac818999d616d453a5291dc6/README.md#layout)

For CPU attribution, join same-process Simpleperf IPs and monotonic times
to valid Dynarmic host ranges before looking up guest function addresses.
For shader compilation or waits in a host library, carry the submitting
guest thread, SVC PC and GSP command-list sequence through the existing
Cara trace instead. The Zelda shader census below used host GL timing and
did not collect a same-boot guest PC map.

On the `c91c256` Android release, a normal first boot reached Zelda file
selection and started a new game. Through the sampled opening, GLES program
links occupied 24.23 s on the emulation thread and 12.14 s on the indexed
shader worker. Actual shader compile/status occupied 3.33 s and 1.73 s,
respectively. These are API scope sums, and concurrent threads can overlap.
Worst cold refreshes were 8,184.51 ms during the opener and 3,177.17 ms
later in the title animation. Those windows' p99 values were 245.22 ms
and 1,377.50 ms. A selected later compile-free window was 16.68 ms
median, 17.59 ms p99 and 17.60 ms worst. A faster guest CPU alone
cannot remove blocking host GL program links.

The same release APK reached the title at 39.10 s cold, 22.01 s with
128/128 restored binaries, and 37.10 s with separate GLES stages and an
empty Cara cache. Android's retained driver caches were left in place.
The three opening windows below had identical guest draw counts in all
arms.

| Refresh window end | Linked cold p99 / worst | Linked seeded p99 / worst | Separate stages cold p99 / worst |
| --- | ---: | ---: | ---: |
| 601 | 245.22 / 8,184.51 ms | 96.51 / 160.63 ms | 179.81 / 11,429.06 ms |
| 841 | 658.13 / 2,446.38 ms | 29.09 / 64.96 ms | 215.98 / 316.70 ms |
| 961 | 1,377.50 / 3,177.17 ms | 35.49 / 37.32 ms | 260.06 / 2,162.21 ms |

The seeded arm had zero timed compilation in those opening windows.
New in-game combinations still linked after starting a file. Both variants
reached and rendered Gulley's first dialogue with a 60 FPS / 100% SPD HUD.
Screenshots were independent live captures, so this is a visual progress
check and does not establish exact pixel equivalence. The longer worst
first-use stall excludes separate stages from default selection on this
evidence. The linked Stories control reached Lilia's opening dialogue.
It did not reach the first playable walk during this check.

## Packed vertex memory path

PICA's draw registers encode attribute formats, strides, index width and
topology. Fixed attributes can be supplied by registers, and immediate
vertices can be supplied through the command stream. The shared rasterizer
must preserve attribute defaults, primitive resets, the original first
vertex used for quaternion orientation and guest-visible memory order.
[PICA programming guide](https://www.3dbrew.org/wiki/GPU/Programming_Guide)
· [PICA lighting](https://www.3dbrew.org/wiki/GPU/Fragment_Lighting)

A warm packed draw follows these steps:

1. Match live guest indices to the prepared cache. The cache contains
   triangle topology and translated indices. A hit still compares the live
   source bytes before reusing any prepared data.
2. Walk the PICA attribute descriptors, merge and align source ranges,
   clear staging, then copy default attributes and live source data.
3. Search the upload bank for a matching snapshot with the same resource
   format and dimensions. The normal path compares staged bytes even when
   the exact guest upload data is already on the GPU. It skips matched GL
   uploads and keeps the resident data.
4. Submit the original draw. Unlit indexed draws can use Mali's native
   index reuse. In this experiment the reusable lit representation adds
   pair construction for the original triangle's reference vertex.

The earlier Slide trace recorded 8.30 MB of vertex plus index API uploads
and 735.65 MB of avoided uploads per 120 lab refreshes. That large skipped
byte count is an avoided driver transfer, not a count of host bytes
actually read. It explains how a low GL upload total can coexist with an
expensive live-source compare and CPU staging path. Prepared ranges and
decoded register plans are independent of the contents of guest RAM and
can be reused with exact register keys. For content reuse, an immutable
upload version needs validated writes across JIT stores, aliases, DMA,
service writes, fills and state restoration. Keep the live compare while
checking a write-generation witness against it. Measure the write tracking
cost as part of the same experiment.

Local ARM disassembly of the matching Zelda code found two functions that
put IPC header `0x000C0000` into the thread command buffer before
`SendSyncRequest`. Their SVC instruction addresses are `0x003DB6BC` and
`0x003E3DEC`. They are candidate guest markers for
`GSP::TriggerCmdReqQueue`. An actual service handle and monotonic capture
are still needed to associate one with a measured shader stall.

### Padding-only clear trial

The packed staging fixture passed 4,323 byte-exact cases under
ASan/UBSan. A fixed-clock Dark Moon A/B/A on `c91c256` used the same
197-entry seeded shader archive and normal boot in each arm. Replacing
the full staging clear with a padding-only clear took mean vertex prep
from 1.83 and 1.85 ms per warm Gadd refresh in the two controls to
1.74 ms. Scene medians stayed around 29 ms and p99 ranges overlapped.

| Warm Gadd, four windows per arm | Window p99 range | Worst refresh |
| --- | ---: | ---: |
| Full clear, first | 34.35–50.32 ms | 70.44 ms |
| Padding-only clear | 34.46–51.63 ms | 63.68 ms |
| Full clear, repeat | 34.10–50.00 ms | 73.70 ms |

Draw totals vary across the live windows. The small prep reduction does
not establish a repeatable end-to-end improvement and no default changed.
The original APK and checked user profile files were restored.

The existing phase trace now emits a `CaraPerf: staging` row for each
120-refresh window. It separates guest-index comparison spans, explicit
vertex and constant copies, vector-growth zero initialization, explicit
clearing, leased snapshot bytes, and required render-target feedback
copy count, size and wall time. Compare spans are the requested byte
length, even if an unequal `memcmp` returns early. Feedback is an API
footprint and wall scope. Neither value is a hardware DRAM counter.
[Android profiling build](https://github.com/dmikey/cara/actions/runs/45224342082)

The `f202e4e` APK reached warm Gadd from a normal boot and emitted the
new counters. Mean work across four 120-refresh windows was:

| Work per refresh | Measured amount |
| --- | ---: |
| Guest index compare span requested | 0.64 MB |
| Guest vertex copies into staging | 4.90 MB |
| Default attribute copies | 0.09 MB |
| Vector growth zero initialization | 2.87 MB |
| Explicit staging clear | 5.43 MB |
| Driver upload payload avoided by the resident cache | 6.13 MB |
| Vertex and index bytes actually uploaded through GLES | 0.052 MB |
| Required render-target feedback copies | 0 |

These are software spans and explicit copy/write requests. They do not
measure physical DRAM traffic. This capture locates substantial duplicate
work before the GPU upload-cache lookup. A production change needs to
test the live guest source ranges directly against the retained snapshot,
then use the immutable resident handle on a hit. That keeps GPU vertex
decode and rendering on the GPU without first rebuilding the same
snapshot on the host. Use already-owned bytes on misses so a future GL
worker never borrows mutable guest RAM. Source validation, retained GL
resource identity and GPU completion are distinct concerns.

The Slide advertises GLES base-vertex drawing, coherent framebuffer
fetch and `GL_EXT_buffer_storage`. It lacks the queried multidraw
extensions and `GL_KHR_parallel_shader_compile`. Keep host scheduling
and shader compilation tests tied to these driver capabilities.

A seeded Stories control on the same profiler build restored 74 of 74
program binaries and reached the opening movie at 30 FPS and 100% SPD.
It measured one 512 KiB feedback snapshot at 0.71 ms during startup and
none during the sampled movie. This is opening coverage. The first
playable walk remains outside this comparison.

### Comparing before staging

The baseline `f202e4e` and isolated `84c8ae7` test APKs both restored
197 binaries from the same cache and booted normally to Gadd. The test
enabled the existing packed snapshot lease and self-equality check.
The test retained an exact source comparison and re-used an owned CPU
snapshot when it matched. The same window ends, 2641 through 3001, were
used in each arm. Guest draw counts differed by about 0.2% on average.

| Warm Gadd, four 120-refresh windows | Baseline | Snapshot before staging |
| --- | ---: | ---: |
| CPU staging writes per refresh | 13.29 MB | 5.63 MB |
| Driver upload bytes per refresh | 51.94 KB | 51.75 KB |
| Vertex preparation per refresh | 1.87 ms | 3.36 ms |
| PICA and graphics thread CPU per refresh | 19.10 ms | 19.71 ms |
| Window median range | 28.95–29.28 ms | 28.25–28.72 ms |
| Window p99 range | 35.85–52.16 ms | 33.14–50.05 ms |
| Worst refresh | 61.75 ms | 59.57 ms |

The byte reduction is real. CPU staging writes above sum the guest vertex
copy, default attribute copy, vector initialization and explicit clear.
The candidate spends more CPU in preparation searching and comparing valid
payload candidates. The small wall-time difference in this single live
pair does not establish a throughput win and the test branch stays isolated.
A faster candidate should do a bounded exact check on a known matching
resource first, and pass that identity into submission so it avoids a
second search. Sample candidate selection and payload comparison separately.
The original APK and checked profile files match after restoration.
ASan/UBSan passed the lease integration, pool observer, and guest index
range fixtures.

[Exact snapshot test APK](https://github.com/dmikey/cara/actions/runs/45228289864)
