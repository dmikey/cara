# Ordered host GLES worker proposal

Source review, November 13, 2026. No worker is implemented and no gain has been
measured. This expands the larger queue proposal in
[apple-parallelism.md](apple-parallelism.md), preserving its smaller-worker
analysis and existing measurements.

An ordered backend worker could overlap host renderer execution with guest CPU
execution while preserving the existing guest event schedule. PICA parsing,
register changes, primitive semantics and guest IRQ/event delivery stay on the
emulation thread at their existing virtual ticks. The worker executes host
rendering in sequence. A dependency blocks host execution until its result is
available; that wait does not itself advance virtual time or reschedule a guest
completion event. This is a host implementation change, not a new GPU timing
model, draw omission, or backend replacement.

The observed roughly 10 ms guest execution and 9 ms graphics work are currently
serialized; draw submission itself includes substantial host CPU work. These
figures identify an overlap opportunity, not achievable parallel throughput.
Snapshotting, remaining frontend work, CPU/GPU dependencies, bandwidth, scheduling
and queue stalls can erase the benefit. Current windows with no readback samples
do not establish that a queued implementation would never require fences.

## Ownership

| Owner | Exclusive responsibilities |
| --- | --- |
| Emulation thread | PICA frontend/register state and primitive behavior; guest timing/IRQs; guest RAM and page tables; pending-range coherence ledger; sequence/epoch assignment; capture and CPU-visible publication |
| GLES worker | EAGL context; GL shadow state; shader/program objects; texture/LUT/upload banks; host surfaces and aliasing; GL transfers, readback execution and presentation |
| Immutable packet store | Owned input bytes and versioned metadata, retained until every consuming packet retires; no borrowed mutable PICA or guest-memory pointers |

Today `Pica::g_state`, `OpenGLState::cur_state`, raster state validity and surface
cache containers are mutable globals or owner-local data without this protocol.
`IOSWindow::MakeCurrent`, `SwapBuffers` and destruction in `ios/main.mm` use the
same EAGL context on the emulation owner. They must move to the backend owner or
use ordered lifecycle/presentation requests. Context access cannot be concurrent.
The optional existing mailbox coordinates presentation and may discard presents;
it does not authorize discarding guest draw packets.

## Packets and metadata store

Each job needs a monotonic sequence, session epoch, operation kind, and resource
read/write intervals. Draw packets must own packed inputs and indices or software
`HardwareVertex` output; framebuffer/texture descriptors; viewport/scissor and
blend/depth/stencil state; and references to immutable shader, uniform, LUT,
default-attribute and layout versions. A `MemoryRef` keeps storage alive but does
not freeze its contents. Likewise a guest command-list pointer is not a snapshot.

Store immutable shader/configuration blocks once per changed version and reuse
references; copying the entire PICA state and all textures for every draw could
be prohibitive. Account for staging bytes, retained versions and queued upload
bytes under an explicit cap. A packet pins resources until ordered retirement;
GL-name reuse and deferred deletion must not invalidate an outstanding reference.

CPU-origin textures, vertices and indices require either owned capture bytes or
an enforceable read-version protocol. Backend texture validation currently reads
live guest memory and chooses surfaces based on cache state; deferring it without
capturing the right memory version would render later CPU writes into an earlier
draw. GPU-origin images should stay backend-owned through ordered copies,
reinterpretations and render-to-texture dependencies whenever CPU bytes are not
needed. Accelerated-path rejection and shader compilation fallback must also be
resolved from captured state; worker code cannot fall back by consulting a later
`Pica::g_state` or current guest buffers.

## Coherence proxy is the prerequisite

`MemorySystem::RasterizerMarkRegionCached` changes page attributes and direct
pointers across guest page tables. FastInterp uses these direct pointers for RAM
access. A render worker must never mutate these tables concurrently. The frontend
must mark a pending GPU destination before it resumes guest execution, with a
separate pending/resident ownership protocol so backend eviction cannot expose a
page that still has queued work. Backend completion messages are published by the
emulation owner at defined synchronization points.

The proxy must cover cached-page access, direct physical-pointer consumers,
service/DMA copies, map/unmap, transfer fallback and save-state paths. Watching
only ordinary guest load/store callbacks leaves bypasses. Source bytes not covered
by a write barrier, notably unregistered vertex/index arrays, should be copied
when captured in an initial correct implementation.

## Dependency and wait map

