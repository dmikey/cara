# Where Gadd waits on the RG Slide

Dark Moon USA booted through file selection into Gadd's lab on the physical RG Slide. These runs used Dynarmic JIT, GLES at 1×, Performance Mode, fixed guest clock 100%, no frame skip, a presentation thread and the same on-device shader seed. The four measured lab windows per boot each span 120 system refresh intervals. Each interval includes time until the emulator completes the next system refresh. The game can submit frames at a different rate. Device configuration and user cache hashes matched after restoration.

## How much work reaches the GPU

With GLES raster-state reuse enabled, the direct and worker boots sampled 109 counter intervals each, approximately 10.90 seconds per boot. The sampler reported four shader cores. Rates use the sum of the driver measurement intervals. The GPU counters cover the device and include compositor work.

| Warm Gadd metric | Direct GLES | Ordered GLES worker |
| --- | ---: | ---: |
| GPU active cycles per wall second, millions | 840.5 | 838.9 |
| Core-active cycles / (4 × GPU active cycles) | 93.66% | 93.54% |
| Fragment queue waiting to issue because processing resources were busy, % of GPU active cycles | 46.23% | 46.72% |
| Tiler unable to issue a position-shading request because the queue was full, % of GPU active cycles | 34.26% | 34.31% |
| Tiler unable to issue a varying-shading request, same denominator | 16.80% | 16.45% |
| Position-shading requests per 1,000 input primitives | 538.7 | 538.5 |
| Fragment queue waiting for a cache flush, % of GPU active cycles | 4.01% | 4.54% |

At Anbernic's rated 850 MHz, the active cycle rates correspond to 98.9% and 98.7% of rated cycle capacity. A live GPU clock was unavailable. Arm counts a cycle as active when there is pending work, including a stalled job. The figures show that the GPU already had work for almost the entire interval under the rated-clock assumption. The stall categories can overlap. They do not identify a particular PICA shader or attachment. [Anbernic specifications](https://anbernic.com/products/rgslide) · [Arm G57 counter definitions](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html)

## Removing redundant raster state

Each of the four boots used the same signed APK. Packed vertex-layout uniform reuse stayed enabled throughout. With raster-state reuse enabled, Cara skips identical GLES raster-state updates. In the worker boot at refresh 2761, the renderer saw exactly 42,926 guest draw requests with either setting. Recorded commands fell from 3,051,240 to 1,182,971. P50 moved from 29.79 to 29.73 ms. At title refresh 961, both runs had 51,168 draw requests. P50 improved from 43.10 to 42.00 ms and p99 from 48.94 to 45.38 ms.

| Submission | Raster reuse | Graphics CPU, ms/refresh | Worker CPU, ms/refresh | Lab window p99 range, ms | Worst refresh, ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Direct | Off | 19.17 | — | 35.51–50.82 | 70.33 |
| Direct | On | 17.66 | — | 49.33–51.84 | 69.78 |
| Worker | Off | 12.57 | 12.61 | 35.05–57.74 | 110.85 |
| Worker | On | 11.76 | 11.02 | 37.28–46.69 | 108.09 |

The producer's surface-remapping scope averaged 7.06 ms of elapsed time per refresh with the worker and raster reuse, versus 0.85 ms on the direct path with reuse. This includes waiting for earlier commands when a helper needs an answer. The guest CPU execution category averaged 5.82 and 5.45 ms of CPU time respectively. Shader compile time and surface download time were zero in all four measured lab windows on each arm. One boot per configuration and different scene animation phases limit claims about p99. [Individual windows](gadd-state-reuse-windows.tsv)

## Where the threads sleep

A separate normal boot with the worker and raster reuse captured Android scheduling events. The trace's clock-sync marker permits alignment to the CaraShader log. The warm slice ends 1 ms before the first new fragment key. In 3,708.59 ms of warm play the producer ran for 2,193.15 ms, including 2,045.32 ms on CPU 7. The worker ran for 1,406.66 ms, all on Cortex-A76 cores. Both ran simultaneously for 977.10 ms (26.35%). Neither ran for 1,085.87 ms (29.28%).

