# Dark Moon's smoky title and save selection on Mali-G57

A normal boot on the RG Slide used Dynarmic JIT, GLES atlas at 1×, fixed guest CLK 100%, the ordered graphics worker, and Performance Mode. The title and save selector were each left running before taking an independent three-second scheduler-stack capture on the GLES worker. The frame comparison below uses complete 120-refresh windows inside each held scene, outside the stack recordings, with zero reported shader compilation. Each test used a disposable app profile.

| Scene | Draws/refresh | 2D table median | Native buffer median | 2D table p99 | Native buffer p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Title | 425–427 | 42.18–42.39 ms | 38.26–38.27 ms | 45.19–46.93 ms | 41.31–46.56 ms |
| Select a file | 467 | 41.36–41.64 ms | 37.76–37.90 ms | 44.55–46.18 ms | 40.97–48.21 ms |

On build `286e078`, the 2D path sent 44,621–44,954 bytes of table data per refresh. Previous direct-submission measurements counted four logical updates per refresh. Lighting and depth fog share this texture. Small guest table changes can interrupt the host GPU pipeline even when the number of bytes is small. No draw or effect was disabled to obtain these measurements. [Earlier transfer-size measurements](lit-index-followup.md#lighting-and-fog-transfer-result)

## Where the graphics worker blocked

The scheduler recorded 1,887.44 ms off CPU in the title's table upload in a 3,015.85 ms capture. **1,886.94 ms** of that had the Mali driver's `osup_sync_object_wait` above `pthread_cond_wait`. The save selector recorded 1,801.34 ms in the upload out of 3,008.15 ms, with **1,800.77 ms** in the same driver wait. Cara's caller was the lookup-table uploader. The vendor stack identifies the waiting thread and API. It does not expose the specific pending GPU job or distinguish copying an old backing store from waiting for its readers. [Worker samples and scheduler intervals](dark-moon-smoke-worker.tsv) · [Arm on updating in-flight textures](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-6-efficiently-updating-dynamic-resources)

Using the existing native GLES texture-buffer representation removed the partial 2D updates. In the same scenes the worker then spent 978.02 and 838.41 ms, respectively, off CPU inside `glDrawElements`. Those stacks predominantly reached a Mali semaphore. Framebuffer attachment and binding also carried waits. The net saving was about four milliseconds per refresh. A caller's total blocking time overestimates the amount of work that would disappear after optimizing it. The native variant used its own shader identity. The comparison is of warmed, compile-free menu windows.

The opening uses the LUT differently. An earlier direct-submission trace found 96 fog-sized updates of 1 KiB each in the 120-refresh window ending at 1681, totaling 0.99 ms of transfer wall time per refresh. The next two windows had no updates. Lab entry reached 4.49 and 7.05 ms, then the established lab window dropped to zero. The smoky menus and the sustained lab have different critical paths. [Opening and lab timing](lit-index-followup.md#lighting-and-fog-transfer-result)

## Multi-second cold freezes

On `e57cc87`, a normal boot with an older build's pipeline archive hit **50,127.10 ms** on refresh 515. The 120-refresh window recorded 81,648.91 ms of compile/link time, with 57 link events. The save-to-opening transition hit **25,036.85 ms** on refresh 1656. Its window recorded 26,794.35 ms of compile/link time and 17 link events. The slowest individual link in that window took 3,107.54 ms. Compilation events and renderer scopes cover different thread and operation boundaries and should not be added together.

The cache signature included the complete host rasterizer class header. Changing its private staging buffer invalidated the archive even though the program sources and GPU-facing layouts remained compatible. The branch now tracks the generator, UBO/attribute declarations, vertex layout, shader helpers and cache schema, and excludes the CPU-only rasterizer header. The generator builds without that header. A CMake dependency test confirmed that changing host scratch no longer changes the key, while changing the shader UBO header, native geometry source or shared TEV shader text does. This changes the key once on upgrade. Driver, OS, feature, and archive-integrity checks still apply. New programs on a first-ever run still require linking.

An initial launch of the older profiler APK crashed in the AudioTrack thread before reaching the title. Its top frame resolved to `AudioCore::CubebSink::Impl::DataCallback`. The harness restored the original installation and retried. No time from that failed launch is included.

## Keeping the same shader and replacing the whole table image

The second experiment (`00d2de9`, hidden test setting `lighting_lut_snapshots`) keeps the RG32F 2D sampler, shader binary, draw order, and fog interpolation. The rasterizer assigns stable offsets to all 24 lighting tables and the 128-entry fog table. When a table actually changes, it packages one complete, padded 1024 × 7 texture image. The worker owns the bytes until submission and replaces the image with one `glTexImage2D`. GLES must preserve the old contents for earlier draws. The implementation leaves the choice of backing storage to the driver. A physical GLES fixture verified both channels of all 14,336 pixels across two queued generations and checked unpack state and the active texture unit. This is a synthetic table-sampling oracle. [Exact test APK](https://github.com/dmikey/cara/actions/runs/45262587431) · [Fixture build](https://github.com/dmikey/cara/actions/runs/45263131076)

Both menus submitted four complete images per refresh, or 229,376 bytes. The partial-update control uploaded roughly 44.6 kB. On the complete-image path the warm title median fell to 36.87–37.40 ms and the save selector to 36.57–36.64 ms. Both bracketing controls ran slower at the median. The title p99 improved. File-selection p99 was 45.56–47.28 ms, compared with 43.62–44.23 ms in the first control. Median improvement alone is insufficient for a default. [All complete menu windows](dark-moon-smoke-windows.tsv)

The content of the GPU queue also changed. In four-second title samples, the fragment queue contained work for 69.65% of GPU-active cycles on the initial control and 82.99% on the complete-image path. Non-fragment occupancy moved from 54.22% to 64.68%. Whole-device overdraw was 1.37 and 1.34 shaded fragments per pixel at the median. External traffic increased from 1,001 to 1,160 bytes per input primitive. These counters span the whole workload and were captured separately from the worker call stacks. Queue occupancy includes stalls. The title's fragment job-issue stalls were 53.56% and 58.77% of GPU-active cycles respectively. [Per-phase hardware counters](dark-moon-smoke-counters.tsv) · [Arm G57 counter definitions](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html)

## How much work crosses the command queue

In the title window ending at 961, the ordered path recorded 1,249,502 commands over 120 refreshes, split into 20,304 worker batches with 2,160 synchronous requests. That is 10,412.5 commands, 169.2 batches and 18 synchronous requests per refresh for 426.4 guest draws. The complete-image path had the same command and batch deltas. An earlier per-API census attributed 3,039.8 records per refresh to selecting a texture unit, 2,502.4 to binding a texture and 1,398.6 to binding a buffer. These are submitted operations across the entire scene, including helpers. Command frequency is not elapsed driver time.

The comparison supports giving each in-flight draw a stable view of the tables it reads. Further reduction should focus on which binding changes survive to a draw or a guest-visible dependency. A deferred state packet has to account for uploads, remaps, helper contexts and readbacks that observe bindings between two guest draws. Simply moving the same sequence of barriers or collecting consecutive scalar binds into one queue item can leave the driver doing the same work. [Related Gadd synchronization experiments](gadd-worker-synchronization.md)

A prior scalar-versus-texture-packet benchmark gives a useful negative control. At title refresh 961, packetizing the same GLES work reduced queue records from 10,412.5 to 7,404.4 per refresh and batches from 169.2 to 122.6. The window median changed from 42.46 to 41.87 ms and p99 from 46.17 to 46.43 ms. At Gadd refresh 2881 the corresponding medians were 29.79 and 29.64 ms. Compare that with the five-millisecond median gain from changing the resource lifetime while leaving the command count essentially unchanged.

One source of actual bind churn is the cleanup at the end of a PICA draw. The rasterizer clears all ordinary guest texture names and calls `state.Apply()` because the cache may destroy a texture. A deferred unbind would need to preserve texture deletion, live attachment and sampling alias handling, image-store barriers, and the bindings observed by helper operations. The API census counts every caller, so it cannot establish how many of the recorded binds this cleanup caused or how many can safely disappear. A signature-by-signature count of immediate unbind/rebind pairs and their intervening consumers is the next discriminating measurement.

On the second APK (`00d2de9`), the warm control restored **128 of 128 local program binaries with zero source links**. Time to the title was 21.28 s, down from 104.69 s with an incompatible archive. Refresh 515 fell from 49,907.82 to 520.37 ms. The maximum between save confirmation and the lab gate fell from 41,023.87 to 115.99 ms. The complete-image boot restored the same archive and measured 522.37 and 118.73 ms. Warm sample completeness and the repeated control isolate a separate first-use cost at refresh 515. [Android shader-cache dependency build](https://github.com/dmikey/cara/actions/runs/45264428668)

## The half-second hitch with a valid archive

A paired thread capture repeated the first title transition on the partial-update control with 128 of 128 binaries restored. Refresh 515 took 523.50 ms with zero compile time in its window. SurfaceFlinger independently recorded a 524.90 ms presentation gap, the largest in the common 7.02 s Simpleperf recording. Scheduler intervals and CPU samples were restricted to the two presentation timestamps.

| Thread | On CPU during the gap | Largest measured wait | On-CPU samples |
| --- | ---: | --- | --- |
| Emulation | 446.88 ms | 76.26 ms waiting for GLES results | 62 of 87 samples included the Dynarmic backend, 13 were in PICA draw preparation |
| GLES worker | 82.18 ms | 440.85 ms waiting on its work queue | 6 of 17 were in texture subimage uploads |

The longest single empty-worker wait was 258.4 ms. The emulation stack included ARM64 block emission, `RegAlloc::UpdateAllUses`, `RegAlloc::AssertAllUnlocked`, and constructing a random device. Those compiler costs occur when the guest executes new CPU blocks even with the graphics pipeline archive warm. This result identifies a producer-limited interval inside an otherwise driver-limited menu. The scheduler times overlap between threads. [Gap-specific thread timing and sample counts](dark-moon-refresh515.tsv)

### Testing the ARM64 producer

The control build is `81b104d`. Candidate `97cde21` keeps a high-water mark for the spill slots actually used while emitting one block. It preserves the original allocation order, reuses freed slots and retains the final assertion over the entire spill array. Spill candidate selection uses a bounded local array. The random engine is initialized once per compiler thread, when it first needs a victim. Guest CPU timing, PICA execution and the GLES renderer are identical between these APKs.

On the M3 Max, the vendored Dynarmic suite with the A32 frontend passed 200,561 assertions in 47 cases. A separate oracle translated an ARMv7 NEON block using 32 D registers 512 times, clearing the translation cache every iteration. All 64 integer lanes and the final guest PC matched a scalar reference in the original and candidate builds. In nine alternating host trials, the median for the whole 512-translation program fell from 123.56 to 110.41 ms. That is a host compiler workload, separate from the RG Slide game measurement.


The Slide restored all 128 program binaries on each warm boot of the control and the two candidate runs. The normal-boot title window ending at refresh 601 contained 25,561 draws every time and recorded zero shader compilation. Refresh 515 took **517.30 ms** on the control, **435.36 ms** on the first candidate boot and **433.45 ms** on the repeat. Window p99 fell from **151.55 ms** to **126.02 / 127.28 ms**. This change cut a repeatable 82–84 ms from the title transition. The remaining 433–435 ms is still visible. The sampler that attributed the original stall was disabled in all three timing runs.

| Warm Gadd, JIT + GLES, CLK 100% | Graphics CPU per refresh | Median frame range | Window p99 range | Worst frame |
| --- | ---: | ---: | ---: | ---: |
| Control | 11.27 ms | 29.61–32.51 ms | 35.92–48.46 ms | 110.74 ms @ 2593 |
| JIT candidate | 11.14 ms | 29.69–32.66 ms | 36.80–51.17 ms | 89.45 ms @ 2746 |
| Candidate repeat | 11.34 ms | 29.83–32.72 ms | 37.95–88.37 ms | 104.43 ms @ 2593 |

The register allocator improves the first title transition. Gadd still misses the 16.7 ms budget. Frame-tail differences between the two candidate boots are larger than the median differences versus the control. Every run restored the original signed APK and the checked user configuration and cache. [Exact candidate build](https://github.com/dmikey/cara/actions/runs/45266874010) · [Gadd window data](dark-moon-jit-gadd.tsv) · [Normal-boot freezes and shader links](dark-moon-cold-links.tsv)
