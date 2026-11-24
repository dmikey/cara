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

### Adreno separable-stage check

The Adreno-only opt-in reused the existing GLES 3.2 stage pipeline. The
release built in CI and ran from a normal boot into Mario's opening
cutscene. The no-Cara-stage-cache cold startup had a 6,803.98 ms
worst refresh. Two standalone vertex programs took 2,252.64 and
2,480.83 ms to link. The driver cache was left in its existing state.

A compile-free cutscene window of 12,240 draws measured 48.47 ms p50,
52.09 ms p99, and 52.41 ms worst with stage pipelines. Linked-GLES
windows of the same draw count in that cutscene measured
22.24–22.55 ms p50 and 25.29–25.84 ms p99, with a 26.45 ms maximum.
The scenes were aligned by cutscene and workload, rather than
pixel-exact replay. In the stage window, the accumulated LUT time
was 20.29 ms per refresh compared with 7.29–7.80 ms on the linked
route. This is a driver-path timing, and can include waiting behind
earlier work. Stage pipelines stay off on Adreno. The comparison did
not proceed past the cutscene because the warm regression was already
large. [Adreno stage and swap-only build](https://github.com/dmikey/cara/actions/runs/45454216365)

The linked World 1-1 entry capture saw nine fragment keys and nine
distinct GLSL sources, three of them lit. Source-string deduplication
alone cannot remove the expensive link in that capture. The separate
Zelda opening sample saw 48 keys and 32 distinct sources, making
cross-key reuse more relevant there. These counts cover the logged
scenes and omit earlier cache hits.

For the G Cloud smoky title, a final normal boot on the same release
enabled only the EGL swap probe. No guest GPU interval queries ran.
The window ending at 721 took 18.674 s for 120 guest refreshes
and 51,206 draws, with 241.00 ms p99 and 299.08 ms worst at 629.
The window ending at 841 took 18.732 s and 51,120 draws, with
244.90 ms p99 and 253.18 ms worst at 760. Neither window compiled
shaders. Mailbox swap medians were 237.31 and 307.64 ms, close to
the earlier query-enabled capture. This confirms long presentation
waits even without timer queries. It does not identify a GPU clock
or isolate the cost of a PICA draw on Adreno.

## Stock Vulkan capabilities on the two Android handhelds

Physical device enumeration through `adb shell cmd gpu vkjson`
reported the following driver capabilities.

| Handheld | Stock Vulkan device | Device API | Pipeline creation feedback and cache control | Timeline semaphores and synchronization2 |
| --- | --- | --- | --- | --- |
| Logitech G Cloud | Adreno 618, Qualcomm 2021-11-30 driver | 1.1.128 | Advertised | No corresponding extensions advertised |
| RG Slide | Mali-G57, Arm r40p0 | 1.3.225 | Advertised | Advertised |

Both expose a graphics queue family with valid timestamp bits. The
G Cloud advertises 48 bits and the Slide advertises 64 bits. Capability
enumeration does not establish timestamp accuracy or game performance.
A shared renderer baseline can use Vulkan 1.1 render passes, binary
semaphores and fences. Optional synchronization and pipeline features
should be queried on the device before use.

The G Cloud Mario map spends 23.7 ms per guest refresh in PICA graphics
and driver work, including about 6.4 ms in lighting and fog transfers.
Vulkan could move submission off that critical path, upload changed
ranges to explicitly owned storage, and reduce stalls when prior draws
still use those tables. This number includes PICA translation and
driver waits, so it cannot estimate the Vulkan speedup on its own.
The RG Slide's approximately 45 ms sampled GPU command intervals
give another test target. Keeping attachments tile-local and avoiding
barriers that force earlier work to drain are plausible improvements.
An equivalent PICA workload and pixel comparison are required to
distinguish a lower renderer cost from different guest work.
[Arm Vulkan mobile guidance](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/vulkan-mobile-best-practices-and-management)
[Khronos threaded recording sample](https://docs.vulkan.org/samples/latest/samples/performance/multithreading_render_passes/README.html)

Vulkan still needs pipelines for new guest shader and fixed-function
state combinations. Azahar's Android defaults and Vulkan shader cache
are useful prior art. Compare shader creation, frame time p99, worst
refresh and sustained speed on the same scenes, with fresh and seeded
caches separated. Cara Android currently ships GLES.
[Azahar Android defaults](https://github.com/azahar-emu/azahar/blob/master/src/common/settings.h)
[Azahar Vulkan shader cache](https://github.com/azahar-emu/azahar/discussions/1942)

## Mario default pacing versus fixed guest clock

The same installed GLES build ran the opening and World 1-1 from two
normal boots with seeded shader programs. One used stock Performance
Mode pacing with Adaptive Frame Skip available. The fixed-clock arm
locked CLK at 100% and disabled Adaptive Frame Skip. Each timing range
covers complete 120-refresh windows selected by the same draw count.

| Warm scene | Arm | Emulated CLK | p50 | p99 | Worst | Median host window |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Lit opening, 12,240 draws | Fixed | 100% | 21.98–22.48 ms | 25.05–30.35 ms | 32.30 ms at 2936 | 2.707 s |
| Lit opening, 12,240 draws | Stock pacing | 50% | 21.98–22.38 ms | 25.50–28.24 ms | 30.50 ms at 3111 | 2.692 s |
| Stationary map, 36,000 draws | Fixed | 100% | 30.75–30.92 ms | 34.25–36.96 ms | 40.20 ms at 3932 | 3.712 s |
| Stationary map, 36,000 draws | Stock pacing | 50% | 30.74–30.97 ms | 33.92–35.30 ms | 36.60 ms at 4332 | 3.707 s |

The observed stock pacing did little to sustained throughput in the
graphics-heavy scenes. It returned the guest clock to 100% in the first
part of World 1-1, where the HUD reported 60 FPS and 100% speed.
Build the timing comparison around both the lit cutscene and the map.
At a 60 Hz guest refresh target, 80% throughput corresponds to about
20.8 ms per refresh, leaving about 4.2 ms to recover on the critical path.

The opt-in draw framebuffer cache reused 23,520 FBO acquisitions in
each steady 36,000-draw window without constructing additional FBOs
in those windows. The eight measured map windows still took 3.732 s
at the median, with p50 of 30.96–31.15 ms, p99 of 34.59–36.19 ms,
and a worst refresh of 44.35 ms. The three matched lit opening
windows had 22.39–22.71 ms p50 and took 2.739 s at the median.
Framebuffer reuse stays a diagnostic on this path.

## Emulator prior art for the Adreno critical path

- **PPSSPP GLES** records ordered render steps on the emulation side and
  executes them on a GL owner thread. The owner also performs the swap.
  It bounds frame slots, records the presentation point, and uses an
  explicit synchronous flush for CPU-visible framebuffer reads. This
  suggests testing a single GL owner for both draw submission and
  presentation in Cara. The existing inline mode is a diagnostic control
  for the split-context handoff. It runs on the emulation thread, so it
  cannot establish how much a batched render worker would overlap.
  [PPSSPP GLES render manager](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLRenderManager.cpp#L264-L278)
  [PPSSPP swap ownership](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLRenderManager.cpp#L364-L439)
- **Dolphin** uses a generic shader for a draw while the specialized
  shader builds, then switches to the specialized path. This addresses
  first-use stalls. On old mobile GPUs, measure the fallback's cost as
  well as the compiler time it removes. Cara's World 1-1 link took
  1.81 s, while the shader's first submitted draw took under 0.1 ms.
  A narrow fallback that preserves the fragment result would address
  that link. It needs measured coverage of the vertex/fragment pair
  and a correct fallback for the draw; a cache only helps after that
  first encounter. [Dolphin performance guide](https://dolphin-emu.org/docs/guides/performance-guide/#shader-compilation)
- **AetherSX2's PCSX2 lineage and the open mobile ARMSX2 work** give
  distinct precedents. The public PCSX2 Vulkan renderer owns frame
  resources together, uses a separate initialization command buffer
  for transfers, and tracks retirement with submission fences. ARMSX2's
  GS replay tool can measure CPU preparation on a deviceless backend
  with a selected device capability profile and can vary upload ring
  behavior for an A/B. Its Adreno 650 profile and newer Mali profiles
  illustrate the probe design. They provide no performance result for
  the G Cloud's stock Adreno 618 or the Slide's G57. AetherSX2's
  official FAQ and archive did not provide readable implementation
  details during this review. [PCSX2 Vulkan implementation](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp)
  [ARMSX2 renderer replay controls](https://github.com/ARMSX2/ARMSX2/blob/master/pcsx2-gsrunner/Main.cpp#L1184-L1213)
- **Adreno attachments.** Mesa describes both GMEM tiling and direct
  system-memory rendering on Adreno. The G Cloud's compositor GLES
  context advertises Qualcomm's tiled-rendering extension. The
  extension can make attachment loads and stores explicit for a
  proven region, and starting a region can itself resolve earlier
  work. Reusing an FBO handle left Mario's frame time near 31 ms,
  so a tiling experiment needs to count actual attachment traffic
  and identify guest reads, feedback and transfers at each boundary.
  [Mesa Adreno hardware notes](https://docs.mesa3d.org/drivers/freedreno.html#hardware-architecture)
  [Qualcomm tiled-rendering specification](https://registry.khronos.org/OpenGL/extensions/QCOM/QCOM_tiled_rendering.txt)

## Exact table residency on GLES Atlas

Build `ec7c1df` keeps eight byte-verified versions of each lighting
table and the fog table in the existing 2D upload ring. A draw with
identical table data can reuse the resident offset. Ring wrap clears
all historical offsets and republishes every current table. The probe
uses the existing GPU texture and shader addressing. Native texture
buffers and whole-image replacement continue through their own paths.
The host check under ASan/UBSan covered A→B→A, per-table separation,
eviction, zero offset, and clearing history before reusing wrapped
storage. [Android release build](https://github.com/dmikey/cara/actions/runs/45457501555)

The G Cloud ran normal boots on the same signed APK, JIT, native 1×,
fixed CLK 100%, Adaptive Frame Skip off, stock Adreno driver, and the
same 99 restored shader binaries. The table compares stationary
windows by PICA draw count and excludes shader creation.

| Mario scene | Mailbox mode | Draws per window | Table uploads per refresh | Median host window | p99 range | Worst |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Lit opening | Partial uploads | 12,240 | 38 | 2.713 s | 25.16–27.71 ms | 29.83 ms at 2851 |
| Lit opening | Exact table reuse | 12,240 | 0 | 4.844 s | 108.97–111.82 ms | 112.01 ms at 2780 |
| Castle map | Partial uploads | 36,000 | 32 | 3.732 s | 35.08–35.71 ms | 40.98 ms at 4431 |
| Castle map | Exact table reuse | 36,000 | 0 | 4.137 s | 48.19–79.68 ms | 80.44 ms at 3708 |

In the opening windows, graphics scope time fell from 14.73 to
11.23 ms per refresh and LUT scope time fell from 7.80 to 0.25 ms.
The emulation thread then waited about 22–23 ms per refresh on the
mailbox's presentation fence. The separate EGL context finished
60 swaps per window, with median swap durations of 76.7–78.8 ms.
Overall guest speed regressed. A lower frame median in the residency
arm hid this behavior because long stalls were concentrated in a
subset of refreshes.

On the RG Slide, the same source revision ran in its isolated preview.
Both steady title boots restored 47/47 program binaries and had zero
shader creation in the timing windows. The four windows below each
issued exactly 51,120 draws.

| Dark Moon smoky title | Median range | p99 range | Worst | 120-refresh window |
| --- | ---: | ---: | ---: | ---: |
| Partial uploads | 50.84–53.31 ms | 58.11–58.90 ms | 62.53 ms | 6.139–6.178 s |
| Exact table reuse | 36.03–36.52 ms | 39.76–40.35 ms | 42.15 ms | 4.345–4.366 s |

The measured throughput rose from about 19.5 to 27.6 guest refreshes
per second. Table upload calls fell from 480 to zero per window.
A repeat partial-update boot reproduced 6.09–6.13 seconds across
eight matched windows. The available captures preserve the title,
smoke and controls visually across animated frames. Those captures
have no matched-refresh pixel oracle.
[RG Slide preview build](https://github.com/dmikey/cara/actions/runs/45458212650)

The inline swap comparison used the same APK and shader archive. The
inline path has zero mailbox recycling waits. It also uses a different
uniform/vertex stream-buffer policy, so it changes more than swap
ownership.

| Mario scene | Inline mode | Median host window | p99 range | Worst |
| --- | --- | ---: | ---: | ---: |
| Lit opening, 12,240 draws | Partial uploads | 2.640 s | 23.55–24.73 ms | 36.53 ms at 2973 |
| Lit opening, 12,240 draws | Exact table reuse | 4.951 s | 53.90–59.53 ms | 67.43 ms at 2913 |
| Castle map, 36,000 draws | Partial uploads | 3.615 s | 32.22–33.19 ms | 33.67 ms at 3824 |
| Castle map, 36,000 draws | Exact table reuse | 4.071 s | 35.94–38.69 ms | 43.74 ms at 3917 |

Inline reduced the worst exact-reuse tail compared with the same
probe under the mailbox. It still took substantially longer than
partial uploads in the same inline path. The remaining cost is in
the rendering/submission path and EGL swap together. Adreno's invalid
elapsed-query results prevent attributing it to GPU execution alone.
With partial uploads, inline gives about 3% higher guest throughput
in the sampled opening and map. The opening peak is worse, so the
test does not qualify inline as the default. Table reuse stays
opt-in pending wider validation and is unsuitable for this Adreno
driver in the tested scenes.

A second title run with exact reuse repeated 4.410–4.421 seconds and
39.90–41.23 ms p99. The preceding control repeated 6.102–6.126 seconds
with exactly the same 51,120 draws and 480 partial updates per window.

The lab comparison booted both arms through empty preview slots to
the first E. Gadd cinematic. Five warm windows in each arm had zero
shader creation. Their median ranges overlapped. The exact 42,229-draw
comparison produced 3.968 and 3.989 s control windows versus 3.995 s
with reuse. Both paths issued zero LUT uploads in that exact
comparison. Control p99 was 39.03–39.64 ms and the candidate p99 was
42.31 ms. Across the five windows, the control worst was 56.26 ms
and the candidate worst was 58.38 ms. The exact reuse win is confirmed
for the title. Warm Gadd leaves its draw and submission workload as
the optimization target.

The first lab control seeded shader programs from a 47-entry archive.
Its longest first-use refresh was 40.55 s. The second boot restored
120 programs, so this does not compare cold lab-entry performance.
Both lab screenshots show Gadd, room lighting, and translucent ghosts.
Camera and animation ticks differ. The preview was removed and the
player's configuration hash matched after the original app reopened.

## One render thread for draw submission and swap

Cara already has a bounded ordered GLES dispatch queue. The new Android
route transfers the window EGL context to that queue's worker. The core
records the game and composition commands, and each swap follows its
composition commands on the same FIFO. The Android UI display callback
continues to supply the Adaptive Frame Skip ticket. It never binds the
window context in this mode. The renderer retains each native window
until prior swaps retire, and keeps its context on a pbuffer while no
window is available.

The route is selected by this existing pair of debug options. The
third setting caches the packed layout uniform location and values so
the worker comparison does not repeat the same synchronous program
query for every packed draw.

```ini
[Debug]
ordered_gles_worker = True
use_present_thread = False
packed_layout_uniform_reuse = True
```

Compare direct inline and worker inline with the same packed layout
setting. Stock production keeps the mailbox and its direct GLES path.
Use the same APK, restored shader programs, fixed CLK 100%, 1×, and
normal boots. Report total elapsed time over each 120-refresh window
alongside p99 and worst, then trace the actual swap owner thread.
[PPSSPP GL worker and present order](https://github.com/hrydgard/ppsspp/blob/master/Common/GPU/OpenGL/GLRenderManager.cpp#L364-L439)
[Android release build](https://github.com/dmikey/cara/actions/runs/45464327707)

Before testing the new swap owner, G Cloud preflights used the prior
`ec7c1df` release with the existing ordered worker and split-context
mailbox. In the three identical 12,240-draw, compile-free opening
windows, caching the layout uniform eliminated 12,240 uniform-location
RPCs per window. Other synchronous calls stayed at 360. Median time
for 120 refreshes fell from 9.522 s to 2.716 s. P99 fell from
174.66–184.67 ms to 25.29–27.06 ms and the worst fell from 205.82 ms
to 27.37 ms. The uncached preflight stopped after the opening. The
cached run continued through the map and into World 1-1.

On the physical G Cloud, build `238b8f0` logged 6,308 successful
swaps on the CaraGLES worker during the first normal Mario boot. The
NativeEmulation thread was separate. Both modes restored 99 program
binaries, and neither linked shaders in the draw-matched timed windows.
The worker's castle-map windows executed about 2.08 million recorded
GL calls per 120 guest refreshes despite zero synchronous GL queries.
Its median worker thread CPU time in that scene was 17.62 ms per
refresh, excluding the producer. The worker and producer can overlap.

| Warm Mario scene | Swap route | Median elapsed for 120 refreshes | p99 | Worst |
| --- | --- | ---: | ---: | ---: |
| Lit opening, 12,240 draws | Direct inline, cached layout | 2.653 s | 23.83–24.45 ms | 32.71 ms |
| Lit opening, 12,240 draws | Queued worker, cached layout | 2.660 s | 24.94–29.20 ms | 40.98 ms |
| Castle map, 36,000 draws | Direct inline, cached layout | 3.620 s | 32.24–33.39 ms | 33.94 ms |
| Castle map, 36,000 draws | Queued worker, cached layout | 3.620 s | 34.43–40.75 ms | 50.06 ms |
| World 1-1 opening, similar draw counts | Direct inline, cached layout | 2.005 s | 20.09–22.48 ms | 30.21 ms |
| World 1-1 opening, similar draw counts | Queued worker, cached layout | 2.004 s | 23.37–35.09 ms | 42.22 ms |

The transition into the map had a 523.31 ms maximum refresh on the
worker path and 126.19 ms on the direct inline path. The transition
windows have different draw counts. No shader compile was measured
in either peak window. Compare these transition observations separately
from the exact-draw stationary windows. The tables report host frame time. Queue profiling also drains outstanding work
at each 120-refresh boundary.

A third normal boot on the same APK used the stock direct mailbox
renderer, with the timing clock fixed at 100%. Its matched lit
opening took 2.712 s per 120 refreshes, with p99 of 24.81–25.64 ms
and a worst of 26.39 ms. The castle map took 3.708 s per window,
with p99 of 34.68–35.30 ms and a worst of 36.17 ms.
The single worker's 2% throughput margin versus this stock route
also appears with direct inline presentation. The isolated inline
comparison gives no sustained gain from moving GL commands to the
worker and shows heavier frame-time tails.

With raster-state reuse enabled on the same worker, the steady map
recorded 780,120 commands per window, down from 2,075,280, and worker
CPU time fell from 17.62 to 16.23 ms per refresh. Map throughput stayed
at 3.619 s per window. P99 ranged from 32.78 to 34.75 ms with a
65.69 ms worst. The lit opening still took 2.660 s. That arm had
25.28–66.19 ms p99 with an 80.54 ms worst. A number of measured peaks
fell immediately after a 120-refresh reporting boundary. The
measurement drains the worker at those boundaries. The larger first
worker arm's scene-transition hitch occurred within a window.

The lifecycle check left World 1-1 for Android Home. Starting the
library component opened a second activity rather than resuming the
game. Android Back returned to the existing game. The same CaraGLES
thread resumed successful swaps and the World 1-1 HUD read 60 FPS,
100% speed and CLK 100%. Normal settings were restored afterward.
The one-worker route stays opt-in.

The parallel Slide experiment isolated direct-GLES indexed draws
against arrays in the first E. Gadd lab cinematic. Its exact
42,229-draw match improved from 30.5–30.7 to 34.2 guest refreshes/s.
P99 went from 39.07–40.42 ms to 34.27 ms and the worst from
63.72–78.34 ms to 51.94 ms. The longer captures had variable tails
and the candidate boot reused more shaders. The Slide player
installation and configuration were restored.
[RG Slide direct GLES retest](https://github.com/dmikey/cara/blob/dmikey/mali-gadd-draw-followup/docs/rg-slide-mali-handoff/dark-moon-entry327.md#direct-gles-retest-with-exact-table-residency)

## Indexed draws on stock Adreno 618

The signed `238b8f0a` build ran on the G Cloud from the title screen
through New Game, the stationary castle map, and World 1-1. Both paths
used JIT, GLES Atlas, Performance Mode, 1×, fixed CLK 100%, direct draw
submission with mailbox presentation, and Adaptive Frame Skip off.
Each eligible timing window contains 120 guest refreshes. The
stationary map has exactly 36,000 PICA draws per window and the lit
opening has 12,240. The following ready windows have zero foreground
shader creation, zero selected draws waiting for their indexed program,
and no logged background shader compile or link overlapping the window.
The first arrays boot and warm indexed repeat both restored 128 of 128
shader binaries. The first indexed boot restored 99 of 99.

| Scene and route | Windows | Median host time per window | p99 range | Worst refresh |
| --- | ---: | ---: | ---: | ---: |
| Lit opening, arrays | 4 | 2.712 s | 25.21–26.09 ms | 33.63 ms at 2816 |
| Lit opening, indexed repeat | 3 | 2.204 s | 20.53–21.75 ms | 29.75 ms at 2655 |
| Castle map, arrays | 8 | 3.720 s | 34.52–35.65 ms | 37.45 ms at 4547 |
| Castle map, indexed repeat | 13 | 4.039 s | 37.33–38.71 ms | 42.53 ms at 5027 |
| Castle map, first indexed use during background builds | 4 | 4.498 s | 41.25–245.35 ms | 391.75 ms at 3807 |
| World 1-1 opening, arrays | 7 | 2.006 s | 23.84–27.11 ms | 29.86 ms at 5336 |
| World 1-1 opening, indexed repeat | 7 | 2.006 s | 24.51–27.54 ms | 48.92 ms at 5645 |

The ready map went from 54% to 50% of the paced World 1-1 baseline.
The opening went from 74% to 91%. The first indexed boot also improved
the opening, to 2.262 s per window, 21.32–22.80 ms p99, and a 23.84 ms
worst refresh. At the map, all 19,440 selected indexed draws executed
with their ready programs on the second indexed boot. Confirmed
early World 1-1 gameplay reached full speed on both routes. The
World 1-1 windows use similar draw counts and are separate from the
exact-count map and cutscene comparisons.

On the map, the indexed draws were lit
draws. The unlit index route had no eligible draws there. In the
opening 7,200 lit and 3,360 unlit draws used the indexed routes.

The warmed map used 23.19 ms of emulation-thread CPU time per
refresh for PICA and GLES with arrays and 24.93 ms indexed, versus
2.48 and 2.55 ms to run the guest CPU. In the arrays arm the
lighting/fog transfer API accounted for
6.97 ms per refresh inside the graphics scope. The map changes 32
tables on average and transfers about 124 KB per refresh in both
arms. Draw submission took 4.14 ms, uniform work 2.15 ms, GL state
application 1.68 ms, and vertex upload 1.20 ms. The corresponding
indexed timings were 7.95 ms for table transfers, 4.54 ms for draw
submission, 2.21 ms for uniforms, 1.76 ms for GL state, and 1.23 ms
for uploads. These API timings are host wall scopes and some are
nested. An API call
can include waits on previously submitted GPU work, so its duration
does not measure the isolated execution cost of that operation.
In a separate 11.98 s sample on the indexed map, the emulation
thread was on CPU 86.7% of the time and runnable but unscheduled
for 0.9%. It ran on cores 6 and 7 with a median big-core policy
frequency of 2.323 GHz.

Leave indexed draws opt-in on Adreno. The opening and map respond
differently to the same setting, and the earlier exact LUT-residency
test on this driver removed transfers at the cost of longer
presentation waits.

### Paired GPU timestamps

On `d3eb4472`, the paired timestamp probe samples every 16 guest
refreshes. It polls existing query pairs for availability and rejects
disjoint data and intervals longer than the corresponding host
submission-to-result window. A host fixture exercised asynchronous
availability, 40-bit wrap, a reversed timestamp, disjoint handling,
and the original elapsed query route under ASan and UBSan.
[Khronos timestamp query semantics](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_disjoint_timer_query.txt)
[Android release build](https://github.com/dmikey/cara/actions/runs/45468629340)

The arrays map repeated at 3.714 s per 120 refreshes with this probe,
versus 3.720 s without GPU queries. Five matching windows gave
34.08–35.86 ms host p99 and a 36.97 ms worst. The GPU timestamp
interval had a 30.13 ms median (37 samples, 28.68–31.79 ms). The
same boot measured 22.12 ms in the lit opening (23 samples) and
9.02 ms in early World 1-1 (52 samples). The span includes any idle
time between the timestamp markers. It does not report GPU busy
time or the duration of individual shader invocations. The stock
device gave no KGSL ftrace packets through Perfetto with GPU
frequency, busy, and batch tracepoints requested.

| Timestamp retest | Route | 120-refresh median wall time | Host p99 | Worst host refresh | Median GPU marker span |
| --- | --- | ---: | ---: | ---: | ---: |
| Lit opening | Arrays | 2.709 s | 25.16–25.60 ms | 27.61 ms | 22.12 ms (23 samples) |
| Lit opening | Indexed | 2.201 s | 20.27–23.62 ms | 25.39 ms | 17.98 ms (23 samples) |
| Stationary map | Arrays | 3.714 s | 34.08–35.86 ms | 36.97 ms | 30.13 ms (37 samples) |
| Stationary map | Indexed | 4.012 s | 37.83–39.68 ms | 39.76 ms | 32.99 ms (60 samples) |

The paired probes reproduced the map regression and the opening
improvement. The indexed capture reached the castle map and collected
eight timing windows. Host disk exhaustion stopped that capture as the
harness moved to World 1-1. The earlier no-timestamp indexed repeat
already captured the World 1-1 comparison.

The same build tested the existing persistent coherent uniform stream
on a normal arrays boot. The map performed 36,000 persistent writes
and zero uniform-page waits per matched window. Uniform wall time
fell from 2.06 to 1.09 ms per refresh, and PICA/GLES thread CPU fell
from 23.68 to 22.24 ms per refresh. Map throughput stayed near 54%.
The eight matched windows took a median 3.699 s per 120 refreshes
with 34.70–35.79 ms p99 and a 36.28 ms worst. The GPU marker median
was 30.54 ms across 60 samples. The normal boot reached 100% in
early World 1-1 with the probe enabled. The existing default keeps
persistent uniforms off.

The G Cloud is on the signed `d3eb4472` APK. Its temporary profiling
configuration was byte-checked before removal, and Cara reopened
to its library with normal settings.

[RG Slide cold and warm indexed matrix](https://github.com/dmikey/cara/blob/1dc411b230dc04ac97bc821466b71af1ad5a9f18/docs/rg-slide-mali-handoff/dark-moon-entry327.md#cold-archive-and-warm-replay-matrix)
