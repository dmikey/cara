# Fog and lighting texture lifetime on Mali-G57

Dark Moon USA booted normally on the RG Slide with Dynarmic JIT, ordered GLES atlas at 1×, Performance Mode and fixed guest CLK 100%. Every measured run used the same signed APK and disposable app profile. The cache restored 128 program binaries with zero source links. Controls used partial texture updates. The first alternative replaced the complete RG32F image on the same texture name. The distinct-texture alternative initialized a fresh object for each changed table image and retired old names in batches of 32. All three used the same PICA values and fragment shader. [Exact build](https://github.com/dmikey/cara/actions/runs/45274132179)

With separate texture objects, the old contents remain available to earlier draws while later draws sample the newly initialized object. The code submits no fence wait for retiring those names. GLES commands stay ordered, and GLES controls when it can reclaim a deleted object. A physical EGL test checked both color channels and observed all 14,336 pixels match for two simultaneously queued texture generations, including deletion of the first texture before readback. [OpenGL ES 3.2 object lifetime rules](https://registry.khronos.org/OpenGL/specs/es/3.2/es_spec_3.2.pdf)

The Simpleperf scheduler captures show which entry point was on the stack while the graphics worker was asleep. The captures are separate from the timing windows. The initial distinct-texture run was interrupted by a host disk-space error during stack export. The harness restored the original installation and profile, and a fresh normal boot provided the completed distinct-texture sample.

| Run and scene | Capture, ms | Worker asleep in LUT upload, ms | Indexed draws, ms | Blits, ms |
| --- | ---: | ---: | ---: | ---: |
| Partial update, smoky title | 3,012.05 | 1,901.26 | — | — |
| Whole image on same object, smoky title | 3,012.19 | — | 934.90 | — |
| Distinct textures, smoky title | 3,021.34 | 0.29 | 951.54 | — |
| Partial update, warm Gadd | 3,017.32 | — | 477.35 | 280.47 |
| Distinct textures, warm Gadd | 3,026.15 | — | 451.44 | 209.18 |

The partial-update title spent 1,900.73 ms in `osup_sync_object_wait` above `pthread_cond_wait`. With distinct textures, the new image upload accounted for only 0.29 ms of recorded worker sleep. Indexed draws still reached the Mali semaphore, and framebuffer attachment calls accounted for another 276.65 ms at the title. The sampled Gadd draw and blit totals moved between runs, while the lab frame medians stayed around 30 ms. A larger wait under a call can still come from earlier queued GPU work. [Scheduler measurements](dark-moon-fog-lut-waits.tsv)

## Frame times from normal boot

The title comparison uses the matched 120-refresh windows ending at 961, 1081 and 1201. The warm lab comparison uses complete windows with at least 41,800 PICA draw requests and zero measured shader compilation. The initial control's window ending at 3001 crossed lab entry and peaked at 114.58 ms. It is included in the per-window data and excluded from the warm lab row. The save selector rows use the complete windows captured during the hold.

| Scene and run | Median range, ms | Window p99 range, ms | Worst frame, ms |
| --- | ---: | ---: | ---: |
| Title, partial update | 42.18–42.34 | 44.79–47.86 | 51.14 @ 1199 |
| Title, whole image on same object | 36.78–36.95 | 39.50–40.98 | 48.07 @ 1194 |
| Title, distinct textures | 37.23–37.27 | 39.05–39.67 | 41.38 @ 925 |
| Title, partial update repeat | 42.03–42.11 | 44.07–46.03 | 50.56 @ 918 |
| File selection, partial update | 41.27–41.41 | 43.92–45.25 | 45.89 @ 1603 |
| File selection, whole image on same object | 36.28–36.48 | 40.44–45.86 | 46.88 @ 1919 |
| File selection, distinct textures | 36.67–36.75 | 39.16–47.05 | 48.04 @ 1721 |
| File selection, partial update repeat | 41.03–41.13 | 43.55–44.04 | 48.07 @ 1463 |
| Warm Gadd, partial update | 29.73–32.05 | 37.13–48.66 | 84.56 @ 3166 |
| Warm Gadd, whole image on same object | 29.30–30.46 | 43.51–47.59 | 87.79 @ 3589 |
| Warm Gadd, distinct textures | 29.71–30.33 | 38.55–64.07 | 86.49 @ 3251 |
| Warm Gadd, partial update repeat | 29.39–32.59 | 36.85–51.50 | 104.59 @ 2839 |

Distinct textures removed the observed upload wait and improved the compared title p99s. The same-object replacement retained the best title median. The distinct-texture route increased title graphics CPU compared with both partial-upload controls while improving total frame time. File-selection tails and warm Gadd results varied. The distinct-texture route remains behind the `lighting_lut_snapshots` and `lighting_lut_generations` debug settings. [Complete timing windows](dark-moon-fog-lut-windows.tsv)

## What the indexed draws and blits block

A separate normal boot with distinct textures enabled tagged the queued work during four complete warm Gadd windows. The producer waited an average of 11.633 ms per refresh for synchronous worker calls to begin, of which 10.982 ms overlapped earlier recorded work. This diagnostic samples elapsed time on the critical path. It does not measure how long an individual shader ran on the GPU.

| Earlier work | Time overlapping a producer wait, ms/refresh | Principal producer request |
| --- | ---: | --- |
| Unlit indexed draws | 3.620 | Remap state query |
| Unlit indexed draws at PICA vertex entry 327 | 3.110, included above | Remap `glGetIntegerv` |
| Guest display-transfer blits | 1.062 | Presentation `glClientWaitSync` |
| Guest texture-copy blits | 0.171 | Presentation and other queries |

Entry 327 accounts for 86% of the unlit indexed-draw overlap. The producer's active-texture query waited behind it for 3.100 ms per refresh. This reproduces the earlier entry-327 finding with the LUT lifetime change enabled. The blit wait is weighted toward guest display transfers, so optimizing feedback texture copies would attack a different path. The display-transfer implementation may scale or flip, so an image copy is only equivalent when dimensions, orientation and formats allow it. [Matched backlog summary](dark-moon-fog-lut-gadd-backlog.txt) · [Earlier tagged trace](gadd-worker-synchronization.md)

The diagnostic boot is excluded from the timing table. Both test sessions restored the original signed Cara APK, and the app configuration and shader archive checksums matched before and after testing.
