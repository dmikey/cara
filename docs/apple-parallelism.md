# Apple CPU and renderer parallelism

Status: source review and architecture proposal, 2026-11-13. No renderer worker is implemented by this document. M4 iPad is the first measurement target; A19 validation is pending. Keep the implementation portable ARM64 and select measured policies per device rather than assuming identical cache, memory bandwidth, thermal budget, or scheduling behavior.

## Existing ownership and threads

Cara runs guest CPU execution and synchronous GPU command processing on the emulation thread. `emulator/src/core/core.cpp::RunLoopMultiCores` advances guest cores sequentially; this is not a host parallel ARM interpreter. Splitting an instruction stream or concurrently running those guest cores requires kernel, memory, exclusive-monitor, scheduling, and event-order work beyond a renderer optimization.

`emulator/src/video_core/renderer_opengl/renderer_opengl.cpp` already contains an optional presentation mailbox (`RenderToMailbox`, `TryPresent`). Its mutex, queues, and GL fences coordinate screen presentation, not Pica command execution. `GetPresentFrame` can discard a pending presentation frame. That policy is not a substitute for an ordered renderer worker or permission to omit guest draws. iOS frontend display/layout synchronization does not protect rasterizer or guest-memory state.

The important shared mutable state is currently unsynchronized:

| Source | Ownership constraint |
| --- | --- |
| `emulator/src/video_core/command_processor.cpp` and `pica_state.h` | Command parsing, register writes, shader setup and primitive state use `Pica::g_state`. Command lists point into mutable guest memory. A raw pointer is not an immutable work packet. |
| `emulator/src/video_core/renderer_opengl/gl_state.cpp` | `OpenGLState::cur_state` and raster-state validity are global shadow state, not per-thread GL-context state. |
| `emulator/src/core/memory.cpp` | CPU accesses to rasterizer-cached pages call flush/invalidate directly. Cache marking also changes CPU page-table pointers/attributes. Unmapping and region operations participate in coherence. |
| `emulator/src/video_core/renderer_opengl/gl_rasterizer_cache.cpp` | Surface flush/download, invalidation, overlap and eviction mutate caches and issue GL operations. No worker ownership protocol currently covers these calls. |
| `emulator/src/video_core/vertex_loader.cpp` | Existing loading reads guest backing memory and global default attributes. `GetPhysicalRef` keeps backing storage alive; it does not freeze contents. |

Moving `ProcessCommandList`, `AccelerateDrawBatch`, or `Draw` wholesale to another thread would race these consumers. Required fences include CPU reads of GPU-written memory, CPU writes to cached sources, remap/unmap, transfer/fill/readback, overlapping textures/render targets, save/load/reset and shutdown. Preserve guest event deadlines and recipients independently of host worker completion; the separate experimental P3D timing work is not authorization to reorder host rendering.

## Smallest actual overlap interval

In `gl_rasterizer.cpp::AccelerateDrawBatch`, the current order is `PrepareIOSVertices`, reuse-mode selection, `SetupVertexShader`, `SetupGeometryShader`, then `Draw`. `Draw` acquires framebuffer and texture surfaces, configures attachments/state, synchronizes LUTs and uploads uniforms before `AccelerateDrawBatchInternal` uploads vertex inputs and submits the draw. This is a real potential overlap interval: a CPU-only worker could prepare an immutable captured input packet while the owner performs those framebuffer/texture/shader/state operations. Join before vertex input upload/submission. It requires splitting capture/validation from transformation and preserving fallback behavior; simply putting the current preparation call on a worker and immediately joining adds overhead without overlap.

The worker must receive owned index/attribute bytes, layout/default attributes and private scratch. It must never call GL, guest Memory, global Pica state, kernel or timing APIs. Snapshot before owner-side cache operations that could change backing memory. Reuse-mode/shader decisions currently depend on preparation results (`ios_indexed_vertex_inputs`, cached index metadata and counts); those dependencies must either be resolved safely during capture or remain after the join. Thus the whole shader setup interval cannot yet be assumed available. State changes on a failed accelerated path must retain existing fallback semantics.

`AccelerateDrawBatchInternal` selects a texture upload-bank entry, performs `memcmp` reuse tests, and immediately uploads inputs based on their results. A worker introduced at the existing comparison call would immediately join. A useful comparison worker would need earlier selection/pinning of immutable shadow buffers, or a narrower split that compares index/element data while the owner uploads independent vertex inputs. The latter has a shorter interval and risks extra synchronization for little work. Do not add generic queue infrastructure before proving a real overlap interval in the consumer.

