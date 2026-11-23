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

The producer's surface-remapping scope averaged 7.06 ms of elapsed time per refresh with the worker and raster reuse, versus 0.85 ms on the direct path with reuse. This includes waiting for earlier commands when a helper needs an answer. The guest CPU execution category averaged 5.82 and 5.45 ms of CPU time respectively. Shader compile time and surface download time were zero in all four measured lab windows on each arm. One boot per configuration and different scene animation phases limit claims about p99.

## Where the threads sleep

A separate normal boot with the worker and raster reuse captured Android scheduling events. The trace's clock-sync marker permits alignment to the CaraShader log. The warm slice ends 1 ms before the first new fragment key. In 3,708.59 ms of warm play the producer ran for 2,193.15 ms, including 2,045.32 ms on CPU 7. The worker ran for 1,406.66 ms, all on Cortex-A76 cores. Both ran simultaneously for 977.10 ms (26.35%). Neither ran for 1,085.87 ms (29.28%).

Matching complete producer sleep intervals to their first wake request attributed 1,500.96 ms to 2,208 wakes from CaraGLES. The worker in turn spent 1,500.47 ms asleep before a Mali driver thread named `mali-cmar-backe` requested its wake, plus 680.57 ms before wakes from NativeEmulation. These are overlapping intervals on two CPUs. The waking thread identifies the handoff. An individual wake supplies no GLES call site or hardware fence identity.

The next shader transition had one 506.12 ms vertex compile and program links of 1,764.09 and 2,056.57 ms. Including part of that transition reduced measured producer/worker overlap to 17.24%. The four p99 windows above precede the transition.

To reproduce the scheduler calculation, pass the emulation TID from CaraPerf and the CaraGLES TID from the same process. The script checks for lost context switches and uses the time span shared by all traced CPUs.

```sh
python3 summarize-gles-schedule.py worker-schedule.txt 6565 6578 --to-epoch-ms 1795462972198
```
