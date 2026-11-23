# Apple CPU and Metal upstream research, November 20, 2026

This is source research, not a new performance result. Public upstream files and
GitHub commit metadata were inspected without cloning, building or executing the
projects. No upstream implementation or game code was imported. The review was
made on November 20 in America/Chicago; upstream UTC timestamps can be November
21. The revisions below freeze this review rather than referring to a moving
branch.

| Project | Reviewed revision | Relevant source |
| --- | --- | --- |
| Azahar | `6f293a24daa292bd02b4ffee1a2adb5435e1ae00` | [FastInterp cache](https://github.com/azahar-emu/azahar/blob/6f293a24daa292bd02b4ffee1a2adb5435e1ae00/src/core/arm/fastinterp/fastinterp_cache.h), [Vulkan scheduler](https://github.com/azahar-emu/azahar/blob/6f293a24daa292bd02b4ffee1a2adb5435e1ae00/src/video_core/renderer_vulkan/vk_scheduler.cpp) |
| Panda3DS | `b18e6586a56b4d35ac0953be6359beed93586dfe` | [Metal renderer](https://github.com/wheremyfoodat/Panda3DS/blob/b18e6586a56b4d35ac0953be6359beed93586dfe/src/core/renderer_mtl/renderer_mtl.cpp), [upload buffer](https://github.com/wheremyfoodat/Panda3DS/blob/b18e6586a56b4d35ac0953be6359beed93586dfe/include/renderer_mtl/mtl_vertex_buffer_cache.hpp) |
| melonDS | `361ffa9501b631d53d7150fe57dec40a8341f664` | [ARM execution](https://github.com/melonDS-emu/melonDS/blob/361ffa9501b631d53d7150fe57dec40a8341f664/src/ARM.cpp#L672-L674), [interpreter table declaration](https://github.com/melonDS-emu/melonDS/blob/361ffa9501b631d53d7150fe57dec40a8341f664/src/ARMInterpreter.h#L31) |
| Dolphin | `7dac5ec790496bb0b10944bcbf7eade65aad671d` | [cached interpreter](https://github.com/dolphin-emu/dolphin/blob/7dac5ec790496bb0b10944bcbf7eade65aad671d/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreter.cpp) |
| PPSSPP | `77106069bf68fd80d4a7a5a519ae9d06b5677c4b` | [deferred draws](https://github.com/hrydgard/ppsspp/blob/77106069bf68fd80d4a7a5a519ae9d06b5677c4b/GPU/Common/DrawEngineCommon.cpp#L1364-L1410), [Metal uniform binding](https://github.com/hrydgard/ppsspp/blob/77106069bf68fd80d4a7a5a519ae9d06b5677c4b/GPU/Metal/DrawEngineMetal.mm#L606-L617) |

## First candidate: retain uniform uploads across pass changes within one lease

PPSSPP's Metal `BindResources` updates base and light uniform allocations when
their dirty bits change, when no buffer exists, or when the render-manager frame
changes. The existing buffer/offset is otherwise rebound. This gives a concrete
model for separating uniform-content lifetime from encoder lifetime, rather than
copying every block for each draw. Its frame number belongs to its own allocator;
it is not a safe lifetime token for Cara.

Cara's narrow transferable candidate is a per-command uniform cache outside
`OpenDrawPass`, retaining exact bitwise comparisons and the current arena lease.
Closing an encoder need not discard immutable uploaded bytes. Clear the cache on
lease end, failure and cancellation. A buffer retained by Metal is still mutable:
cross-command borrowing is unsafe if the arena can overwrite its page after the
original command finishes while a later reader remains active. That broader
design needs reader leases/completion tracking or independent immutable buffers.
See [upload arena contract](../emulator/src/video_core/renderer_metal/pica_metal_upload.h)
and [current draw upload/cache](../emulator/src/video_core/renderer_metal/pica_metal_draw.mm).

Measure copied common/light/VS bytes, exact reuse hits, arena waits, pass starts
and total completed work. Also measure
[`CapturePicaMetalUniforms`](../emulator/src/video_core/renderer_metal/pica_metal_native_uniforms.cpp):
GPU allocation reuse alone does not eliminate host conversion of every light and
VS uniform block. This extends the [proven GLES uniform-reuse mechanism](hardware-investigation.md#uniform-reuse-and-interpreter-code-layout-2026-11-12),
without assuming PPSSPP's performance transfers to Cara.

## Second candidate: keep host recording, submission and device completion distinct

Azahar's scheduler explicitly separates `Flush` (send work without waiting),
`WaitWorker` (drain worker execution) and `Finish` (submit and wait for device
completion). Its worker recycles executed command chunks. These are useful
ownership/completion boundaries, not authorization to move mutable PICA parsing
or guest-memory access onto another thread. See the pinned
[scheduler implementation](https://github.com/azahar-emu/azahar/blob/6f293a24daa292bd02b4ffee1a2adb5435e1ae00/src/video_core/renderer_vulkan/vk_scheduler.cpp#L62-L90).

Cara already has an ordered Metal submission queue, completion tickets and
authority intervals. The useful next step is attribution of why each group
publishes and why the producer waits: arena exhaustion, submission capacity,
attachment replacement, sampled-resource dependency, CPU visibility or final
drain. Reuse existing counters in
[the draw lane](../emulator/src/video_core/renderer_metal/pica_metal_native_draw_lane.mm)
and [submission queue](../emulator/src/video_core/renderer_metal/pica_metal_submission_queue.mm).
Avoid introducing a second worker queue until retained owned packets and an
actual overlap interval justify it. The [existing worker research](host-render-queue.md)
already covers snapshots, partial writes, save/load, epochs and main-thread
callback deadlocks.

Panda3DS does have a native Metal backend. Its 128 MiB shared vertex buffer
appends CPU copies and resets at `endFrame`; overflow creates additional buffers.
Crucially, its
[`commitCommandBuffer`](https://github.com/wheremyfoodat/Panda3DS/blob/b18e6586a56b4d35ac0953be6359beed93586dfe/include/renderer_mtl/renderer_mtl.hpp#L121-L132)
waits for completion, and presentation then calls `endFrame`. Importing only the
reset strategy into Cara's asynchronous arena would remove its lifetime guard.
Its draw path also selects inline vertex data for payloads below 4 KiB and a
buffer otherwise; this is an API transport example, not evidence for changing
Cara's packed PICA/quaternion pipeline or adopting its threshold.

## Third candidate: bounded preparation and decoding, driven by current samples

PPSSPP batches compatible primitive/index requests and flushes when topology,
draw-count, decoded-vertex or expanded-vertex limits would be exceeded. Its
[deferred-record structures](https://github.com/hrydgard/ppsspp/blob/77106069bf68fd80d4a7a5a519ae9d06b5677c4b/GPU/Common/DrawEngineCommon.h#L380-L418)
bound vertices and index requests separately. Transfer the admission discipline,
not its capacities or pointer ownership: Cara's deferred draws must keep owned
inputs, exact state and every guest draw. PICA triangle-first quaternion
orientation, primitive topology, lighting and guest-visible alias writes are
additional constraints. Cara already batches native Metal vertex work; inspect
its actual group boundaries before adding another batching layer.

melonDS dispatches Thumb instructions through a 1,024-entry table selected by the
upper ten bits (`CurInstr >> 6`). This supplies a concrete bounded classifier
example. Cara already predecodes instructions and has a validated tick-bucket
classifier; a new table would primarily affect cold decoding, not remove hot
FastInterp handler dispatch. Any candidate needs exhaustive Thumb16 classification
against the original decoder, preserving overlapping decode priority, ARM/Thumb
mode, invalid instructions and canonical tick functions. Count decode CPU and
replacement causes first. The reviewed melonDS files are GPL-3.0-or-later;
do not confuse this project with the separately reviewed MPL-licensed mGBA table.

Dolphin's cached interpreter emits precompiled callbacks plus operand records and
an analyzer-selected idle callback. That callback only calls `CoreTiming::Idle`
when the actual next PC returns to the recognized idle start:
[implementation](https://github.com/dolphin-emu/dolphin/blob/7dac5ec790496bb0b10944bcbf7eade65aad671d/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreter.cpp#L220-L225),
[admission](https://github.com/dolphin-emu/dolphin/blob/7dac5ec790496bb0b10944bcbf7eade65aad671d/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreter.cpp#L398-L418).
This supports strict analysis before scheduler-aware idle recognition. It does
not justify skipping Stories' hot copy/filter/pacing loops: the retained Cara
census contains stores and counters, and prior research found no proven pure idle
loop. Keep original instructions, budgets and event boundaries for ordinary
fusion experiments. See [interpreter prior art](performance-prior-art.md#cpu-interpreter-prior-art-and-bounded-candidates).

## Apple techniques across M1 and newer devices

The following five Apple references constrain practical experiments. Select by
reported device/OS capabilities and measured workload, not a chip-name whitelist.
Retain the current path on unsupported devices and an explicit comparison
override. A newer API or larger pool is not itself an optimization.

| Priority | Primary reference | Concrete application and acceptance boundary |
| --- | --- | --- |
| First: distinguish CPU starvation, latency and GPU saturation | [Optimizing GPU performance](https://developer.apple.com/documentation/xcode/optimizing-gpu-performance) | Inspect concurrent vertex/fragment/compute timelines, occupancy, limiters and per-line shader samples. Serial replay is useful for isolated counters but changes overlap and is not runtime speed. Low occupancy alone does not prove arithmetic shortage. Match the capture's passes to CPU binding, arena-wait and queue-wait counters before changing shaders or threadgroup sizes. |
| First: preserve tile contents while reducing pass boundaries | [Metal render command encoder](https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Render-Ctx/Render-Ctx.html) | Load/store actions operate at pass boundaries. Cara must preserve color/depth/stencil that later guest draws, aliases or CPU reads consume. Prefer eliminating an unnecessary encoder split with the same ordering and attachments before considering transient attachments. Never substitute `DontCare` for live guest output merely to reduce bandwidth. |
| Second: reduce repeated CPU resource binding | [Improving CPU performance using argument buffers](https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers) | Prototype immutable resource bundles only if `encode_state_and_draw` is material after source/pipeline lookup improvements. Argument buffers require explicit residency and capability/tier-aware encoding. Track writable-resource use and all backing lifetimes; keep direct resource bindings as fallback. Avoid rebuilding a large bundle every draw when most members change. |
| Later: reuse command descriptions where state really repeats | [Indirect command encoding](https://developer.apple.com/documentation/metal/indirect-command-encoding) | ICBs can contain reusable CPU/GPU-encoded draws. First establish compatible pipeline/resource/state runs and CPU encoding cost; Cara's changing uniforms, dependencies and effects remain part of each original draw. Do not turn this into GPU visibility culling or dropped PICA draws. Keep direct draws when device/OS/pipeline capabilities or small workloads make ICB construction inappropriate. |
| Startup only: retain validated pipeline compilation work | [Discover compilation workflows in Metal, WWDC21](https://developer.apple.com/videos/play/wwdc2021/10229/) | Binary archives retain compiled pipelines/functions and support later launches. Cara already has archive and MSL-cache machinery: measure archive hit/miss, library creation and pipeline creation before adding another cache. Preserve full shader/pipeline/compiler identity, transactional fallback and OS/device compatibility. This does not address warm per-draw lookup scans or arena waits. |

For SIMD/occupancy experiments, use the actual pipeline execution width and
thread/memory limits instead of hardcoded SIMD widths or an M3-only policy.
Profile register pressure, inactive lanes and memory stalls on both M1 and a
newer target. More simultaneous threads can trade away registers or local storage;
a larger threadgroup is not automatically faster. Preserve float precision,
special values and PICA quaternion results. An exact GPU fixture establishes
correctness, while the separate clean matched replay decides whether the added
compute or submission work is useful.

Bound memory by bytes as well as entry count. Shared Apple memory does not remove
CPU copies, resource hazards or lifetime fences. Report retained CPU shadows,
Metal allocations, in-flight pages and high-water memory separately. Prefer the
smallest validated capacity that avoids the observed bottleneck, retaining a
lower-memory fallback on iPad; do not copy desktop budgets into every device.

## Interpreter science: use static specialization where the current profile pays

Ertl and Gregg's [Implementation Issues for Superinstructions in Gforth (2003)](https://www.complang.tuwien.ac.at/anton/euroforth/ef03/ertl-gregg03.pdf)
distinguishes static combinations from dynamically assembled machine code and
discusses the costs to compilation, tooling and interpretation. The transferable
no-JIT mechanism is an authored precompiled combined handler selected from decoded
data. Preserve original source slots, instruction tick charges, exception/MMIO
ordering, block invalidation and scheduling budget boundaries. Never concatenate
or emit executable machine code for this target. Cara already has AL-entry,
operand, pair and tick-bucket experiments; this paper is not a reason to repeat
rejected dispatch-tail or relative-dispatch variants without a new profile.

Shi, Gregg, Beatty and Ertl's [Virtual Machine Showdown: Stack Versus Registers,
VEE 2005](https://www.usenix.org/events/vee05/full_papers/p153-yunhe.pdf) studies the
trade between fewer interpreter operations, larger decoded code and additional
operand loads. ARM is already a register ISA, so its Java VM results cannot be
applied as a predicted ARM-emulation gain. The useful question is whether selected
decoded operands or an authored pair remove repeated host loads/dispatches enough
to justify cache footprint and decode cost. Use real dispatch/decode self samples
and exact disassembly, not static guest opcode frequencies, to choose candidates.

Build-time PGO is a separate bounded experiment: collect training with the same
source, target and real gameplay, freeze the profile and evaluate held-out scenes
with matching compiler/UUID/binary receipts. It may change branch/layout choices;
it cannot authorize altered guest instruction accounting. Cara already exposes
[PGO build controls](fastinterp.md) and has retained negative compiler-layout
experiments. Start with the existing native Metal index-scan memo/specialization
and production-policy parity checks before changing interpreter architecture.
Only the measured result can decide whether any of these choices should become a
capability-selected default.

## Azahar update audit and command-stream port boundary

FastInterp [PR #2376](https://github.com/azahar-emu/azahar/pull/2376) merged September
16, 2026 as `98932aeeb7d81ba08715a3e6eff57d47c2d1a65d`. The current path-specific
GitHub history inspected here contains that initial implementation and
`0ab6399ca468c8bdd87f6a7f9c77a494c2435a8d`, the October license-reference update.
This bounded path audit found no additional FastInterp implementation commit to
import after Cara's [recorded pin](fastinterp-upstream.json)
`6b4e7f2e694514fdf2f72dc20368609b7cd4912e`. It is not a claim that changes elsewhere
in Azahar cannot affect interpretation.

The existing command-list optimization
[PR #2318](https://github.com/azahar-emu/azahar/pull/2318) uses register metadata,
batched writes and SIMD comparison. Its author's reported 0.5–1 ms saving is an
upstream measurement, not Cara evidence. Cara has already documented the narrow
port: repeated shader program/swizzle streams first, retaining scalar header,
raw special-register payloads, mask-zero effects, valid-prefix handling,
VS-to-GS mirroring, command positions and ordered draw/IRQ/jump behavior. See
[the source comparison and differential gates](stories-draw-submission-2026-11-13.md#upstream-code-research-and-next-isolated-port)
and [packet census](pica-shader-packet-census.md). Do not restart this investigation
from a generic batching proposal or combine it with rejected unchanged-write
suppression.

Azahar's project license changed to GPLv3+ in
[PR #2626](https://github.com/azahar-emu/azahar/pull/2626), while the reviewed
FastInterp and scheduler files explicitly retain GPLv2-or-later notices. Record
the exact file revision and applicable notices for any future imported code;
do not relabel all current Azahar source using Cara's historical pin. PPSSPP and
the reviewed Dolphin file carry GPLv2-or-later notices. Panda3DS ships a
[GPLv3 license text](https://github.com/wheremyfoodat/Panda3DS/blob/b18e6586a56b4d35ac0953be6359beed93586dfe/LICENSE.md).
These are research provenance observations; this document introduces no copied
implementation and changes no bundled dependency licensing.

## Use the existing analysis chain before building new probes

1. Use [`profile_native_metal.py`](../tools/profile_native_metal.py) for isolated
   runs, binary/build provenance, fixed guest clock and explicit flags. Use
   instrumented passes to find costs and separate quiet runs for speed claims.
   Preserve endpoint pixels, guest boundaries and matched input/workload evidence.
2. Use [`profile_cpu_stacks.py`](../tools/profile_cpu_stacks.py) with exact
   executable UUID/address identity. For physical iOS,
   [`sample_device_stacks.py`](../tools/sample_device_stacks.py) supplies bounded
   LLDB stop/resume snapshots; those are diagnostic stacks, not CPU-time weights.
3. Use [`map_guest_hot_ranges.py`](../tools/map_guest_hot_ranges.py) to verify
   captured block bytes against private mapped images, then
   [`summarize_guest_pc_cpu.py`](../tools/summarize_guest_pc_cpu.py) for actual
   bounded CPU attribution. A resident-block census omits evicted blocks and is
   not a time profile. Follow the [guest-PC measurement contract](guest-pc-cpu-summary.md).
4. Use [`analyze_guest_gpu_trace.py`](../tools/analyze_guest_gpu_trace.py) to join
   submission, IRQ, wait/wake and VBlank observations. Reject truncated or
   incomplete captures; equal recorded state/addresses do not establish equal
   buffers or redundant draws. Follow [trace semantics](guest-gpu-trace.md).
5. Existing [Capstone inspection credits](inspection-tool-attributions.md) and
   [offline mapping rules](guest-hot-range-mapping.md) already cover private
   ARM/Thumb disassembly. Capstone's official
   [Python API](https://www.capstone-engine.org/lang_python.html) includes a lighter
   disassembly interface for basic instruction data. Ghidra's official
   [project](https://github.com/NationalSecurityAgency/ghidra) supplies disassembly,
   decompilation and scripting; it can help inspect callers, CFGs and data types
   locally. No maintained Cara Ghidra pipeline was found in this review, and no
   Ghidra installation or game-image analysis was performed. Static decompiler
   names, ARM/Thumb boundaries and inferred function extents need byte-identity
   and runtime coverage checks before attributing cost or proposing fusion.

Keep ROMs, decoded images, shaders, checkpoints and raw disassemblies private.
None were uploaded for this research. Preserve 100% guest CPU, audio, original
guest timing and complete rendering throughout acceptance; neither a native
Metal backend nor another emulator's benchmark establishes Cara performance.

## Azahar stereo skip and shader compilation

[Azahar's right-eye change](https://github.com/azahar-emu/azahar/commit/758ded7)
classifies a second top-screen command list using the guessed viewport and
render address, then can suppress both the list and its display transfer. This
changes guest graphics work, can flicker in some titles, and needs per-game
evidence. Cara's native Metal LCD code already reads the left-eye scanout
addresses. Monoscopic presentation alone does not establish whether the guest
still generates second-eye rendering work. The Gadd baseline keeps every
admitted command list and transfer.

A current Cara desktop JIT/OpenGL diagnostic from normal boot, with guest 3D
intensity at zero, reports 480 top-screen swaps and zero distinct nonzero
left/right address pairs over refreshes 3120–3600. The same Gadd reference image
was reproduced. This counts swaps and does not measure individual eye draws or
the effect of the Azahar heuristic. Receipt:
`work/luigi-metal-cce1a67/gadd-lastmile-current-gl-stereo-diagnostic/stereo-result.json`.

[Azahar's configuration](https://github.com/azahar-emu/azahar/blob/master/src/android/app/src/main/jni/default_ini.h)
exposes Vulkan shader compilation on worker threads. For Cara's native Metal
path, [Apple's asynchronous pipeline creation](https://developer.apple.com/documentation/metal/pipeline-state-creation?language=objc)
is relevant to cold-cache compilation. In the measured Gadd boot with learned
pipelines, the v32 control attempted 90 preloads and recorded 57 archive hits,
zero archive misses and zero draw-time misses. Moving compilation can improve
first-use latency in other runs. These receipts do not identify compilation
as a bottleneck in the warmed lab.

[Azahar's GSP timing rework](https://github.com/azahar-emu/azahar/pull/2095) is
another Dark Moon lead, mapped separately in
[Cara's GPU completion prototype](gpu-completion-prototype.md). Guest interrupt
timing must be evaluated independently of host Metal queue pressure.

## Azahar completion progress and Cara's input cache

Azahar records Vulkan command chunks on a persistent worker, recycles them
after execution, and assigns each submission a monotonically increasing
completion value. The GPU wait tests cached progress, refreshes the timeline
semaphore, and then blocks if the target remains incomplete. Its fence fallback
uses a completion thread. Worker drain and device completion have distinct
entry points. [Scheduler](https://github.com/azahar-emu/azahar/blob/6f293a24daa292bd02b4ffee1a2adb5435e1ae00/src/video_core/renderer_vulkan/vk_scheduler.cpp)
and [completion implementation](https://github.com/azahar-emu/azahar/blob/6f293a24daa292bd02b4ffee1a2adb5435e1ae00/src/video_core/renderer_vulkan/vk_master_semaphore.cpp).

Cara's measured queue pressure is already behind committed draw commands.
The v33 experiment moves the nonblocking poll on cached color, depth and
exact RAM snapshot hits to the existing draw recording boundary. Imports,
submissions, guest reads and LCD presentation retain their existing guards.
This tests how much duplicated host polling costs before taking on the
ownership changes of a worker or a new completion channel. Guest interrupts
follow their existing path throughout the comparison.