| Operation | Required behavior |
| --- | --- |
| CPU/service read overlaps pending GPU writes | Drain relevant ordered work, finish necessary device execution and materialize exact bytes before returning |
| CPU partial write overlaps GPU-owned dirty data | Preserve every untouched byte. Initially drain and use existing flush/invalidate behavior; do not replace whole surfaces with stale RAM |
| CPU full overwrite | Can avoid old-image readback only when old pending readers have immutable versions and no untouched byte needs preservation; otherwise fence |
| GPU render followed by texture/alias/reinterpret/copy use | Retain FIFO dependency on backend resource versions; normally no CPU readback or guest-thread wait |
| CPU packing/fallback needs GPU-produced vertex/index/texture bytes | Same visibility fence as a CPU read, before creating its immutable packet |
| Display transfer, texture copy or fill | Preserve ordered memory effects and CPU-fallback decision. Current boolean accelerator return may require synchronous RPC until deterministic capture/preflight exists |
| Resource eviction or upload-bank reuse | Retire references and observe backend/device lifetime fences before reuse; a submitted job is not necessarily finished on the GPU |
| Map/unmap or page-table replacement | Drain affected versions, publish bytes and update mappings on the guest owner; retain backing memory until old users retire |
| Screenshot/presentation | Execute after its required rendering sequence. A display consumer can wait without forcing the guest to wait except where bounded buffering/reuse requires it |
| Save/load/reset/shutdown | Ordered drain, CPU-visible publication and epoch transition; reject stale completion messages and destroy GL objects on their owner |

Distinguish three states: captured, executed by the host worker, and completed on
the device. A worker-queue barrier alone is insufficient for CPU readback.
Conversely, inserting `glFinish` after every job would unnecessarily serialize
ordinary GPU dependencies. A conservative initial write fence is correct but may
make overlap ineffective; measure its frequency before replacing it with resource
versioning or copy-on-write.

Partial writes and save states are correctness gates. Saving must resolve pending
GPU writes into authoritative RAM and preserve the existing serialized state
contract; loading must drain/discard old-epoch work before restoring RAM and
rebuilding host resources. Neither a saved raw pointer nor a delayed old-epoch
upload may survive the transition. Flush/invalidate must retain current alias,
format-reinterpretation and untouched-region semantics.

## Verified primary prior art

Sources were inspected on November 13, 2026; these links follow their current
upstream branches, not a frozen imported dependency.

- [PPSSPP GLRenderManager](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLRenderManager.cpp)
  is the closest GLES model: producer steps become owned SUBMIT/PRESENT tasks,
  resource dependencies are recorded, and result-sensitive readbacks use
  `FlushSync`. Its host queue synchronization is distinct from blanket GPU finish.
- [Azahar Vulkan scheduler](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_vulkan/vk_scheduler.cpp)
  separates nonblocking `Flush`, CPU-worker `WaitWorker`, and device `Finish` /
  timeline waits. The transferable mechanism is explicit ownership and completion
  levels; Vulkan APIs and virtual GPU timing policies are not the proposal.
- [Dolphin asynchronous requests](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/AsyncRequests.cpp)
  flush the vertex pipeline before executing queued result-sensitive requests;
  its [FIFO](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/Fifo.cpp)
  supplies explicit producer/GPU synchronization. This is architectural evidence,
  not a drop-in Cara queue or permission to adopt its timing policy.
- [Mikage's technical report](https://mikage.app/year-in-review-2022/)
  describes resource ownership and delaying memory materialization until needed.
  It supports the ownership model, without establishing Stories performance.

## Reviewable candidate and acceptance gates

Queuing only `glDraw*` is not a useful first implementation: the same context
performs surrounding state, uploads, cache operations and transfers. Context
migration or a round trip per draw defeats overlap. A low-level GL recorder also
needs owned upload data, virtual resource IDs, map/query/compile results and
ordered deletion; it is already a substantive backend API split.

First collect a shadow dependency trace with no changed execution or timing:
logical sequences, read/write ranges, source-version mutations, snapshot bytes,
resource lifetimes and each hypothetical visibility fence. Include physical
pointer and service paths, not just present readbacks. Replay this trace through
bounded queue policies to identify how much work can actually be outstanding and
where the producer must block; do not equate a model with measured speed.

Then implement an opt-in packet path with one backend owner, strict FIFO order,
one small bounded in-flight window, explicit byte limits, a synchronous reference
mode and conservative coherence RPC. Reject stale epochs. Compare per-operation
memory, partial writes, aliasing, exact fixture pixels and guest event/tick traces.
Exercise forced stalls, full queues, shader fallback, CPU read-after-draw,
write-after-capture, overlapping transfers, resource eviction, unmap, save/load,
reset, pause/background and shutdown. Use race/sanitizer checks where supported.

Only after correctness, measure worker busy time, useful owner overlap, capture
cost/bytes, host and device fence stalls, queue high-water memory, pacing/audio,
thermal behavior and matched final pixels/workload at 100% guest CPU. Check more
than Stories before a default change. This is a candidate for a larger architectural
unlock; its benefit with Cara's actual dependencies remains unmeasured.
