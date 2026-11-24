# Dark Moon shader stalls on RG Slide

RG Slide T820 and Mali-G57, Dark Moon USA, normal boot from a raw 3DS image,
JIT + GLES Atlas, Performance Mode, 1×, and guest CLK 100%. This run used a
disposable Cara profile with an empty Cara shader cache. The driver-private
cache was preserved. The prior APK and profile were restored after the run,
and the original config and shader-cache SHA-256 values matched.

## Cold control

APK `3967947`. Title checkpoint arrived in 105.34 s. The first measured
cold title compile/link burst consumed 81,109.11 ms across a 120-refresh
window. Window p99 was 30,962.63 ms and its worst refresh was 49,616.51 ms.

| Lab window ending at system refresh | p50 | p99 | Worst | Shader scope wall time in window |
| ---: | ---: | ---: | ---: | ---: |
| 2264 | 17.64 ms | 87.09 ms | 40,979.32 ms | 40,790.14 ms |
| 2504 | 28.25 ms | 165.91 ms | 30,501.47 ms | 30,440.00 ms |
| 2624 | 28.26 ms | 51.64 ms | 76.83 ms | 0 ms |
| 2744 | 28.86 ms | 51.62 ms | 52.88 ms | 0 ms |
| 2864 | 29.03 ms | 50.02 ms | 51.26 ms | 0 ms |
| 2984 | 28.94 ms | 51.92 ms | 34,364.72 ms | 34,293.78 ms |
| 3464 | 32.42 ms | 3,185.28 ms | 42,004.04 ms | 45,041.12 ms |

The first lab HUD checkpoint showed 23 guest-submitted FPS and 47% SPD with
CLK 100%. Frame times above are system refresh durations. Several single
refresh stalls are large enough to dominate responsiveness while appearing
outside a 120-sample p99. The report keeps the maxima alongside p99.

The saved cache reached its 128-entry cap before the run ended. These
entries reference 34 distinct vertex sources and 74 distinct fragment
sources. Of the fragment sources, 54 contain generated per-light vector
code. The disk snapshot contains 19,214,012 bytes of program binaries and
10,737,551 bytes of GLSL strings, with 3,382,163 unique GLSL bytes across
both stages. This snapshot cannot enumerate all keys or compile attempts.

The legacy cold trace collects compile and link under a single aggregate.
The shared census APK adds separate events for generation by PICA key,
source hash, fragment compile, link, binary restore, and first PICA draw
submission. Its log includes the thread ID for driver work.

## Shared GLES census

APK `2344283`, with exact stage reuse enabled on the single existing EGL
worker. The app's program cache started empty and the driver-private cache
was preserved. Title checkpoint arrived in 103.81 s.

| Phase | New PICA keys | Distinct generated FS | Fragment compile calls, emulation / other | Emulation compile and status | Emulation link and status | Worst first PICA draw |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Boot through title | 43 | 37 | 44 / 10 | 9.30 s | 73.80 s | 5.11 ms |
| Save transition through lab checkpoint | 52 | 43 | 37 / 11 | 3.41 s | 104.57 s | 0.27 ms |

The generated fragment count includes 27 lit sources through the title
and 28 newly generated lit sources after save selection. Compile counts
include GL helper and worker sources outside the PICA generation log.
Cold lab refreshes 2332 and 2511 took 41.78 s and 31.27 s respectively.
Across four subsequent lab windows with zero recorded shader work, p99
ranged from 34.27 to 54.53 ms, worst refresh was 167.38 ms, and the HUD
showed 27 game FPS at 57% SPD and CLK 100%.

The existing source cache already collapses PICA keys that emit identical
GLSL. It cannot remove the cost of linking distinct vertex/fragment pairs.
The emulation thread still linked 56 programs on the route from save
selection into the lab even with an active EGL worker for indexed draws.
Link time includes the blocking GL status query. The observed first-draw
times measure host submission only.

