# Cara’s architecture and foundations

## What Cara carries forward from MMJ

Cara’s pinned emulator foundation is weihuoya’s Android-focused Citra MMJ branch.
That branch carries Citra’s original emulation and graphics systems alongside
MMJ’s mobile-oriented changes. The lineage below describes the implementation
Cara inherited; it does not attribute every original Citra subsystem solely to MMJ.

| Inherited system | Role in the mobile renderer | Cara’s development |
|---|---|---|
| **GPU draw acceleration and OpenGL ES rendering** | The command processor routes supported guest draw batches to the hardware renderer. | A custom Apple vertex pipeline adds GPU execution of vertex programs, packed input decoding, and direct native screen presentation. |
| **Shader generation and program reuse** | The generator/decompiler translates guest graphics state and programs into host shaders; the shader manager caches stages and linked programs for reuse. | Cara adapts shader generation for its GLES3 vertex interfaces and Apple driver behavior. Shader reuse within a run is distinct from persistent disk caching. |
| **Texture and render-target caching** | The surface cache finds reusable GPU resources, tracks valid memory regions, and handles copies and synchronization with guest RAM. | Cara develops prepared-input reuse, bounded upload caches, and memory-coherence handling to reduce repeated staging and transfers while preserving guest-visible data. |
| **Performance-oriented renderer controls** | MMJ exposes options such as Skip Slow Draw and Skip CPU Access, allowing speed/correctness tradeoffs. | These controls are part of the inherited source, not the basis of Cara’s reported rendering gains. Cara’s work targets redundant processing and transfers. |

### Source lineage

The pinned MMJ implementation can be inspected in the
[command processor](https://github.com/weihuoya/citra/blob/302b0c3bc4312bc96489c11584a97664894b343f/src/video_core/command_processor.cpp),
[shader manager](https://github.com/weihuoya/citra/blob/302b0c3bc4312bc96489c11584a97664894b343f/src/video_core/renderer_opengl/gl_shader_manager.cpp),
and [surface cache](https://github.com/weihuoya/citra/blob/302b0c3bc4312bc96489c11584a97664894b343f/src/video_core/renderer_opengl/gl_rasterizer_cache.cpp).
Cara’s corresponding implementations are bundled under
[`emulator/src/video_core`](../emulator/src/video_core/).

MMJ’s Android-generated shader program binaries and bundled system-data assets
are excluded from Cara. The iOS frontend currently disables persistent shader
caching; shader generation and in-memory reuse remain part of the renderer.

## FastInterp and Cara’s execution path

Cara adapts **Azahar FastInterp** for CPU execution without JIT. Guest instructions
are decoded into data records and dispatched to precompiled handlers in the signed
app. Cara integrates that interpreter with its memory, scheduling, and save-state
systems. See [FastInterp](fastinterp.md) for backend details and provenance.

Cara’s custom GLES3 rendering path moves vertex processing and packed vertex
input decoding onto the GPU. Prepared data reuse reduces repeated CPU expansion
and uploads. Direct GPU presentation avoids reading every displayed frame back
into CPU memory. The [performance results](performance.md) distinguish measured
whole-app outcomes from the purpose of individual optimizations.

## Current execution and frame-production work

Cara extends FastInterp with static arithmetic specialization, bucketed instruction
timing lookup, an eight-way decoded-block cache on iOS, decode-record reuse fixes,
and cache-independent chained-block execution budgets. Static paired-instruction
and load/compare prototypes remain experimental; they are not reported as enabled
performance gains.

The [guest frame-production investigation](gpu-frame-production.md) records the
Stories underclock and internal-frame-limit findings, separates host presentation
from guest draw production, and defines the Luigi, Zelda and Xenoblade checks for
any reusable engine fix. [Prior art](performance-prior-art.md) links the upstream
interpreter and GPU timing implementations being evaluated.

## Recent GPU-residency and host-worker findings

The M1 development backend can keep Luigi’s supported RGBA8 tiled/linear alias
conversion on the GPU. Its matched checked comparison reached 16.65 ms per
refresh from 28.41 ms, with no underclock, JIT or guest-timing change. Native
OpenGL on the Mac and GLES3 on iOS are distinct paths; the result is not a native
Metal or Vulkan measurement. See the [current update](updates/2026-11-16.md) and
[per-run evidence](performance.md).

An opt-in ordered host worker also has measured M1 results. It preserves owned
upload bytes and GL mapping semantics, and trades more whole-process CPU for
some overlap. Producer-side PICA interpretation, memory ownership and dependency
checks remain significant; a worker is not an automatic solution to every CPU
cost. The [worker proposal and status](host-render-queue.md) distinguish tested
parts from the larger ownership design.

The existing guest descriptor executor itself has an outstanding-work gate.
Offline Stories traces show P3D waking its descriptor worker and next high-level
building overlapping older submissions. They do not prove early hardware
completion or justify a universal frame-rate cap. [Updated causal findings](gpu-frame-production.md#retained-m1-causal-traces-2026-11-15)
record the measured boundary and missing evidence.

## A reusable core and a native shell

Cara is developed as a reusable 3DS core for integration into Delta, alongside
its own standalone iOS shell. The shell provides game loading from Files, touch
controls, customizable Delta-compatible skins, audio, save states, and iCloud
save-folder support. Delta integration is a development target; the standalone
Cara app is the playable build provided here.

## Source and attribution

Cara builds on GPL-2.0-or-later emulator code. Original copyright notices are
preserved. See [NOTICE](../NOTICE), [source revisions](../emulator/VENDORED_SOURCE.json),
and [FastInterp provenance](fastinterp-upstream.json) for contributors and pinned revisions.

See the [renderer performance notes for a future Vulkan backend](renderer-vulkan-handoff.md) for resource lifetime, GPU ownership, and reproducible Metal experiments.