The older instrumented resumed-draw capture (`profiles/resumed-draw-diagnostic/draws-frame-timing.json`, last three windows) measured roughly 0.75–0.77 ms/frame vertex preparation and 0.62–0.64 ms/frame reuse comparison. Their sum, about 1.4 ms/frame, bounds work available in these two categories for that capture; it is not an achievable speedup or a general per-title ceiling. Snapshot copies, already-packed memcpy paths, dispatch/join cost and overlap dependencies reduce the recoverable portion. Shader compilation was absent in those windows. The observed roughly 10.6 ms CPU execution plus 11.6 ms GPU-command work cannot all be overlapped by this small worker.

A bounded prototype should use one persistent CPU worker, private per-job scratch, a large-draw threshold, at most one or two in-flight packets and an explicit byte cap. Commit results and counters on the owner in original order. Drain or invalidate generations on state changes and shutdown. Compare output bytes, pixels, guest state and timing invariants before accepting device performance results.

## Larger ordered backend queue

To overlap substantial guest CPU execution with renderer work, keep Pica frontend parsing, guest timing and service events on the emulation thread and introduce an ordered immutable backend packet stream. One dedicated thread owns the GL context, GL shadow state and surface cache. Packets must capture all consumed data/state or hold a defined coherent resource version. Bounded backpressure prevents unbounded RAM use. Memory/coherence callers need explicit synchronous RPC or sequence fences, including downloads and CPU-visible transfers. Save/load/reset must drain and invalidate outstanding generations. This is a substantial architecture change, not an environment flag or a presentation-thread toggle.

## Verified upstream mechanisms

* [Azahar Vulkan scheduler](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_vulkan/vk_scheduler.cpp): records command chunks and dispatches a FIFO to a persistent worker; `WaitWorker` and `Finish` are explicit synchronization points. This is backend command recording/submission, not proof that arbitrary Pica or guest state can run concurrently.
* [Azahar OpenGL renderer](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_opengl/renderer_opengl.cpp): presentation/mailbox separation is distinct from a general GPU-command worker. Do not describe the existing OpenGL presentation thread as parallel guest instruction execution.
* [PPSSPP GLRenderManager](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLRenderManager.cpp): producer-side recorded steps become ordered render-thread SUBMIT/PRESENT tasks; readbacks use explicit `FlushSync`. This is the closest concrete model for a larger GLES backend-owner queue.
* [PPSSPP software TransformUnit](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/TransformUnit.cpp): actual geometry helper jobs use bounded helper slots, chunk claiming, generations, workload thresholds and completion tracking. Transferable to immutable vertex work, with Cara-specific dependency analysis still required.
* [Dolphin FIFO](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/Fifo.cpp) and [asynchronous requests](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/AsyncRequests.cpp): GPU-thread operation has explicit FIFO synchronization and pipeline flushes before queued result-sensitive operations. Its architecture demonstrates necessary synchronization, not a drop-in Cara implementation.
* [Apple GLES concurrency guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/OpenGLES_ProgrammingGuide/ConcurrencyandOpenGLES/ConcurrencyandOpenGLES.html): a context cannot be accessed concurrently, and independent CPU work is the useful concurrency target. This archived API guidance is an ownership constraint, not current M4 performance evidence.

## Apple-first acceptance gates

Measure first on M4 with the same title, checkpoint, replay, 100% guest clock, renderer flags and resolution. Record worker busy time, useful owner overlap, join stalls, snapshot bytes/time, queue high-water bytes, CPU/GPU-command phases, frame-time distributions, memory pressure and thermal state. Preserve event order, complete draws and pixel output. Validate Luigi, Zelda and Xenoblade independently before claiming a general improvement.

Use portable ARM64 code and OS scheduling rather than hardcoded CPU affinity, assumed cache geometry, or undocumented performance-core selection. Benchmark bounded job sizes and worker QoS; an interactive QoS label does not guarantee a performance core. Keep temporary memory below a measured device budget, avoid false sharing, and test whether extra copies contend for cache/bandwidth or increase energy enough to erase the gain. A19 may need different thresholds and queue limits; its performance, RAM pressure and sustained thermal behavior remain unmeasured. Enable a policy per validated device only after correctness and repeatable wins, with an inline fallback.
