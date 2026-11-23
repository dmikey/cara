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