Programs linked to known lit fragment sources account for **89.18 s** of
the 104.57 s emulation-thread link total after save selection. The six
highest-cost lit fragment sources each account for roughly 5.0 s across
their vertex/fragment program combinations. The next four each account for
about 4.0 s. This points to the exact programs using lit fragments as the
primary target for the hybrid path. The lit shaders in the capped cache
include 18 five-light, 12 seven-light, and 21 eight-light sources.

## Warm restart

The same diagnostic APK loaded 128 binaries in 90.87 ms total GL upload
and status-query wall time. The title checkpoint arrived in 21.49 s. The
save transition to the same lab checkpoint generated the same 52 PICA
keys and 43 fragment sources without a driver fragment compile or program
link. Across four lab windows, p99 was 35.18–49.57 ms, worst refresh was
69.68 ms, and the HUD showed 27 FPS at 58% SPD and CLK 100%. Shader
prewarming removes the observed compile burst on this sampled route.
Sustained lab speed remains limited after shader work disappears.

The original APK and player profile were restored after both runs. The
original config and shader-cache SHA-256 checks matched.

## Warm attachment reuse probe

The same APK and seed cache were used with paired color/depth FBO reuse
enabled. Four warm lab windows contained 37,200–37,820 cache hits each
and zero cached FBO rebuilds. Median draw submission wall
time was 454.39 ms per 120 refreshes for the control and 435.62 ms for
the probe, about 0.16 ms per refresh lower.

| Warm lab | p50 range | p99 range | Worst refresh | HUD |
| --- | ---: | ---: | ---: | --- |
| Control | 28.03–29.28 ms | 35.18–49.57 ms | 69.68 ms | 27 FPS / 58% SPD |
| Paired FBO | 28.74–29.18 ms | 35.94–50.51 ms | 59.76 ms | 27 FPS / 58% SPD |

This single A/B shows similar sustained throughput. The reduced largest
refresh needs repeated measured A/B runs. Device counters were recorded
for the FBO probe alone, so they cannot establish a tiler-cycle delta.
The FBO option remains experimental.

Twenty fragment source hashes were seen in compile requests on both the
emulation thread and the worker. Summed wall time for those requests was
81 ms on emulation and 2 ms on the worker. That duplication exists, but
eliminating it would address very little of the observed cold stall.

## Unused combiner operand correction

APK `53770ae` passed Android CI. A normal cold run matched all 95 PICA
keys and all their generated source hashes through the title and the lab.
The title still had 54 emulation-thread links and the save/lab phase had
56. Title boot took 104.86 s and the worst title refresh was 50.24 s.
The two largest early lab stalls were 41.42 s and 30.47 s. This game
route never exercised a combination that would emit fewer sources under
the unused-operand correction. The fixture verified the code path with
synthetic input and the device experiment establishes no Dark Moon gain.

## Existing separable shader route

