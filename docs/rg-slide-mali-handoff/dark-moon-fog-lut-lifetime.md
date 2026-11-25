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

## Cold shader-hint follow-up

Three normal boots used the same signed Android APK (`d1a4bc6`),
an empty Cara shader archive, and the separable GLES Atlas profile
at native 1× with Dynarmic, Performance Mode, fixed CLK 100%, and
frame skip off. The driver-private cache was retained. The later
arms separately tested `atlas_runtime_tev` and distinct whole-image
LUT generations. The isolated player profile was restored and verified.

| 120-refresh window | Baseline p99 / worst | Unlit runtime TEV p99 / worst | Whole LUT generations p99 / worst |
| --- | ---: | ---: | ---: |
| Smoky title, 25,561 draws | 2,286.93 / 2,295.33 ms | 2,247.13 / 2,396.13 ms | 2,235.15 / 2,393.00 ms |
| Gadd entry, ending 2521 | 175.03 / 1,145.93 ms | 226.41 / 1,206.51 ms | 243.17 / 1,155.52 ms |
| Following lab window, ending 2641 | 61.32 / 796.12 ms | 64.93 / 787.45 ms | 106.38 / 174.83 ms |

The title recorded zero PICA fog draws. The baseline generated 27
distinct fragment sources with lighting enabled and ten without
lighting in the title window. The unlit runtime path took thousands
of selections and left the large title stall in place.

The baseline lab window ending at 2641 attributed 601.96 ms to fog
uploads. Total LUT transfer wall time in the corresponding 120-refresh
window dropped from 601.53 to 9.82 ms with distinct images, while
uploaded bytes increased from 144,384 to 6,995,968. Across the three
windows ending at 2521, 2641 and 2761, transfer time dropped from
632.85 to 14.93 ms. The later foreground fragment burst shifted
into window 2761 with generations enabled, where the worst refresh
was 799.34 ms. The window p99s therefore measure different mixtures
of scene and compiler work. Whole-image generations also increased
title uploads from 71,680 to 19,955,712 bytes. Keep this experiment
opt-in. An independent 1 KiB fog resource would let earlier draws
retain their exact table while preserving the existing lighting
residency path.

The baseline settled lab window ending at 2881 had zero LUT uploads.
It spent 21.46 ms per refresh in graphics-command CPU work and 5.85 ms
in guest CPU execution. Fog resource lifetime, fragment links and
steady lit draw processing need separate measurements.

[Android build used in all three arms](https://github.com/dmikey/cara/actions/runs/45507523389)

## Independent fog texture and lit shader experiment

The Android GLES Atlas renderer now keeps PICA fog in independent,
versioned 128 × 1 RG32F textures. Each changed table uploads 1 KiB.
Draws already in the queue keep their earlier table, while lighting
continues to use its existing storage. GLSL still interpolates the
same 128 pairs of PICA fog values. The production selector uses the
GLES renderer string to enable this on Mali-G57 across games. The
`separate_fog_lut` debug override allows controlled comparisons.

The RG Slide ran Dark Moon from a normal boot with an empty Cara
shader archive, the system driver's existing cache, and identical
ROM-derived hints. Tests used Dynarmic, GLES Atlas, Performance Mode,
native 1×, fixed CLK 100 and frame skip off. The first two candidates
used explicit debug settings to isolate fog and the lit combiner.

| Window | Original p99 / worst | Independent fog p99 / worst | Fog + lit combiner p99 / worst |
| --- | ---: | ---: | ---: |
| Smoky title, 25,561 draws | 2,286.93 / 2,295.33 ms | 2,125.54 / 2,270.25 ms | 1,879.18 / 2,531.70 ms |
| Lab entry, through refresh 2521 | 175.03 / 1,145.93 ms | 198.48 / 1,122.37 ms | 249.80 / 1,105.73 ms |
| Following lab window, through refresh 2641 | 61.32 / 796.12 ms | 82.10 / 814.54 ms | 139.71 / 142.50 ms |
| Matched settled lab window, 41,880 draws | 32.44 / 33.06 ms | 32.43 / 32.76 ms | 40.59 / 62.77 ms |

The lab entry windows begin at different points in the guest
animation. In particular, the lit shader experiment's next
foreground fragment burst occurred in the window ending at 2761
and produced a 651.00 ms refresh. The settled comparison uses
windows with the same draw count and no foreground shader builds.
The median was 30.30 ms originally, 30.18 ms with independent fog,
and 33.94 ms with the lit combiner.

Across the two lab-entry windows, elapsed time attributed to fog
uploads dropped from 633.90 to 9.49 ms. In the title, the fog-only
build still recorded four lighting uploads totaling 71,680 bytes
and zero PICA fog draws. Title-only shader stalls therefore have
a separate cause.

The optional lit combiner reduced foreground fragment links in the
title window from 36 to 29 compared with the fog-only candidate.
Time inside those links changed from 3,163.61 to 3,150.87 ms. The
same windows also contained about 9.9 and 9.7 seconds, respectively,
of summed vertex link time on the compiler worker. This can overlap
foreground work, so the totals should not be added to frame time.
The combiner stays off because the title's worst refresh and
settled gameplay both regressed. The traces identify foreground
driver link time and possible worker contention as separate
targets for a follow-up, with overlap still to be measured.

[Fog-only Android build](https://github.com/dmikey/cara/actions/runs/45511627135) ·
[Lit combiner Android build](https://github.com/dmikey/cara/actions/runs/45512044038)

A second title-only boot on the same fog-only APK omitted the hint
file. The same 25,561-draw window recorded 1,912.50 ms p99 and
42,179.27 ms worst, at refresh 515, compared with 2,125.54 ms p99
and 2,270.25 ms worst with hints. The no-hint trace put several
multi-second vertex links on the foreground thread at refresh 515.
The hint file both prioritizes a matched ROM vertex fingerprint and
enables bounded scheduling of unmatched live vertex sources in the
separable program path. Removing it changes both behaviors. This
control confirms that the 42-second stall is reproducible and that
the fog lifetime change does not control that stall.

[Arm guidance on updates to resources used by queued draws](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-6-efficiently-updating-dynamic-resources)

The shipping linked-program route was also tested on the fog-only
APK with the same hint file present. Its 25,561-draw title window
recorded 31,256.05 ms p99 and 49,864.90 ms worst, with 81,717.95 ms
of measured shader compile and link work. This confirms that the
hinted separable comparison cannot be used as the timing for the
shipping linked backend. A production cold fix needs matched
linked-program coverage, especially when an exact fragment program
has no already-linked software-vertex fallback.