Matching complete producer sleep intervals to their first wake request attributed 1,500.96 ms to 2,208 wakes from CaraGLES. The worker in turn spent 1,500.47 ms asleep before a Mali driver thread named `mali-cmar-backe` requested its wake, plus 680.57 ms before wakes from NativeEmulation. These are overlapping intervals on two CPUs. The waking thread identifies the handoff. An individual wake supplies no GLES call site or hardware fence identity.

The next shader transition had one 506.12 ms vertex compile and program links of 1,764.09 and 2,056.57 ms. Including part of that transition reduced measured producer/worker overlap to 17.24%. The four p99 windows above precede the transition.

To reproduce the scheduler calculation, pass the emulation TID from CaraPerf and the CaraGLES TID from the same process. The script checks for lost context switches and uses the time span shared by all traced CPUs.

```sh
python3 summarize-gles-schedule.py worker-schedule.txt 6565 6578 --to-epoch-ms 1795462972198
```

## Combining equivalent draws on the worker

The combined Android build records both instanced draw entry points in order. Client arrays and client index pointers still complete synchronously. On the G57, the eight-pixel EGL fixture compared two ordinary draws with one two-instance submission for both arrays and an index buffer. Color blending, depth rejection, two stencil increments and indexed vertex ID matched in the worker and direct contexts. The first worker fixture used a blend entry point outside the renderer dispatch. Switching it to the renderer's actual separate blend function fixed the fixture. Full-game pixel equivalence at identical guest refreshes remains unverified. [APK build](https://github.com/dmikey/cara/actions/runs/45251659704) · [Fixture build](https://github.com/dmikey/cara/actions/runs/45252023601)

Both normal boots enabled the GLES worker and raster reuse. The same APK ran first with independent submissions and then with proven draw pairs. Gadd combined 17,316 of 167,913 guest triggers (10.31% fewer submissions). The control processed 169,649 guest triggers. Four 120-refresh windows averaged 11.21 to 10.55 ms of graphics CPU on the producer, and 11.35 to 10.58 ms of worker CPU. The counter sample still showed 538.6 position requests per 1,000 input primitives in both arms.

| Window ending at refresh | Control p99 / worst, ms | Paired p99 / worst, ms |
| ---: | ---: | ---: |
| 2641 | 103.40 / 104.10 | 36.88 / 51.19 |
| 2761 | 44.15 / 82.23 | 42.46 / 62.74 |
| 2881 | 46.92 / 48.83 | 38.49 / 45.84 |
| 3001 | 41.91 / 49.18 | 38.17 / 39.53 |

Worst frames were 2549 (control) and 2701 (paired). The first control window was an outlier relative to prior normal boots. At title refresh 961 the candidate merged zero pairs. P50 moved from 42.24 to 42.32 ms, p99 from 47.00 to 45.72 ms, and graphics CPU from 11.42 to 11.66 ms. This first comparison supports a reduction in preparation and submission cost in Gadd. Repeated boots are needed to quantify the effect on frame tails. The launcher control averaged 2.75 million active GPU cycles per second over 2.90 seconds, versus roughly 839 million in the lab.

## Cost of returning a synchronous GLES result

A profile build bracketed the producer flush and queue call, the worker's raw API invocation and the return to the producer with a monotonic clock. Times are wall milliseconds per system refresh, summed over the four warm windows at 2641, 2761, 2881 and 3001. Profiling targets the synchronous API wrappers. Internal drains and ordinary asynchronous draw calls have separate timing. The query that first needs a result can inherit the backlog of preceding draws.

| Warm Gadd API | Calls/refresh | Before the driver call | In the driver call | Return to producer | Total |
| --- | ---: | ---: | ---: | ---: | ---: |
| glGetIntegerv | 3.04 | 5.588 | 0.008 | 0.038 | 5.634 |
| glFenceSync | 1.00 | 1.642 | 0.120 | 0.011 | 1.772 |
| glGetError | 3.04 | 1.590 | 0.001 | 0.021 | 1.612 |
| glClientWaitSync | 1.00 | 1.156 | 0.002 | 0.014 | 1.172 |
| All measured synchronous calls | 20.27 | 10.396 | 0.143 | 0.162 | 10.701 |

