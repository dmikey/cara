# Logitech G Cloud GLES baseline

The stock Android 11 G Cloud reports Adreno 618, OpenGL ES 3.2, and driver
V@0502.0. The Snapdragon 720G is a separate target from the RG Slide's
T820/Mali-G57. [Logitech specifications](https://support.logi.com/hc/en-in/articles/7712834867991-Specification-Logitech-G-CLOUD-Gaming-Handheld)

Cara main `5d9b6d9b`, native resolution, Dynarmic, and GLES atlas. The
library used a scoped Android folder grant. The controller tests used Android
gamepad key events and the touchscreen overlay. Guest clock was adaptive under
Performance Mode. The timings below are Cara refresh intervals in 120-refresh
windows.

| Workload | p99 | Longest refresh | Notes |
| --- | ---: | ---: | --- |
| New Super Mario Bros. 2, repeat boot welcome page, windows ending at 241–841 | 30.48–33.47 ms | 37.31 ms | CLK 100%, 3000 draws per window, zero compile time |
| A Link Between Worlds, initial opening, window ending at 601 | 703.14 ms | 11,986.70 ms | 12,387.04 ms total compile/link time in window |
| A Link Between Worlds, next window (721) | 20.40 ms | 26.20 ms | Zero compile time |
| Dark Moon, initial loading window, last complete window observed (241) | 104.15 ms | 108.61 ms | No additional timing window after update rate collapsed during this capture |

The failed trivial vertex shader used the GLSL extension support macro as a
guard for PICA clip planes. In GLSL ES, the macro can be present before
the built-ins are enabled. The regular atlas prefix omitted the extension
directive. The shared atlas patch emits the directive before declarations
where that extension is exposed, including the native-LUT variant. Azahar
similarly uses the extension capability for clip-distance support, and
PPSSPP keys its precision workaround to specific old Adreno models.
[Khronos extension](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_clip_cull_distance.txt)
[Azahar GLES driver](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_opengl/gl_driver.cpp)
[PPSSPP GLES capability checks](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLFeatures.cpp)

The alternate GLES 3.2 backend also stalled on Dark Moon. A three-second
sampling slice after the long pause attributed 25.3% of 336 CPU samples to
the Adreno GLES driver and 39.6% to Cara native code, on the emulation
thread. The driver is stripped, so those samples alone do not identify
a driver call. Mesa documents Adreno's GMEM and sysmem render paths.
Measure driver submission, attachment traffic, and shader compiler waits
on each device rather than extrapolating the Mali path from Adreno.
[Mesa Adreno architecture](https://docs.mesa3d.org/drivers/freedreno.html)
[Qualcomm profiling guidance](https://www.qualcomm.com/developer/blog/2025/08/optimize-performance-and-graphics-for-adreno-gpu-low-power-gaming)

## Driver behavior observed on the G Cloud

SurfaceFlinger's GLES extension list includes `GL_EXT_clip_cull_distance`,
`GL_EXT_buffer_storage`, `GL_EXT_texture_buffer`,
`GL_EXT_disjoint_timer_query`, and `GL_QCOM_tiled_rendering`. Its context
is missing `GL_KHR_parallel_shader_compile`. These are observations from
the stock driver and context, so probe availability in Cara's own context
before dispatching an optional entry point.

Qualcomm's tiling extension can control color/depth/stencil resolves.
Starting a tiled region can itself resolve previous rendering, so using
it as a per-draw hint could increase traffic. Only consider it over a
span of PICA draws where attachment load and store requirements are
known. Arm's framebuffer guidance covers compatible render-pass
grouping and attachment preservation for Mali.
[Qualcomm tiling specification](https://registry.khronos.org/OpenGL/extensions/QCOM/QCOM_tiled_rendering.txt)
[Arm framebuffer guidance](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-2-how-to-correctly-handle-framebuffers)

The clip correction `b05ad97f` enables a GLES language feature already
advertised by the driver. The Dark Moon trace confirmed successful
vertex compilation. A foreground program link took 10.22 s with the
background program link completing 12 ms later. One foreground fragment
compile took 4.67 s and completed 34 ms before a background link. The
long paired call intervals show contention is a plausible explanation.
Finer tracing would be needed to identify a vendor driver lock.
The first draws of those linked programs took tens of microseconds.

## Patched boot evidence

The Android production build of `b05ad97f` passed CI. It retained the
standard package and scoped library. The recorded title transitions
below use an empty Cara shader cache for the changed shader source ABI.
The stock driver cache may have contributed hits.

- Dark Moon reached the title after about three minutes. In the window
  ending at refresh 361, p99 was 61.07 s and the longest refresh was
  80.60 s. The title window at 481 still had p99 of 880.76 ms with
  no shader compilation on the emulation thread.
- Mario's first title transition had a 12.44 s maximum refresh, including
  12.30 s in foreground compile/link calls in the window. Its first
  gameplay opening cutscene hit another 7.10 s refresh and 7.00 s in
  foreground compiler calls. Two subsequent compile-free cutscene
  windows were near 60 FPS, with p99 of 20.48–23.46 ms at CLK 100%.

Mario also passed the world map into World 1-1. Its stationary warm map
ran at about 32 FPS and 53% speed with the adaptive guest clock at 50%.
Across five compile-free map windows, p99 was 34.00–36.79 ms and the
worst refresh was 40.58 ms. Earlier warm cutscene windows attributed about
6.1–6.8 ms per refresh to texture transfer for lighting/fog LUTs. On World
1-1, four windows were roughly 60 FPS with p99 of 24.99–27.06 ms and a
worst refresh of 27.39 ms as the guest clock returned toward 100%.

For cold performance, measure the shared worker against a path that avoids
building a second linked program when the identical-fragment fallback is
also cold. Separate context labels in shader traces show contention directly.
For warm performance, reuse the native LUT and attachment-lifetime probes
with a fixed guest clock and check color and depth against the same scene.
These are different experiments and need equivalent runs on each driver.

A Link Between Worlds reached the title, selected a new file, and reached
Gulley's first dialogue in Link's room. The patched first opening still had
a 12.81 s worst refresh and 686.54 ms p99 in the window ending at 601.
The immediate compile-free window had 19.80 ms p99 and 20.35 ms worst.
The transition into Link's room hit another 4.09 s refresh alongside
3.96 s of foreground compile/link. The following window at CLK 100%
had 19.26 ms p99 and 20.26 ms worst.

## Warm Adreno and Mali comparison

### Dark Moon smoky title

Adreno runs used `b05ad97f`, JIT + GLES Atlas, 1×, Performance Mode,
fixed CLK 100% and adaptive frame skipping disabled. Each run booted
normally and reused the Cara program cache. The two Adreno replacement
paths used the same fragments and preserved draw order. At refresh 481
all three runs had 43,776 PICA draw requests and zero measured compiler
time.

| Adreno title window ending at 481 | p50 | p99 | Worst | LUT texture transfer per refresh | Vertex upload per refresh | Presentation wait per refresh |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Partial updates | 152.61 ms | 297.58 ms | 302.53 ms | 56.19 ms | 28.04 ms | 13.41 ms |
| Whole image on the same object | 53.66 ms | 825.82 ms | 889.87 ms | 0.42 ms | 9.07 ms | 110.41 ms |
| Whole image on distinct objects | 50.00 ms | 832.30 ms | 853.04 ms | 0.45 ms | 8.89 ms | 114.35 ms |

The partial-update control changed phases as the title progressed. Windows
ending at 721 and 841 had 86.55 and 88.31 ms per refresh in vertex uploads
and 1.16 and 1.55 ms per refresh in LUT texture transfer. The draw path
updates sampled vertex atlas images with `glTexSubImage2D`. The observed
stall can surface there, in the LUT update, or at presentation as queued
work drains. The presentation timing wraps recycling a mailbox frame with
a present fence. These are wall time measurements at GL calls, not GPU
shader execution counters.

On the Mali-G57 RG Slide, the earlier smoky-title experiment found 1.90 s
asleep in LUT uploads in a three-second partial-update sample. Fresh
textures reduced sampled LUT sleep to 0.29 ms and improved window p99
from 44.79–47.86 to 39.05–39.67 ms. Adreno's same-title test shows the
tradeoff can reverse. The Slide results used an older APK, the ordered
GLES worker, and later title windows. Compare the effect of each policy
within its device run. [Mali-G57 title and lab measurements](rg-slide-mali-handoff/dark-moon-fog-lut-lifetime.md)

### Mario warm world map on Adreno

The comparison reloaded the same World 1 castle-map save state. It uses
four steady 120-refresh windows ending at 241 through 601 after the state
load, each with 36,000 PICA draws, fixed CLK 100%, and zero foreground
compilation.

| Mode | p99 range | Worst | Notes |
| --- | ---: | ---: | --- |
| Partial updates, matched reload | 33.71–35.12 ms | 35.86 ms | About 14.9 MB of LUT texture payload every 120 refreshes |
| Exact LUT-change tracking and defer unused LUTs | 34.49–35.36 ms | 35.75 ms | No consistent frame-time gain |
| Whole images on distinct objects | 44.49–57.32 ms | 70.81 ms | 220.2 MB of LUT texture payload every 120 refreshes |
| Native texture buffers | 65.63–70.85 ms | 77.32 ms | LUT time down from about 7.2 to 0.6 ms per refresh, presentation waits up to about 9.9 ms per refresh |

The ordered GLES worker also ran this map. Its first post-load window
took 175.79 ms at p50 and 651.68 ms at p99. Initial restore/compile work is
included in that result, so it is a restore and responsiveness diagnostic.
The worker owns millions of recorded GL calls and thousands of synchronous
calls by that point. Use shorter ordered batches and measure queue stalls
before testing a shipping default.

The foreground and worker cold Dark Moon traces linked 35 shared fragment
sources against different vertex stages. In the recorded interval the
foreground spent 115.09 s inside link/status calls, compared with 0.012 s
across 78 first draw submissions. Worker time overlaps that interval, so
adding foreground and worker timings would overstate wall time. The first
test branch only lets background vertex compilation trigger a CPU-vertex
fallback when that exact fallback program is already linked. Each mode
keeps the same guest PICA fragment program and drawing order.

Presenting inline on the emulation context tightened that same Adreno
Dark Moon window's tails. Partial updates gave 156.40 ms median, 171.22 ms
p99, and 189.05 ms worst. Whole-image replacement on one object gave
156.17 ms median, 164.27 ms p99, and 164.32 ms worst. Both completed the
same 43,776 draws. The inline path bypasses mailbox waits and changes the
stream-buffer policy. The near-identical wall throughput across the slow
title runs suggests the producer and presentation paths expose the same
underlying backlog with different pacing.

Cara also sizes its partial-update lighting atlas differently today.
The Mali path uses 525,312 bytes of stream capacity, and the generic
Adreno path uses 4 MiB. Test that allocation difference independently
of the driver on the same scene. A driver's new backing-store cost can
depend on allocation size while the reported uploaded byte count only
describes the updated region.

## Ready fallback trial

The follow-up build `0d7bae32` passed Android ARM64 CI and booted the
same Dark Moon image on the G Cloud at fixed CLK 100%. The GPU shader
path still uses the original PICA vertex program and fragment program.
It schedules background vertex-program creation only when the matching
software-vertex program already exists and can submit the draw immediately.

Cara rebuilt its program cache for this binary. The device driver cache
could contribute hits. In this run the first title checkpoint arrived
at 156 seconds. All 54 recorded links ran on the emulation context,
with 84.46 s total link/status wall time. The earlier run had at least
83 recorded links across both contexts and reached the title in about
three minutes with adaptive clock enabled. The comparison establishes
that the trial removed the extra background link work on this boot.
Clock policy and cache history can affect the elapsed difference.

| Trial window | p50 | p99 | Worst | Foreground compile/link |
| --- | ---: | ---: | ---: | ---: |
| Cold opening ending at 361 | 38.04 ms | 36.81 s | 84.73 s | 122.44 s |
| Smoky title ending at 481 | 155.35 ms | 304.02 ms | 312.75 ms | 0 ms |

The worst cold refresh remains severe. The warm title numbers are similar
to the earlier fixed-clock control. Testing the same source change on
Mali needs a matched normal boot on the RG Slide. [Trial build](https://github.com/dmikey/cara/actions/runs/45445245497)

## Vertex atlas update investigation

The normal-boot control on the G Cloud used JIT + GLES Atlas at native
resolution and fixed CLK 100%, with adaptive frame skip disabled. On
the window ending at 721, 51,206 draws took 156.92 ms at p50, 210.81 ms
at p99, and 225.73 ms at the maximum. Vertex upload averaged 89.84 ms
per refresh. The 841 sampled vertex subimage calls took 172.92 ms of
thread CPU in 176.00 ms wall time. The scope includes driver processing
and possible active waits. It cannot distinguish allocation, copying,
and spinning inside the driver.

The existing indexed route took 54.45 ms per refresh in uploads in
the same window, but presentation waits rose from 2.61 to 26.44 ms
per refresh. P99 reached 255.04 ms and the worst refresh reached
302.64 ms. With indexed draws and inline presentation, upload time fell
to 4.54 ms per refresh and p99 and worst were 161.31 and 161.85 ms.
The 120-refresh window still took 18.45 s, compared with 18.74 s in the
initial control. The inline path changes where the driver can throttle
the CPU, so the upload counter alone overstates the improvement.

The Adreno-only experiment `dd28a0a` uses a whole-image definition for
changed vertex and index textures. Exact cache hits still use the
resident image. It retains the existing texture pool and the draw
order, and uses GL's ordered storage lifetime. This lets the driver
discard the old image once in-flight draws release it. The diagnostic
requires an explicit flag and an Adreno renderer. [Arm dynamic resource update guidance](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-6-efficiently-updating-dynamic-resources)

The EGL fixture from the same revision passed on Adreno 618 and Mali-G57
for the existing upload-generation, depth/stencil, and packed-decoder
checks. It reported 40-bit Adreno and 64-bit Mali GPU timestamp and
elapsed-time counters in its test context.

### Whole-image retest on Adreno and Mali

The release APK `dd28a0a` used the same normal boot and shader cache for
both Adreno arms. JIT + GLES Atlas, 1×, fixed CLK 100%, disabled adaptive
frame skip and the threaded presentation mode were fixed. Both arms ran
the same draws with no source compile/link in either measured window.

| G Cloud smoky title | Command | PICA draws | p99 | Worst refresh | Vertex upload per refresh | Host window |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| End 721 | Partial image | 51,206 | 262.62 ms | 281.86 ms at 714 | 78.23 ms | 18.78 s |
| End 721 | Whole image | 51,206 | 360.11 ms | 376.48 ms at 714 | 73.76 ms | 18.89 s |
| End 841 | Partial image | 51,120 | 286.23 ms | 288.27 ms at 758 | 79.13 ms | 18.75 s |
| End 841 | Whole image | 51,120 | 280.26 ms | 294.37 ms at 746 | 75.57 ms | 18.74 s |

The same 841 sampled upload calls at 721 took 150.10 ms in the
Adreno partial-image control and 7.72 ms in the Mali control.
Whole-image definition reduced some Adreno upload time while the
host window duration stayed close and the worst refresh rose.
A CPU-vertex control had zero atlas-upload time, but the median of
the matched 43,776-draw window at 481 rose from 154.63 to 259.88 ms.
Guest output still requires the rendering work that an API change can
move elsewhere. The experiments leave the default upload policy in place.

The physical RG Slide ran the same source revision as a separate preview.
Both warmed normal boots used fixed CLK 100%. Requesting the
Adreno option kept Mali on the subimage path with the same per-window
draw counts.

| RG Slide title windows ending at 721 through 1081 | p99 range | Worst refresh | Steady host window |
| --- | ---: | ---: | ---: |
| Flag off | 57.88–59.16 ms | 64.32 ms at 658 | about 6.12 s |
| Flag requested | 58.07–59.06 ms | 62.23 ms at 1068 | about 6.12 s |

The preview and its ROM copy were removed after testing. The player
config hash matched before and after.
[Release build](https://github.com/dmikey/cara/actions/runs/45448880706)
[Preview build](https://github.com/dmikey/cara/actions/runs/45449160385)

### Early Mario and Zelda presentation controls

On Adreno, inline presentation narrowed Mario's three warm welcome-page
windows from 29.51–31.75 ms p99 and 33.49 ms worst to 21.37–24.46 ms p99
and 26.94 ms worst. Zelda's compile-free Triforce window ending at 721
went from 20.90 ms p99 and 21.79 ms worst to 19.40 ms p99 and 20.13 ms
worst. Later Zelda transitions still compiled on the foreground path.
These are opening checkpoints. The inline route also changes the stream
buffer policy and should be measured in gameplay before choosing defaults.

A normal Mario boot then continued through the map into World 1-1 with
threaded presentation. The map outside the castle showed 32 FPS and
53–54% speed at CLK 100%. During level entry, a newly encountered lit
fragment compiled in 7.79 ms, linked in 1,808.04 ms, and submitted its
first draw in 0.086 ms. The 120-refresh window ending at 11401 measured
232.79 ms p99 and a worst refresh of 2,084.32 ms at 11375. Compile and
link contributed 2,019.16 ms across that window.

At the start of gameplay, the ten compile-free windows ending from
11641 through 12721 showed 60 FPS and 100% speed on the HUD. Their
p99 was 24.17–27.12 ms, their maximum was 27.18 ms, and each window
took about 2.01 s. These timings cover the first section of the level.
The foreground link trace and the steady gameplay timings identify
separate problems. Android's current shared shader worker relies on a
ready, matching software-vertex program for this fallback, so the
Performance Mode default still permits synchronous links.

### Swap and GPU interval tracing

The opt-in GPU trace queries one in sixteen guest rendering intervals
with asynchronous result collection. Display composition and EGL swap
are outside the query. An elapsed interval can include GPU idle time
between commands. Swap wall times are measured separately.
[Khronos elapsed timer query specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_disjoint_timer_query.txt)

On the physical RG Slide, the compile-free title windows ending
841–1441 took 6.29–6.31 s each. Guest GPU interval medians were
44.94–45.94 ms (45 samples), while the mailbox swap medians stayed at
0.59–0.70 ms. Host p99 was 62.62–63.54 ms, with a worst refresh of
64.99 ms at 993. This instrumented run used a fresh installation and
includes query overhead. The independent warm controls above establish
the uninstrumented frame times. The fresh first encounter took another
84.91 s in the preceding 120-refresh window, including 79.22 s in
foreground shader work.

On the Adreno 618, the same GLES timer returned wrapped negative values
across the smoky title even though early boot values were plausible.
Discard those intervals. The follow-up diagnostic rejects wrapped
results, polls only when a result is pending, and gives swap timing
its own opt-in that issues zero GPU queries. In the instrumented
mailbox run, only 60–62 swaps completed per 120 guest refreshes in the
matched title windows. The swap median was 240.30–308.40 ms, even
as guest refreshes advanced about every 156 ms. Those waits are on
the Android presentation thread. An EGL swap duration includes driver
and compositor backpressure.

| G Cloud smoky title | Emulated refreshes | p99 | Worst refresh | Host window | EGL swap median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mailbox, end 721 | 120 | 234.02 ms | 259.69 ms at 680 | 18.721 s | 240.30 ms |
| Inline, end 721 | 120 | 165.80 ms | 168.58 ms at 711 | 18.863 s | 96.03 ms |
| Mailbox, end 841 | 120 | 193.23 ms | 224.58 ms at 725 | 18.622 s | 308.40 ms |
| Inline, end 841 | 120 | 166.50 ms | 167.06 ms at 735 | 18.983 s | 94.28 ms |

Both arms ran the same render workload and had zero measured warm source
compilation. Inline submitted one EGL swap per guest refresh and moved substantial
backpressure into the synchronous swap. The emulator's p99 improved
while overall progress stayed near 6.4 guest refreshes per second. The
runs establish where the waits surface. Valid Adreno GPU intervals
are still needed to distinguish GPU work from driver and compositor
latency. [Release trace build](https://github.com/dmikey/cara/actions/runs/45452025482)
[RG Slide trace build](https://github.com/dmikey/cara/actions/runs/45452032191)

### Mario warm normal-boot retest

After the first World 1-1 run, Cara restored all 98 saved programs as
driver binaries and did zero source links during restore. The second
normal boot entered World 1-1 at fixed CLK 100%. It performed no GL
source compile or link around level entry. The entry still had a
260.65 ms worst refresh at 7775, with 38.95 ms p99 in that window.
The next transition into gameplay had 94.54 ms p99 and a 226.87 ms
worst refresh at 7959. These windows include changing guest workloads
and do not align exactly with the initial 2.08 s hitch window.

The castle map and the first section of World 1-1 differed even with
warm shader programs. The values below are host emulation-thread
profiler wall time inside each scope divided by 120 guest refreshes.
The graphics scope can include driver waits. Its nested phases overlap.

| Scope | Castle map end 7441–7561 | World 1-1 end 8161–8521 |
| --- | ---: | ---: |
| Guest Dynarmic execution | 2.46–2.55 ms | 2.88–2.94 ms |
| PICA GPU commands and rendering | 23.65–23.75 ms | 8.74–8.93 ms |
| Draw path within graphics | 21.25–21.26 ms | — |
| Lighting/fog texture transfer inside graphics | 6.36–6.44 ms | <0.01 ms per refresh |
| Frame time p99 | 34.81–35.73 ms | 24.24–25.66 ms |
| Worst refresh | 36.38 ms at 7455 | 27.98 ms at 8316 |
| PICA draws per guest refresh | about 300 | 89–91 |

Each steady map window issued 32 LUT texture updates per refresh and
copied roughly 0.96 MB of guest vertex data while clearing or
initializing another 1.92 MB of staging data per refresh. The LUT
upload is one large measured contributor, while earlier Adreno
whole-image and native-buffer retests moved the wait or made the
scene slower. The frame time target for a 20% relative throughput
increase from a 31 ms median is about 25.8 ms.

On a second warm normal boot, the existing packed scratch-buffer
experiment reduced clear and initialization writes from about 1.92 MB
to 0.25 MB per refresh at exactly 36,000 PICA draw requests per window.
Vertex preparation dropped from 1.06 to about 0.90 ms per refresh.
Four stationary map windows still had 30.53–30.92 ms medians,
34.30–35.36 ms p99, and a 35.39 ms maximum. That removes work but
does not reach the throughput target.

The Apple-style raster-state and vertex-uniform reuse pair also ran
on Adreno. Five compile-free 36,000-draw map windows measured
30.67–31.05 ms at p50 and 34.19–36.19 ms at p99, with a 37.51 ms
maximum. The ranges overlap the control.

Linux scheduler counters from that run identify where the SoC ran
the emulation thread. During a 12 s steady map sample, the thread
was scheduled for 87.2% of wall time and runnable but waiting for
0.8%. All 51 last-CPU samples placed it on performance cores 6–7.
The sampled performance-cluster frequency was 2.323 GHz at the
median. In the first part of World 1-1, it ran for 78.8% of wall
time, waited runnable for 0.9%, and sampled the same cores and
median cluster frequency. These counters are scheduler observations,
including time in userspace driver calls. An EGL/GL timing scope can
include CPU execution, driver synchronization and already queued GPU
work. The CPU placement and clock observations give no indication of
an efficiency-core scheduling issue in these samples.
