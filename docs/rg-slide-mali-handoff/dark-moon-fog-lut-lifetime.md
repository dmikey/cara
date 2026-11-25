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

The automatic-selector build `f463f764` passed Android production
and ordered-GLES probe CI. It was signed and installed on the same
Slide, whose stock driver reports `Mali-G57`. The normal boot used
the same separable comparison profile without a `separate_fog_lut`
override. The first two lab-entry windows recorded 180 1 KiB fog
uploads, zero mixed light/fog uploads, and 9.13 ms total fog upload
time. The app reached the settled lab. The matched 41,880-draw
window showed 29.64 ms median, 33.01 ms p99 and 52.89 ms worst.
The title recorded 2,319.82 ms p99 and 2,348.64 ms worst in the
same 25,561-draw window. There was no logged shader build failure.
The test profile was isolated and the player profile was restored
with matching file hashes. These tests compared render progress
and timings. They did not compare pixels at identical guest ticks.

[Verified automatic-selector Android build](https://github.com/dmikey/cara/actions/runs/45513683248)

## Exact p95 replay

Android now logs the nearest-rank p95 of each 120-refresh window
and includes the 114th sample in the phase trace. The earlier trace
started at rank 115. The same signed build (`5cbbf688`) ran two normal
boots on the Slide with Dynarmic, separable GLES Atlas, CLK 100,
Performance Mode and frame skip off. Both profiles began with
empty Cara archives, the same ROM hints and the existing driver
cache. Automatic fog ran first. The control forced fog isolation
off. The title and settled rows match by draw count. Lab entry
animation timing varied, so those rows compare adjacent scene
windows. The test verified all 50 p95 summary values against their
rank-114 samples and restored the player profile after each run.

| Scene | Fog off p95 / p99 / worst (ms) | Fog on p95 / p99 / worst (ms) |
| --- | ---: | ---: |
| Smoky title, 25,561 draws | 190.84 / 2,233.43 / 2,337.38 | 177.56 / 2,211.67 / 2,390.43 |
| Lab entry, window ending 2521 | 131.87 / 148.70 / 1,155.69 | 134.33 / 242.10 / 1,142.54 |
| Following lab window, ending 2641 | 40.67 / 89.39 / 794.14 | 31.37 / 59.91 / 795.21 |
| Settled lab, 41,880 draws | 34.06 / 35.57 / 38.42 | 31.22 / 32.53 / 61.52 |

Both title p95 refreshes ran software vertices on 238 of 285
draws, with zero foreground shader compile time. The p95 refresh
with fog on used 164.49 ms of graphics-command CPU time and
8.12 ms of guest execution. The title had zero PICA fog draws,
so its timing difference cannot be attributed to the fog LUT.

Lab-entry p95 had software vertices on 72–108 of 351 draws and
121.90–124.48 ms of graphics-command CPU time across the two runs.
Neither p95 refresh recorded foreground compilation. In the next
lab window, the p95 frame spent 5.93 ms in LUT uploads with fog
isolation off and 0.14 ms with it on. The two lab-entry windows
accumulated 451.52 vs 8.85 ms of fog upload time. In the same later
window the worst refresh stayed around 795 ms, of which 747–755 ms
was foreground compile and link time.

The matched settled p95 with fog on had zero software vertex draws
and zero compile time. Graphics-command CPU still took 20.44 ms,
with another 6.39 ms in guest execution. Neither settled window had
fog uploads, so this pair does not establish a warm fog speedup.

[Android CI](https://github.com/dmikey/cara/actions/runs/45515359560)

## Temporary GPU vertex stage

An opt-in separable GLES experiment built the same decompiled PICA
vertex program with one GLSL call site for the triangle reference and
actual vertex evaluations. The draw still evaluates the same inputs,
preserves the quaternion sign rule, and uses the existing fragment
stage. The same EGL worker publishes this temporary program, then
builds the specialized stage. The CPU vertex path continues to draw
while both programs are pending. This branch excludes the priority
queue change.

The RG Slide ran two timing boots on signed APK `3586abfa` with JIT,
separable GLES Atlas, CLK 100, Performance Mode, native 1×, empty
Cara archives, identical ROM hints, and the existing system driver
cache. Draw skipping and adaptive frame skip were disabled. The
temporary program ran first. The two title windows match draw counts.
The lab windows compare the same scene progression, with different
input and animation timing.

| Window | Original FIFO p95 / p99 / worst (ms) | Temporary GPU p95 / p99 / worst (ms) |
| --- | ---: | ---: |
| First smoky title | 181.45 / 2,130.09 / 2,191.22 | 186.54 / 2,167.97 / 2,401.98 |
| Following smoky title | 82.35 / 85.40 / 86.57 | 83.18 / 87.15 / 96.26 |
| Gadd entry, through refresh 2521 | 124.12 / 243.00 / 1,169.07 | 125.69 / 148.59 / 1,176.16 |
| Following lab window, through refresh 2641 | 34.64 / 60.96 / 823.20 | 34.26 / 81.77 / 830.64 |

At the title, source `a929c69f4d7ad36a` first ran through the
temporary GPU stage at refresh 525. Its specialized stage replaced
it at refresh 573, compared with refresh 538 with FIFO. The driver
spent 2.02 s linking the temporary stage and 3.41 s linking the
specialized stage. The FIFO exact link took 3.23 s. The temporary
stage reduced software draws in the first title window by 70,
from 15,005 to 14,935. The following window added 1,550 software
draws while other stages waited longer. A smaller on-demand shader
still competes for driver compiler time.

Separate boots captured raw RGBA at named guest refreshes. All 18
matched title samples were byte-identical outside the FPS overlay.
This includes the eight samples at refreshes 525–560 during the
temporary-stage interval. Every difference was within x=74–213,
y=7–19, where the FPS and speed text is drawn. The player reported
flicker during the test. The control pixel run was stopped before
the lab, so Gadd pixel parity and the cause of the observed flicker
are unresolved. The previous signed build (`62bf2a8a`) was restored
and relaunched at the library. The player profile matched its
pre-test hashes. The temporary GPU experiment stays off.

[Android CI](https://github.com/dmikey/cara/actions/runs/45520829077)

### Upstream approaches to the first draw

- Dolphin's hybrid renderer uses an already compiled general shader
  for the first draw, then switches to a specialization. Its measured
  architecture avoids building two programs at first encounter.
  [Dolphin hybrid ubershaders](https://dolphin-emu.org/blog/2017/07/30/ubershaders/)
- RPCS3 preloads its shader interpreter and uses guest instructions
  from separate buffers. It also has explicit eligibility checks
  where interpreter coverage is incomplete. This is closer to PICA's
  programmable vertex stage than a fixed-function combiner
  ubershader. Cara can use the pattern with its own PICA semantics
  and GLES resources. [RPCS3 OpenGL implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/GL/GLGSRender.cpp)
- Azahar's programmable GLES path looks up a generated vertex shader
  and returns failure when it has no handle. Its Android configuration
  exposes parallel compilation for Vulkan. This provides a useful
  specialized shader and CPU software baseline for comparison.
  [Azahar GLES shader manager](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_opengl/gl_shader_manager.cpp) ·
  [Azahar Android configuration](https://github.com/azahar-emu/azahar/blob/master/src/android/app/src/main/jni/default_ini.h)
- PPSSPP measures render-thread wait separately from total worker
  time. Those figures are for MoltenVK, but the method applies to
  Cara's EGL worker: log time from first draw request to first use,
  alongside actual foreground wait and driver compile and link.
  Arm's offline compiler can estimate G57 spills and occupancy for
  the eventual reusable shader. Link latency still needs measurement
  on the Slide's stock driver.
  [PPSSPP compiler measurements](https://raw.githubusercontent.com/hrydgard/ppsspp/master/docs/shader-compilation.md) ·
  [Arm Mali Offline Compiler](https://developer.arm.com/Tools%20and%20Software/Mali%20Offline%20Compiler)

A reusable PICA vertex fallback should compile on-device before
gameplay, accept validated instruction and swizzle data, and retain
the existing packed vertex inputs and draw order. Program and
swizzle identities are already tracked by `Pica::Shader::ShaderSetup`.
A first fixture can upload an immutable instruction record once per
identity and bind current uniforms and input layout for each draw.
Keep the supported control flow explicit. Compare vertex outputs,
triangle quaternion signs, color and depth against Cara's software
path on the G57 before admitting an opcode or input route. Retain
the existing exact path for unsupported state. Separate opaque,
translucent and depth-dependent cases so title parity alone cannot
qualify the implementation.

## Black-frame flicker controls and PICA interpreter fixture

The RG Slide's Android compositor recording reproduced an alternation
between the lab image and completely black LCDs while the FPS overlay
remained visible. The temporary GPU stage and queue experiments were
off. Turning the independent fog table off did not remove the
alternation. Omitting the ROM hints in separable GLES disabled the
hinted vertex worker, logged zero software vertex draws at lab entry,
and still reproduced the black frames.

The same signed APK (`3586abfa`) retained async vertex hints and
rendered with inline presentation. In a 15-second lab segment the
compositor had zero isolated large image changes that reverted on
the next sample, compared with 26 with the mailbox and independent fog
active. Representative mailbox frames visibly alternate between the
same Gadd shot and black. Inline presentation also changes the
stream-buffer policy on Android, so this test narrows the failing
configuration without isolating the underlying bug. It does not
show that worker activity alone is sufficient to cause flicker.

The capture boots used JIT, separable GLES Atlas, fixed CLK 100,
1×, disabled slow-draw skipping and adaptive frame skip, fresh Cara
archives, and the existing driver cache. Reported timing includes
diagnostic readbacks and screen recording, and lab timing shifts
between boots. An async-hints-off control reproduced a 42.07-second
cold title refresh and an 8.29-second lab-entry refresh.

| Capture window | Mailbox p95 / p99 / worst (ms) | Inline p95 / p99 / worst (ms) |
| --- | ---: | ---: |
| Smoky title, 25,561 draws | 196.57 / 2,364.45 / 2,487.16 | 191.65 / 2,282.89 / 2,361.92 |
| Lab entry through 2521 | 159.15 / 206.15 / 1,131.92 | 156.80 / 303.55 / 1,199.59 |
| Settled lab through 2881 | 47.70 / 56.89 / 81.18 | 39.08 / 41.76 / 65.82 |

The fog-on/off readbacks matched all five sampled title images once
the FPS overlay was excluded. Seven lab-entry frames matched after
aligning a six-refresh offset in the scene. The later samples had
different animation timing. These captures do not validate Gadd's
native depth attachment.

The standalone GLES interpreter fixture passed on the physical
Mali-G57 with raw PICA MOV, ADD, MUL and END instructions, operand
swizzles, and a masked alpha write. Its colors and depth rejection
matched the existing reference shader. It has a fixed fixture layout
and linear instruction stream. Guest control flow, complete register
coverage, shaders from Dark Moon, and game integration remain open.
The fixture executable passed CI and ran separately from Cara.
[Interpreter fixture build](https://github.com/dmikey/cara/actions/runs/45524039196)

The Slide was returned to signed build `62bf2a8a`. The game profiles
were restored before setting the device's `use_present_thread` to
`False`. The previous settings directory was saved on the device,
and the other settings files matched during staging. The workaround
does not change shader policy or enable either experiment.

## Post-flicker performance round

The screenshot state fix was tested on the Slide with both threaded
presentation and shader hints enabled. The eight-second compositor
sample went from 50 black samples out of 613 to zero out of 636 with
the same alternating capture schedule. All 65 sampled bottom-LCD
crops matched at the same refresh. The moving top-LCD sequence could
not be aligned for an exact pixel comparison. This test did not
read or compare guest depth. [Screenshot fix](https://github.com/dmikey/cara/pull/129)

Separate normal boots used Dynarmic, separable GLES Atlas, fixed
CLK 100, native resolution, draw skipping off, and no screenshot
readbacks. The cold run started with an empty Cara archive. The
warm run copied its archive into a fresh profile. System driver
caches persisted across both runs.

| 120-refresh window | Cold p95 / p99 / worst (ms) | Warm p95 / p99 / worst (ms) |
| --- | ---: | ---: |
| Smoky title through 601 | 181.04 / 2171.71 / 2442.70 | 48.59 / 133.74 / 483.41 |
| Gadd entry through 2521 | 141.08 / 190.39 / 1122.97 | 31.99 / 45.49 / 134.82 |
| Settled lab, windows ending 2761–3001 | 31.48–31.95 / 32.95–54.66 / 54.67 | 31.85–32.34 / 33.09–52.70 / 71.25 |

The cold title's worst refresh included 2233.34 ms of foreground
shader compile and link time. Settled warm graphics commands took
20.50–21.02 ms per refresh versus 5.52–5.61 ms in guest JIT.
Graphics-command wall time can include driver waits. These windows
had no shader compilation or software vertex draws.

A scratch-buffer trial eliminated about 960 MB of resize zeroing
and clearing in a matched warm lab window. It retained 599 MB of
guest vertex copies. Median time moved from 29.20 to 31.17 ms,
p99 from 52.70 to 55.94 ms, and worst from 52.94 to 57.41 ms.
The trial leaves the existing staging policy in place.

The exact fragment binary prototype reused the same shader archive
for separable vertex and fragment stages. A warm boot restored 74
fragment binaries and issued zero fragment source links. Warm
title p99 moved from 133.74 to 133.42 ms. The worst warm title
refresh still took 477.71 ms with zero compile time, of which
255.16 ms was guest execution and 214.79 ms was graphics commands.
Warm settled medians stayed at 29.35–29.79 ms, and the cold title
p99 increased in this sample. The prototype stays on its test
branch. The Slide was restored to the screenshot-fix build with
the player's profile and original threaded presentation setting.

[Flicker-fix APK](https://github.com/dmikey/cara/actions/runs/45527665654) ·
[Fragment-cache prototype APK](https://github.com/dmikey/cara/actions/runs/45528887768)