At title refresh 961, three integer queries per refresh spent 21.007 ms waiting to enter the driver and 0.006 ms in it. The surface remap helper saves the active texture unit and checks two enable bits before submitting its conversion draw. The measured approximately 3 integer queries, 6 enable checks, 3 framebuffer status checks and 3 error checks per warm refresh match that helper's shape. This per-API profile does not identify the caller of each query. Across the same four windows the entire remap scope averaged 7.53 ms per refresh. Durations inside remapping overlap the synchronous call figures. In particular, removing a single synchronous query can move its wait to the next result dependency. Moving more work off the producer requires preserving the result dependency for validation and guest reads.

The instrumented workload had p99s of 58.75, 35.33, 47.79 and 47.63 ms. The worst was 86.00 ms at refresh 2620. The fixed-clock four-window run had zero measured shader compilations. The shader links described above occurred later in a separate normal boot. [Profile build](https://github.com/dmikey/cara/actions/runs/45252277457) · [All synchronous API costs](gadd-sync-costs.tsv)

## Which driver calls put the worker to sleep

A separate normal boot profiled the CaraGLES thread with Android Simpleperf during the warm lab. The collector recorded CPU-clock samples and switch-out call chains with `--trace-offcpu`. For each switch sample we joined the next switch-out record and its immediately following switch-in on that thread, counting only the off-core interval. This matched all 6,351 switch samples and 1,809.54 of the 1,813.64 ms of observed off-core intervals over 3,026.20 ms. Earlier sparse sampling, driver return times and these scheduler durations measure different things.

| Worker call or activity | Off CPU, ms | Switch samples | Of those ms with a driver semaphore wait on stack |
| --- | ---: | ---: | ---: |
| Waiting on Cara's queue | 550.18 | 4,095 | — |
| glDrawElements | 538.26 | 281 | 535.01 |
| glBlitFramebuffer | 257.76 | 326 | 255.33 |
| glDrawArrays | 199.17 | 139 | 196.18 |
| glClear | 167.79 | 73 | 167.53 |
| glFramebufferTexture2D | 33.39 | 372 | 0 |
| glClearBufferfv | 21.11 | 122 | 20.36 |
| glBindTexture | 0.80 | 12 | 0 |

The driver actually blocked in draws, blits and clears. Texture binds still consumed on-core time. A driver semaphore can reflect earlier outstanding work. It does not identify a GPU instruction or prove that the particular draw entering the driver is the draw taking the GPU time. The separate texture-packet experiment reduced queue records by 33.1%, but increased producer graphics CPU and failed to improve tails consistently. [Texture packet results](https://github.com/dmikey/cara/blob/1fb11c0/docs/rg-slide-mali-handoff/ordered-gles-worker.md#worker-packet-result)

## Following a producer wait back to its PICA command

Two normal boots on the RG Slide used Dark Moon, Performance Mode, Dynarmic JIT, GLES at 1× and CLK 100%, with the ordered render worker and raster state reuse enabled. Draw pairing was off. The trace carries the original guest operation with each queued GLES command. When the emulation thread requests a synchronous result, it measures how much of its pre-driver wait overlaps each earlier worker command. Time outside those commands includes queue bookkeeping and scheduling. These overlapping wall times belong to the same refresh. Adding them to the producer CPU time would double count part of the work.

On the first boot, earlier commands accounted for 9.584 of the 10.341 ms pre-driver wait per warm refresh. The largest overlap behind a remap query was an unlit indexed draw at 2.817 ms/refresh. The error check at the end of remapping overlapped the remap's own full-screen triangle for 1.401 ms. The display fence overlapped a display clear for 1.202 ms. This locates three different dependencies inside the apparent query cost.

At title window 961, the worker spent 13.567 ms/refresh on queued lookup-table uploads from lit draws while the producer waited. Four uploads per refresh carried 5,394,432 bytes across the 120-refresh window. The procedural LUT scope was idle. The coarse capture labels these uploads `Internal`. The only queued command using that default label in this build is `TransactionLUT` in the GLES queue. Subsequent builds explicitly label the command `UploadLUTOwned`. The measured wall interval includes driver waits for earlier GPU work. An earlier direct-renderer profile independently placed 17.08–18.18 ms/refresh inside the 2D texture transfer at the title. The same worker upload accounted for only 0.122 ms/refresh in warm Gadd. [Coarse capture](gadd-pica-backlog-coarse.log.gz) · [Direct transfer trace and fog experiments](lit-index-followup.md#lighting-and-fog-transfer-result)

The standalone GLES fixture on the physical G57 returned 64 counter bits for both elapsed time and timestamp queries. That verifies that this driver advertises both query targets. The game results above and below measure CPU/driver overlap. Measuring hardware time for an individual pass would require an actual query range, delayed collection, and rejection of disjoint samples. A timestamp on a tile renderer can include work from earlier submissions. [Khronos disjoint timer specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_disjoint_timer_query.txt)

The second boot separated display transfers from texture copies and tagged draws with their PICA vertex entry offsets. Pre-driver waits averaged 11.212 ms/refresh. Earlier worker commands accounted for 10.390 ms (92.7%). In aggregate, unlit PICA work occupied 4.686 ms and lit PICA work 0.815 ms of the producer's wait. This is a conditional measurement of what the worker was doing when the producer requested a result. It excludes work the worker did while the guest could continue.

| Earlier work in warm Gadd | Worker time overlapping the producer wait, ms/refresh |
| --- | ---: |
| Unlit indexed draws, all vertex entries | 3.214 |
| Of those indexed draws, PICA entry 327 | 2.668 |
| Surface-remap triangle | 1.638 |
| Mailbox display clear | 1.487 |
| Guest display-transfer blit | 0.749 |
| Unlit framebuffer attachment change | 0.745 |
| Lit array draws, all entries | 0.288 |
| Guest texture-copy blit | 0.169 |
| Lookup-table upload for lit draws | 0.125 |

Entry 327 accounted for 83% of the indexed-draw overlap. Almost all of it coincided with the remapper querying the active texture unit. This entry is a PICA instruction offset. Different guest programs can use the same offset. The trace alone cannot identify the gameplay effect or show how many nanoseconds a shader instruction spent on the hardware. The remapper's final error check overlapped its conversion triangle for 1.638 ms. The mailbox fence overlapped its clear for 1.487 ms. The client wait on the presentation fence overlapped a guest display-transfer blit for 0.677 ms. At the title, lookup-table uploads again led at 13.919 ms/refresh.

| Gadd window ending at refresh | First trace p50 / p99 / worst, ms | Vertex-tagged repeat p50 / p99 / worst, ms |
| ---: | ---: | ---: |
| 2641 | 32.46 / 106.45 / 107.16 | 33.20 / 107.35 / 107.94 |
| 2761 | 29.26 / 39.01 / 82.02 | 29.98 / 46.66 / 46.93 |
| 2881 | 29.04 / 46.30 / 48.12 | 29.89 / 35.73 / 36.45 |
| 3001 | 29.50 / 42.27 / 49.10 | 30.16 / 37.58 / 50.72 |

The slowest refreshes were 2620 and 2591 respectively. All eight warm windows recorded zero shader compiles. The diagnostics read a clock for every worker command and maintain counters during producer waits. Use separate timing-only boots to judge a performance patch. Both profile restores matched the original configuration and cache checksums. [Detailed capture](gadd-pica-backlog-vertex.log.gz) · [First APK build](https://github.com/dmikey/cara/actions/runs/45254847117) · [Repeat APK build](https://github.com/dmikey/cara/actions/runs/45255452952)

```sh
python3 summarize-pica-backlog.py gadd-pica-backlog-vertex.log.gz 2641 2761 2881 3001
```