Android's existing `shader_type=2` option failed on the RG Slide before
the title. The GLES 3.2 driver rejected a desktop ARB extension directive,
an interface block qualifier, and an interface layout qualifier. The
probe was stopped after the shader failures, with the original app and
profile restored. See `separable-driver-errors.txt` for the original probe
and [the later GLES 3.2 stage measurements](gles-validation.md#first-mali-stage-measurements)
for the corrected implementation.

## Bravely Second on the RG Slide

The November 24 comparison used JIT, 1×, and CLK 100% on the stock Mali-G57
driver. Through Radiant Blast and Black Bane, the installed Cara and MMJ
emulation threads made 52 and 53 GLES program links respectively. Cara
spent 4.30 s in links on the first profiled app-cache-cold boot, compared
with 22.71 ms on MMJ. The app archives began empty and the driver-private
cache was preserved. Several of the most expensive Cara links reused a
single 45,922-byte packed-input vertex shader. The GL shader object was
already shared, but Mali repeated substantial work for each link.

Commit `3c32d1a` adds an Android diagnostic that emits one guest vertex
program call site while preserving the reference and actual evaluations.
Replaying the twelve associated programs in a native EGL pbuffer on the
Slide cut total link time from 6.19 to 4.60 s. With the same preview APK,
independent empty app archives, and the same prologue save, total link
time fell from 6.89 to 4.96 s and the slowest link from 651 to 511 ms.

| Presentation intervals, p99 / worst | Cold control | Cold probe | Warm probe |
| --- | ---: | ---: | ---: |
| Boot and prologue | 33.35 / 3,270.25 ms | 33.27 / 2,313.26 ms | 25.12 / 166.68 ms |
| Radiant Blast | 41.66 / 458.21 ms | 41.65 / 383.32 ms | 25.15 / 41.63 ms |
| Black Bane | 49.95 / 474.94 ms | 25.08 / 366.44 ms | 25.08 / 25.12 ms |

The warm probe restored 45 linked binaries and showed 30 FPS / 100% SPD
at Black Bane. The cold stalls remain and the probe stays off by default.
A separable-program control reduced repeated links but still rebuilt a
roughly one-second vertex stage on restart. Exact on-device replay also
cut link time from 1.94 to 1.28 s by omitting quaternion reference work for
five fragment sources that never read it. That needs an in-game check
before adoption. [Android preview CI](https://github.com/dmikey/cara/actions/runs/45410338317)

The Slide's stock Vulkan driver reports 1.3.225, pipeline creation cache
control, creation feedback, dynamic rendering, and synchronization2.
Graphics pipeline libraries were absent. MaliSX2 supports the G57 via
Arm's kbase interface as an optional Vulkan comparison in a compatible
frontend. Mesa's upstream Panfrost GLES and PanVK have separate support
matrices. Cara's measured path here uses the stock GLES driver.
[MaliSX2 source](https://github.com/bmdhacks/malisx2) ·
[Mesa Panfrost support](https://docs.mesa3d.org/drivers/panfrost.html)

### Native attributes and the actual MMJ shader

A second GLES trace of the installed MMJ captured native-attribute GLSL,
one guest vertex evaluation, a quaternion passed through directly, and
more than 197,000 ranged indexed draws. The published renderer includes
a fixed geometry stage, which the installed trace did not create. Moving
the captured MMJ programs into an independent EGL context cost 1.21 s
for ten links. Its earlier 23 ms in-app total included a substantially
different driver-cache context.

The existing Cara GLES 3.2 preview reduced the worst cold Bravely present
from 3,270 to 833 ms. It reached 30 FPS and 100% speed at Black Bane on
repeat, with 25.04 ms p99 and 25.09 ms worst during that spell. This
renderer also changes surface and submission paths. Dark Moon already
shows warm costs relative to Atlas.

Twelve exact Cara program pairs took 6.28 s to link in a device replay
with packed input and repeated vertex evaluation. A compact geometry
stage preserved the reference quaternion rule and cut this to 3.66 s.
Native attribute inputs with that same stage cut it to 1.59 s. A test
triangle with an opposing quaternion matched all pixels from a
pre-corrected reference. In-game throughput of the combined route is
still open.

Commit `b7cdf84` skips unused reference evaluations on an optional Mali
Atlas route. The stored archive showed 28 pruned entries and every paired
fragment left the quaternion unused. All 45 binaries restored on a
repeat boot. At warm Black Bane the game showed 30 FPS and 100% speed,
with 25.10 ms p99 and 25.12 ms worst presentation. Running the patch
first with a fresh preview reproduced a 3.25 s cold stall. The earlier
1.25 s result was sensitive to run order. The diagnostic remains off
by default. [Android CI](https://github.com/dmikey/cara/actions/runs/45414714508)
### Regular Atlas programs and background linking

At refresh 706 after Continue in Bravely Second, the GLES driver spent
3,044 ms linking six programs during a 3,234 ms emulated refresh. Four
links reused the same translated vertex stage and needed the triangle’s
reference quaternion. The first draw after each link took 64–121 µs to
submit. In a replay of those six programs on the Slide, native attributes
plus the compact quaternion geometry stage reduced total link time from
2.89–2.92 s to 0.70–0.71 s. That replay isolated linker cost. The geometry
stage and vertex feed require separate in-game validation.

The shared EGL worker experiment builds the same vertex and fragment
pair as the synchronous path. A pending program can defer the accelerated
draw only where the regular PICA software vertex path is available.
That path runs the guest vertex program and submits the result through the
existing fragment stage. Surface ownership is updated after an actual
draw, with the normal depth, stencil, and blending behavior. Shadow
draws use their existing synchronous route. If a worker context cannot be
created or the worker declines a build, the regular path can link inline.
Completed programs enter the same cache as synchronous builds.

The diagnostic is controlled by Android’s
`atlas_async_programmable_shaders` debug key and remains off by default.
Android provides the EGL context, while selection is based on the program
and draw requirements. The separate quaternion-elision probe uses the
same GLES fragment dependency check across Atlas hosts.

The same Atlas preview APK at `2bcc5b8` booted Bravely Second from the
title screen on the stock Slide at JIT, 1×, and CLK 100%. The first
cold run used the worker and its existing stage cache. The synchronous
control ran after a fresh preview install. Both started with empty Cara
archives, shared the same player-data snapshot, and reached Radiant
Blast and Black Bane. The independent metrics keep host presentation
intervals separate from emulated system refresh times.

| Cold metric | Synchronous | Worker |
| --- | ---: | ---: |
| Whole-run present p99 / worst | 33.29 / 3,361.62 ms | 33.30 / 282.92 ms |
| Radiant Blast present p99 / worst | 16.82 / 465.97 ms | 33.28 / 33.31 ms |
| Black Bane present p99 / worst | 33.30 / 482.52 ms | 33.29 / 49.95 ms |
| Worst emulated refresh | 3,350.00 ms | 285.68 ms |
| Longest emulation-thread link | 686.87 ms | 44.60 ms |

The worst synchronous refresh was 674, after Continue, with 3,281.19 ms
of emulation-thread shader work in the enclosing window. In the same
transition, the worker run’s largest refresh was 226.90 ms. The
worker run’s worst refresh overall was 305, during save selection.
That window still attributed 244.34 ms to emulation-thread shader work.
Across the full run, the worker linked 29 programs in 7.66 s while the
emulation thread linked 51 in 0.76 s. The time moved off the rendering
critical path. The CPU vertex fallback still needs its own first-use
links, and the total compile workload rose. [Preview build](https://github.com/dmikey/cara/actions/runs/45420125664)

A second worker run, after clearing the preview app’s data, measured
33.34 ms present p99 and 308.24 ms worst. The same save-selection
refresh remained the worst. One 45,922-byte packed vertex stage
accounted for 12 links and 6.34 s in the synchronous run. The repeated
worker run put those links on the worker. Much of the remaining cold
stall is the first use of the fallback’s vertex and fragment pairs.
There were no failed GL links in either run.

| Warm present p99 / worst | Synchronous | Worker |
| --- | ---: | ---: |
| Whole run | 33.23 / 166.73 ms | 25.16 / 166.57 ms |
| Radiant Blast | 25.12 / 49.95 ms | 25.20 / 49.95 ms |
| Black Bane | 25.07 / 25.09 ms | 25.06 / 25.07 ms |

Warm Black Bane ran at 30 FPS and 100% emulation speed in both
runs. The synchronous and worker archives restored 45 and 67 binary
entries, respectively, with no source links during restore. The largest
warm refresh fell in save selection and recorded no compile time in
either run. The captured spell frames show the geometry and effects in
both modes. The animations were captured at different instants, so
pixel equivalence remains unverified. The preview package was removed
after the test and the Slide kept production Cara `07a145eb`.
