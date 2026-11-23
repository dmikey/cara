# Renderer performance notes for a future Vulkan backend

The goal is to leave more CPU time for FastInterp while preserving guest timing, audio, and all PICA rendering and transfers. These notes distinguish changes already observed in Cara from designs a Vulkan backend would need to validate. The current measurements come from private Metal and public desktop OpenGL on an Apple M1.

## What the Metal measurements tell us

The Xenoblade Chronicles 3D replay starts from a normal boot, presses A at refreshes 660 and 1120 and completes 2,400 guest refreshes. The opening window is 1200–2200. FastInterp runs at 100% guest CPU with SDL audio and no CPU vertex fallback. Each Metal comparison uses the same binary and isolated game data.

| Opening window | Tuned desktop OpenGL | Metal off 1 | Metal in-place export | Metal off 2 |
| --- | ---: | ---: | ---: | ---: |
| Mean host ms per refresh | 17.107 | 16.793 | 18.116 | 16.980 |
| p99 host ms per refresh | — | 36.651 | 63.583 | 38.931 |
| Queued RGBA8 exports | — | 6,750 | 6,750 | 6,750 |
| Temporary copy payload avoided | — | 0 | 1.80 GB | 0 |

The older FastInterp profile attributed 2.99 seconds of host elapsed time to synchronous CPU exports in this window, including waiting. The later private build queued those exports on Metal with zero observed CPU export fallbacks. Different source revisions and options prevent attributing their whole-run timing difference to that change alone.

In the later comparison, writing the RGBA8 export straight into the consumer buffer removed 6,750 scratch copies. The in-place arm still took 6.7–7.9% longer per refresh than both controls, with a worse p99. The result supports keeping the optional path off. Logical copy bytes measure work bypassed. They do not measure physical memory traffic. Adding writes to the same destination buffer can change the driver's scheduling and resource dependencies. A trace would be needed to establish the cause here.

Both LCDs matched at all 12 sampled frames across the Metal cases, along with guest SD files and selected work. A top-screen red/blue discrepancy remains against desktop OpenGL. The OpenGL run completed with zero GL errors. This near-60 Hz test measures normal paced behavior. Uncapped performance and iPhone 13 performance require separate runs.

The same Metal controls recorded 79,664 draws in 2,849 draw groups. Draw groups accounted for the largest total recorded command span among producers, with substantial variation between controls. The existing one-entry vertex uniform memo hit 14,736 of 79,664 uploads, leaving 120,506,368 bytes copied. This is a concrete CPU-side cost to measure. A larger memo trades copies for extra byte comparisons, so owner-thread CPU time is the deciding metric.

### Upload reuse and actual CPU time

A separate private Metal run tested a second exact vertex-uniform memo entry within the same command and upload lease. It directly sampled the emulation/render owner thread's CPU clock after captures at refreshes 1200 and 2200. This includes emulation and render preparation on that thread, plus capture CPU. It excludes scheduler waits and work on other threads.

| Opening window | One entry, first | Two entries | One entry, last |
| --- | ---: | ---: | ---: |
| Owner-thread CPU ms per refresh | 6.760 | 7.048 | 6.527 |
| Mean wall ms per refresh | 17.093 | 17.029 | 16.746 |
| Vertex-uniform bytes copied | 120,506,368 | 93,540,544 | 120,506,368 |
| Reuses from the second entry | 0 | 14,529 | 0 |

Both LCDs, guest SD files, and recorded draws, transfers and presentations matched. Copy payload fell 22.4% while measured owner CPU time rose by 0.289–0.521 ms per refresh. The two controls differed by 0.233 ms of CPU time per refresh. Keep the candidate off. Replaying fewer memory copies is useful only when it buys measured headroom.

For Vulkan and Metal, investigate an exact state identity at draw preparation, before rebuilding and copying the packed block. The starting points are [PICA uniform writes](../emulator/src/video_core/command_processor.cpp) and [Metal uniform capture](../emulator/src/video_core/renderer_metal/pica_metal_native_uniforms.cpp). A revision can certify consecutive unchanged state only if every contributing write and state restoration invalidates it. Shader layout and any fields derived outside the register bank still matter. Repeated A→B→A workloads require a separately proven exact identity. Measure invalidation frequency and compare against serialized bytes before using an identity to bypass the existing path. The uniform experiment receipts are in the workspace output `cara-m1-metal-fastinterp-uniform-20261123`.

### Poll submission retirement at the consumer

