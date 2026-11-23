# Dark Moon remap and packed vertex measurements on the RG Slide

All gameplay measurements start from a normal boot of the USA ROM, navigate the title and save selector, and continue into Gadd's lab on the Unisoc T820 / Mali-G57. They use Dynarmic JIT, the GLES atlas at 1×, Performance Mode, P3D/PPF pacing, and fixed guest CLK 100%. Each launch gets an isolated configuration and the same warm shader archive. The title window ends at refresh 961. The four lab windows contain 120 refreshes each. Timing includes lab entry. Individual frame times, draw counts, and compilation are reported per window.

Graphics CPU is exclusive emulation-thread CPU time charged to GPU commands. Worker CPU is the CPU consumed by CaraGLES. Synchronous GLES time is the producer's full wall time for calls that require a result, including queue wait and actual driver execution. Changing the number of synchronous calls can move a wait into another scope. Compare the independent remap and frame wall times as well. Historical overlap with entry 327 identifies commands ahead of a query. Surface provenance records the last marked owner, its vertex offset if it was a draw, and whether its conservative address interval covers the entire copied span. Neither an address range nor a vertex entry uniquely identifies the pixels written by an individual shader.

## Repeated surface and vertex checks

On the same APK, a control followed by verified index identity and the exact surface lookup cache changed graphics CPU from 11.437 to 11.088 ms per refresh. The index-check scope went from 1.954 to 1.488 ms. The cache served 557.8 of 729.6 lookups per refresh. The producer's synchronous GLES time changed from 11.166 to 11.066 ms, and the worker's indexed entry-327 overlap changed from 2.587 to 2.564 ms. A third boot verified cached surface choices against the full lookup on every hit and found zero mismatches in the lab. The comparison establishes a reduction in CPU preparation. Frame-tail and queue-wait gains remain unverified.

At refresh 2881 of the control, 42,228 draws copied 589,130,512 guest vertex bytes, cleared 653,467,648 packed staging bytes and accounted for 344,700,928 bytes of vector resize initialization over 120 refreshes. The upload cache avoided 738,520,832 logical upload bytes. The 5,427,456 remaining logical vertex upload bytes already include the index portion. Staging counters describe writes by the implementation. They do not measure external DRAM traffic.

## Correctness checks

The physical EGL fixture compared the existing byte remapper with the two-texel variant across both RGB/RGBA directions, equal formats, all four tiled/linear source/destination combinations and five byte offsets. All 80 cases matched all 256 RGBA pixels. The worker returned the correct complete and incomplete framebuffer results and restored the active texture unit, dither, and rasterizer discard state. The host padding fixture passed 4,323 cases under ASan/UBSan. The immutable shadow fixture passed 75,142 ownership and lifetime checks. [GLES fixture build](https://github.com/dmikey/cara/actions/runs/45259287865)

## Which draws feed the remap

The owner trace saw 1,446 remaps in four lab windows. All read a 32-bit tiled color draw surface and produced a 32-bit linear view. The most recent marked writer of the source was vertex entry 155 in 1,419 calls and entry 159 in 27. The tracked range covered each copied span. At the warm title, entry 155 was the last writer for all 360 remaps in window 961. Entry 327's indexed draws occupied the worker before the synchronous remap query, but this trace recorded zero entry-327 source owners. Replacing or skipping that earlier draw on an ownership assumption would be incorrect.

Consolidating the remap's state queries, completeness check and error return into one worker call put 8.25 ms/refresh inside that call. Total producer synchronous time was 11.79 ms versus 11.25 and 11.52 in the bracketing control boots. The two-texel shader alone reduced the measured remap scope to 7.80 ms, versus 8.19 and 8.20 in those controls. Its frame p99 ranges overlapped the controls. Neither change establishes a consistent end-to-end speedup.

## Upstream techniques tested against this workload

Arm describes a write-after-read hazard in pipelined GLES. Updating a sampled texture can require waiting or retaining its old contents. Its guidance motivated the independent complete-lighting-image experiment in the [menu investigation](dark-moon-smoke-menu.md). Dolphin fences ranges in a streaming ring and waits when a requested range will be reused. That technique requires retaining the true set of preceding readers when applying it to the lookup-table texture. PPSSPP transfers lifetime-managed commands to its GL thread, and MMJ preserves indexed geometry in its GLES draw path. Cara's new worker experiments retain ownership of vertex bytes through submission and keep the existing guest-memory comparisons at reuse boundaries. [Arm dynamic resources](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-6-efficiently-updating-dynamic-resources) · [Dolphin stream buffers](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/OGL/OGLStreamBuffer.cpp) · [PPSSPP GL submission](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLRenderManager.cpp) · [MMJ geometry submission](https://github.com/weihuoya/citra/blob/302b0c3bc4312bc96489c11584a97664894b343f/src/video_core/renderer_opengl/gl_rasterizer.cpp#L415-L455)

## Matched normal-boot Gadd results

The `e57cc87` test APK collected four warm, compile-free Gadd windows per arm. The sampled interval is one guest system refresh. All values below are milliseconds. Graphics CPU time is exclusive to the producer thread and cannot be added to the worker's CPU time to get a frame time.

| Arm | Graphics CPU/refresh | Worker CPU/refresh | Synchronous GLES wall/refresh | Window median range | Window p99 range | Worst refresh |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Initial control | 11.94 | 11.67 | 10.54 | 29.54–32.98 | 35.09–109.61 | 146.96 @ 2541 |
| Index/surface reuse + staging + shared payload | 10.56 | 11.17 | 11.08 | 29.20–32.38 | 35.40–48.07 | 107.47 @ 2593 |
| Same + mailbox fence packet | 10.53 | 11.30 | 11.32 | 29.77–32.58 | 35.17–78.56 | 101.69 @ 2581 |
| Repeat control | 11.23 | 11.30 | 11.05 | 29.60–32.88 | 37.34–104.87 | 112.93 @ 2533 |
| Same as mailbox + packed vertex fetch | 10.87 | 11.04 | 10.28 | 28.00–31.53 | 33.21–108.35 | 134.64 @ 2540 |

The staging arm reduced explicit clears from 5.45 MB to 0.45 MB per refresh and eliminated 2.87 MB/refresh of value-initialization. It still copied 4.91 MB/refresh of guest vertex data. It saved 0.67 ms of graphics CPU relative to the repeated control, with synchronous wall time still near 11 ms. The mailbox fixture verified both directions of fence exchange with a second EGL context. Moving the wait to the worker did not establish a frame-tail win. Packed fetch produced a 28.00 ms window median and also a 134.64 ms worst refresh. Its archive had a different shader identity. The table uses settled windows, and the selected refresh numbers can shift when entry compiles.

The production configuration remains the reference. These trials are branch experiments and add no player-facing controls. [Per-window draw counts, frame times and CPU times](gadd-remap-copy-windows.tsv) · [Complete ARM64 APK build](https://github.com/dmikey/cara/actions/runs/45260829831)