An isolated Metal replay moved nonblocking retirement polls out of cached input acquisition and into the already existing draw record check. On the M1 with FastInterp and 100% guest CPU, owner-thread CPU per refresh was 6.673 and 6.701 ms in the two controls and 6.573 ms in the candidate. The 0.100–0.129 ms reduction exceeded the 0.028 ms control difference. Wall time remained inside the control range. Both LCDs at 12 checkpoints, SD files, and draw, grouped submission, transfer, color export and presentation counts matched. The option remains default-off.

A cached input lookup still needs an exact backing identity and a live lease. Keep CPU-visible reads, attachment imports, submission admission, error retirement and publication validations on their existing dependency paths. The timed replay preserved those checks and changed only where duplicate nonblocking polls run. In Vulkan, explicit submission completion can similarly be checked at the renderer's real consumers and admission points, provided resource reuse, queue-family ownership and externally observable completion remain correct.

The M1 Luigi check below follows the Xenoblade result. A physical iPhone 13 comparison remains pending. Continue recording owner-thread CPU, full-process CPU, queue waits, missed deadlines and both displays. Do not promote from one M1 opening or a paced frame-time average. Local receipts are in the workspace output `cara-m1-metal-fastinterp-retire-20261123`.

A separate optimization shared one immutable attachment exporter per generation to avoid cloning callable state per draw. Each case had 77,856 resident color acquisitions and 77,856 resident depth acquisitions in the opening. Owner CPU was 7.098 and 7.398 ms per refresh in the controls and 7.100 ms with shared exporters. Outputs and required work matched. The shared-exporter option stays off. The result does not show that allocator or reference-count overhead is a dominant attachment cost. Local receipts are in the workspace output `cara-m1-metal-fastinterp-attachment-exporter-20261123`.

In the M1 Luigi replay, the candidate used 25.759 ms of owner-thread CPU per refresh against 23.227 ms in the first control. The two runs matched both LCDs at 15 checkpoints and the selected work and guest file checks. A separate Xcode build interrupted the final control, so the replay cannot establish a gain across titles. The first control recorded 12.909 s of pipeline and catalog host spans, including 8.294 s nested in runtime fragment library creation. These instrumented spans include host waits and do not give GPU duration. Local receipts are in `cara-m1-metal-fastinterp-retire-luigi-20261123`.

A separate Xenoblade replay skipped 499,694 of 637,312 Metal raster state setter calls. Its host time was 16.814 ms per refresh against 16.825 and 17.595 ms in the controls. Owner-thread CPU was 6.687 ms against 6.609 and 7.866 ms. Both LCDs at 12 checkpoints, guest SD files and selected work counts matched. Leave this option off: the 0.010 ms host margin is below control drift and owner CPU exceeded the lower control. Call counts alone are an insufficient reason to adopt more state tracking in another backend. Local receipts are in `cara-m1-metal-fastinterp-raster-reuse-20261123`.

### Luigi display transfer fragmentation

On the M1, a private Luigi's Mansion: Dark Moon replay at 100% FastInterp
matched both LCDs at 15 checkpoints, selected work, and guest SD files.
Over refreshes 2401–2800, 795 linear-to-tiled transfers spent 704 ms of
host time inside the retile path. Resolving the source interval accounted
for 661 ms, or 1.65 ms per refresh. No shader libraries or render
pipelines were built in that window.

A finer trace found that all 795 source intervals were fully covered by
multiple native buffer owners. They issued 661,440 staged raw-buffer
copies, averaging 832 spans per transfer and 256 bytes per span, for
169,328,640 logical bytes. The retile resolver recorded no guest RAM reads
or synchronous texture exports in this window. A global queued-color
counter happened to equal the retile count because it covered other
operations. Use route-specific counters to attribute a transfer path.

The finer trace recorded 861 ms of owner-thread CPU across the window.
Raw backing checks and blit encoding accounted for 274 and 291 ms
respectively. The finer trace sampled clocks at each span and adds
measurable overhead. These numbers locate the work and are unsuitable
as a baseline for the cost without diagnostics.

An exact single-texture fusion matched all 22 cases in the Metal byte
fixture but failed the source coverage admission for every gameplay
transfer. It stayed off. This workload points to processing fewer
independent copy commands while preserving every authoritative writer,
source and destination range, queue dependency and guest-visible byte.
Report useful copy reductions and unprofiled owner-thread CPU separately.

## Preserve the backend boundary

Share PICA register decoding, ordered guest operations, the guest-address authority map, and guest completion semantics. Each backend should own its host images, buffers, pipelines, encoders and completion objects. Keep two kinds of completion distinct. Guest time decides when the PICA reports completion to an emulated title. Host completion decides when backing storage can be read safely or reused. Always honor a guest read that needs the latest bytes.

Carry these invariants into the backend interfaces:

- **Track bytes and versions.** Associate each guest interval with the authoritative producer, format, layout, byte coverage and resource generation. A partial overwrite replaces only the affected interval. A sampled-view cache must observe any overlapping write, including through another guest view of the same bytes.
- **Make dependencies explicit.** Render-target writes feed later sampling, copies or scanout. The backend records the required execution and memory visibility before those consumers run. A mixed-owner read may compose several GPU sources and RAM spans into one buffer, with each interval bounded and written once. A direct view is eligible only if a producer covers the full interval and its layout and alignment fit the exact consumer.
- **Give uploads a bounded lifetime.** Keep command-local uniform or vertex snapshots immutable until all consumers retire. A valid hit requires exact payload and layout identity, plus a live allocation. Release memoized ranges on lease rollover, abort or reset. Log copied bytes, comparison cost, peak storage and wait sites together.
- **Preserve draw order.** Group draws when attachments, load/store behavior and dependencies allow it. Keep blending, depth, stencil, scissor and intervening transfers intact. Track which dependency or capacity limit ends a group. Pipeline lookup, shader compilation and queue retirement need separate counters.
- **Keep synchronization and correctness fallback paths.** Unsupported views, unaligned intervals, and guest-visible reads retain the exact resolver until an equivalent implementation passes. Validate produced bytes before a failed or incomplete host operation can appear in the LCDs or guest RAM.

## A Vulkan implementation order

1. Start with one graphics-capable queue. Record producer and consumer access, buffer range or image subresource, and current layout. Use precise barriers and image transitions where required. Add extra queues only after showing a whole-window benefit and handling queue-family ownership where applicable. Tie arena reuse to confirmed submission retirement. [Vulkan synchronization and cache control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
2. Use a persistently mapped, bounded upload arena. Honor device buffer-offset limits and noncoherent atom alignment. Flush noncoherent writes before device consumption and invalidate before host reads when required. Compare direct host-visible reads against staging to device-local memory on the actual device. [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
3. Map compatible attachment groups to render passes or dynamic rendering based on supported features. Supply the correct attachment layouts and load/store operations. Dynamic rendering provides rendering scope information. Establish synchronization separately. [Khronos dynamic rendering proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_dynamic_rendering.html)
4. Cache pipelines using the translated shaders, vertex layout, fixed render state and attachment compatibility. Record initial compilation, first use, and repeat visits separately. Keep device-specific pipeline caches distinct from portable PICA state. Preserve correct first-use drawing when a pipeline has yet to finish.

## Validation on iPhone 13

Compare private Metal and tuned GLES builds with FastInterp, 100% guest CPU, frame skip and adaptive clock off, native resolution, and sound on. Test Xenoblade opening and Luigi Gate with a fixed input script. Verify both LCDs, save data, deterministic work counts, audio continuity and guest completion ordering. Run Mario Kart as a separate check that draw-heavy scenes still benefit.

Report emulation-thread CPU time per guest second, whole-process CPU time, host refresh median and p99, missed display deadlines, allocation and residency, and GPU timing where available. Count first-play shader waits separately from warmed gameplay. A lower owner-thread CPU number with worse pacing or higher CPU elsewhere is insufficient. Use matched runs with the same instrumentation, report the control spread, and log thermal and power conditions for longer phone tests. At a paced 60 Hz the wall-time mean can hide CPU headroom. Measure thread CPU and missed deadlines directly.

Source anchors: [pica_metal_guest_authority.cpp](../emulator/src/video_core/renderer_metal/pica_metal_guest_authority.cpp) for byte ownership, [pica_metal_native_draw_lane.mm](../emulator/src/video_core/renderer_metal/pica_metal_native_draw_lane.mm) for resolve/record ordering, [pica_metal_draw.mm](../emulator/src/video_core/renderer_metal/pica_metal_draw.mm) for group and uniform reuse, [pica_metal_upload.mm](../emulator/src/video_core/renderer_metal/pica_metal_upload.mm) for command-owned upload pages, and [pica_metal_frame_cost.hh](../emulator/src/video_core/renderer_metal/pica_metal_frame_cost.hh) for host and queue instrumentation. Local M1 receipts are in the workspace output `cara-m1-metal-fastinterp-direct-export-20261123`, including the source diff, compiler audit, three Metal replays and the OpenGL reference.
