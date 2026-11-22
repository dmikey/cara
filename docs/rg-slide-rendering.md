# Cara rendering on the RG Slide

The Unisoc T820 in the connected RG Slide uses an **Arm Mali-G57** GPU.
The device reports **OpenGL ES 3.2** with the Arm r40p0 driver and exposes
1280 × 960 display modes at 60 and 120 Hz. These measurements apply to that
device and driver.

## Rendering and play settings

The Android ARM64 developer APK runs the shared GLES atlas renderer with
Dynarmic CPU JIT by default. FastInterp remains selectable. A native GLES 3.2
build is available for comparisons. The atlas and guest execution remain
separate choices, so renderer comparisons should use the same CPU backend.

- **Accuracy Mode** keeps the original guest CPU clock and the 100% speed
  limit. It retains the baseline guest completion policy and submits display
  work without the Performance Mode coalescing rule.
- **Performance Mode** uses the shared P3D and PPF completion policy, the
  adaptive guest CPU budget, and display coalescing. Coalescing uses the same
  display-ticket guard as iPad, driven by Android's Choreographer. Guest
  rendering and emulated time keep advancing. The fixed guest CPU clock
  overrides the adaptive clock when selected.
- The library and running settings use the same saved controls. Reopen the
  game after changing modes to apply the selected guest GPU completion policy.
- For a rendering comparison, start at 1× internal resolution, use JIT in
  both runs, and keep scaling, stereo, sound, and the display refresh mode
  constant.

Android's atlas uses the shared packed vertex input, upload, index and uniform
reuse, depth reinterpretation, and GPU surface remapping paths. The GLES 3.2
native lighting LUT option is capability-gated and disabled by default. It
uses buffer textures for lighting, fog, and procedural tables, with its own
shader binary cache. Its ring invalidates and republishes resident tables when
it wraps.

## Shared main build on the RG Slide

The Android APK used the source tree merged as `f0afef5`. It ran the shared
GLES atlas and Dynarmic JIT in Accuracy Mode at 1× with threaded presentation
and the original guest CPU clock. Logs confirmed that P3D pacing, the PPF
dependency policy, and the soft knee were disabled. Apple and Android share
the GLES renderer and guest timing core. Android ARM64 and macOS developer
builds can use Dynarmic. iOS runs FastInterp. Android and iPad drive shared
display coalescing from their native display callbacks.

Each result uses consecutive 120-refresh samples. The p99 is the nearest
rank within each sample and the maximum is an individual emulated system
refresh. Emulation refresh wall time includes limiter sleep. Scenes were
matched visually during normal play. Animated scenes were sampled with
comparable inputs and without deterministic replay.

| Warm scene | Native LUTs off p99 | Native LUTs on p99 | Worst off | Worst on |
| --- | ---: | ---: | ---: | ---: |
| Stories rendered forest opening | 114.13 ms | 78.28 ms | 115.17 ms | 82.73 ms |
| Stories first walk in Secret Path | 180.28 ms | 96.73–99.91 ms | 184.92 ms | 104.85 ms |
| Dark Moon first conversation in Gadd's lab | 67.96–71.28 ms | 71.84–73.43 ms | 73.45 ms | 81.26 ms |
| Revelations first ship corridor | 38.50–40.35 ms | 35.37–37.10 ms | 44.96 ms | 38.20 ms |

Stories' two representative walk windows each issued about 122,000 guest
draw requests. Lighting table work fell from 8,565.47 to 54.44 ms across
120 refreshes. Those warm windows recorded zero shader compilation and no
surface downloads. With native lighting on, the emulation still falls
short of the 16.71 ms target in this walk. Revelations averaged close to the
59.834 Hz emulation cap in the sampled stationary corridor with native
lighting, with tails over two refresh intervals. A separate 10 s Revelations
trace recorded 7.10 CPU seconds on NativeEmulation and 2.61 on the Mali
command backend thread. Scheduler sleep alone does not identify a GPU wait.

Dark Moon's four warm windows per arm had identical draw counts in
corresponding windows (83,062–83,760). After the lab's initial lighting
transition, table work was already small in both arms. Native lighting
slightly increased warm p99.

First-use shaders stayed expensive in both configurations. The worst
Stories forest refresh took 1,730.11 ms off and 1,769.14 ms on. The
Revelations corridor load hit 6,337.46 ms off and 6,236.81 ms on. Dark
Moon's initial title sequence hit 47,542.37 ms off and 47,494.78 ms on.
The title windows recorded 80.94 s and 80.63 s of shader driver wall time,
respectively. On entering Gadd's lab, the worst refresh was 41,119.64 ms
off and 39,895.49 ms on. The aggregate shader timing covers compile, link,
and cache operations. The timing does not identify the individual driver
call responsible for the tail.

A separate Stories boot in Performance Mode confirmed the shared P3D, PPF,
and soft-knee policies were enabled while Dynarmic and the atlas stayed
selected. Early prologue samples reported the guest clock at 75%, then
85%, 95%, and 100%. The sampled wall-clock refresh rate returned to about
59.834 Hz during this part of the prologue.

[Shared GLES and JIT merge](https://github.com/dmikey/cara/pull/84) ·
[Android timing instrumentation](https://github.com/dmikey/cara/pull/85)

## Mali-G57 queue and shader driver measurements

The same main APK ran Stories' first walk in Performance Mode with native
lighting buffers and the original GLES atlas/JIT pairing. It settled at
50% guest CPU clock. Five complete warm windows reached **54.64–55.35**
system refreshes/s, roughly **91–93%** of the 59.834 Hz cap. Per-window p99
was **26.00–36.48 ms** and the worst warm refresh was **45.48 ms**. A
separate **217.86 ms** refresh included **190.57 ms** of shader activity.
The performance policy changed the guest workload and reduced requests
from about 122,000 to 26,000 draws per 120 refreshes.

A separate diagnostic APK compared packed vertex fetch reuse in the same
walk, with native lighting and Performance Mode set in both runs. Timing
instrumentation adds overhead, so compare these rows directly.

| Packed fetch reuse | Mean system refreshes/s | Warm p99 range | Worst warm refresh |
| --- | ---: | ---: | ---: |
| Off | 51.13 | 27.48–38.75 ms | 48.66 ms |
| On | 54.02 | 26.33–31.26 ms | 40.60 ms |

Matched movement windows processed roughly 515,000 GPU vertex entries per
emulated refresh. Median texture acquisition wall time was 3.23 ms/refresh
off and 2.85 ms on. Vertex preparation was about 1.15 ms/refresh in both.
Timing includes driver work and waits within scopes. A repeat A/B on the
main APK showed a smaller mean gain, from 54.88 to 55.38 refreshes/s.
The paired p99 ranges were 25.65–34.53 and 25.59–30.43 ms, and the maxima
were 35.81 and 35.20 ms across seven warm windows each. The non-fragment
queue share fell from 59.4% to 53.4% of GPU-active cycles. Packed fetch
reuse remains an opt-in.

Arm hardware counters sampled simultaneously showed programmable cores
active during about 93% of GPU-active cycles in the walk. The fragment and
non-fragment queues accounted for 71.9% and 57.7% of GPU-active cycles
in the baseline. Those queues overlap. Median external bus read and
write stall rates were 4.5% and 6.1%. GPU and driver work are already
substantial in this scene. These device-wide counters use GPU-active
cycles as their denominator and omit time with the entire GPU idle.

Dark Moon's diagnostic cold title run reached a **47,472.74 ms** maximum.
That window recorded **70,753.93 ms** across 46 program links and status
checks, **10,422.59 ms** across 56 shader stage compiles, and **3.74 ms**
of binary extraction. The slowest individual link took **3,118.40 ms**.
A six-second CPU profile collected during that burst attributed 93% of
emulation thread samples to the Mali GLES library. On an immediate
relaunch of the same APK, all 47 saved programs restored in **30.93 ms**.
The matching title window still peaked at **2,196.57 ms**, with four
vertex stage compiles and zero links.

The stored title programs contain 47 pairs, 17 distinct vertex sources,
and 37 distinct fragment sources. Seven pairs can share a fragment source
when scissor mode is a uniform. An opt-in GLES probe tested that route on
the RG Slide. The cold title window dropped from 46 new links to 39 and
70.56 s to 60.12 s of link wall time. Its p99 moved from 33.49 s to 28.29 s
and the worst refresh from 47.32 s to 43.02 s. Both used separate empty
Cara program caches, with the uniform arm running first. First-start
screens offset the title window positions.

With both cold shader sets built, the stock title windows and a uniform
scissor relaunch had identical respective draw and vertex counts.
Warm medians rose from 97.37–97.91 ms to 102.11–102.51 ms, p99 from
101.64–108.27 ms to 106.91–111.59 ms, and the overall worst from 111.05 ms
to 117.01 ms. The uniform route stays outside the shipping defaults.

An independent packed-input probe reused the last integer texture fetch
between vertex attributes. A GPU decode fixture confirmed exact output
on the Mac. On the RG Slide's Stories first walk, packed fetch alone
averaged 54.42 refreshes/s across six full warm windows. The added rolling
cache averaged 50.79 refreshes/s. The per-window p99 ranges were
26.17–37.23 ms and 26.88–35.87 ms. Non-fragment active cycles increased
from about 23.8 million to 37.6 million per refresh despite similar
position and varying thread counts. The rolling cache stays experimental.

[Rolling fetch APK build](https://github.com/dmikey/cara/actions/runs/45075728414) ·
[Runtime scissor APK build](https://github.com/dmikey/cara/actions/runs/45075090315)

[Diagnostic APK build](https://github.com/dmikey/cara/actions/runs/45072818833) ·
[Hardware counter sampler build](https://github.com/dmikey/cara/actions/runs/45074322844) ·
[Arm Mali-G57 counter reference](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html)

## Dark Moon comparison with Citra MMJ

The installed MMJ build reports `3eb22034a`. Its saved global settings
request native resolution, hardware vertex shaders, CPU JIT and a 100%
speed cap. Cara used Accuracy Mode, JIT and 1×. Both Cara variants came
from the same diagnostic source tree and ran without the scissor probe.
An additional shipping-source atlas run confirmed the atlas result.
Each measurement below counts actual game-layer buffer presentation on
the RG Slide at 60 Hz after reaching the Dark Moon title.

| Installed build | Presents/s | Presentation p99 | Worst present interval |
| --- | ---: | ---: | ---: |
| Citra MMJ | 59.47 | 33.15 ms | 33.41 ms |
| Cara GLES atlas | 10.37 | 100.05 ms | 100.08 ms |
| Cara native GLES 3.2 | 8.24 | 149.95 ms | 150.01 ms |

The same-commit atlas at the device's 120 Hz panel mode measured 10.19
presents/s, 108.34 ms p99, and 108.35 ms maximum. The Cara title refresh
trace establishes the largest gles32 difference. Native GLES 3.2 spent
about 8.0 s per 120 emulated refreshes in surface download, with no such
time in the atlas windows. A native cold window completed 46 links in
28.54 s compared with 70.56 s on atlas. Faster linking alone did not
carry through to the warm title because of the downloads.

MMJ presents measure observed output cadence. Installed game overrides
and emulated system timing were not matched to Cara. A prior separate
comparison at Gadd's lab found that the shipped FMV and texture-copy
overrides changed refresh and game-submission rates. Public MMJ source
uses raw buffer uploads and preserves guest primitive topology. Cara's
native GLES 3.2 path also does that. The diagnostic data below show how
much the surface residency bug contributed to the gap.

### Native GLES surface residency repair

The Mali compiler rejected the shared remapper's native fragment shader
because its helper had no default float precision. Atlas supplies that
precision in its preamble. The native renderer fell through to the CPU
readback path for the tiled RGBA8 source aliased as a linear image. The
shared shader now supplies the GLES precision directly.

The same Dark Moon title windows on the RG Slide had matching draw counts,
1× Accuracy Mode, JIT, a 60 Hz panel, and threaded presentation.
Per 120 warm refreshes, native GLES went from **240 downloads and zero
GPU remaps** to **zero downloads and 720 GPU remaps**. Download wall time
fell from **8.10–8.15 s** to zero in those windows. Core p99 fell from
**124.72–129.44 ms** to **105.59–110.22 ms**, with maxima of 132.39 ms
and 113.12 ms. SurfaceFlinger presentation rose from **8.27 to 10.02**
presents/s, with p99 **149.92 to 116.66 ms**. The existing surface
coherence fixture passed on the patched device build.

The corrected native renderer reached 19.92 presents/s with Performance
Mode. The tuned atlas reached 27.12 with native lighting and packed fetch
enabled. Both settled at a 50% guest clock. Keep the atlas as the default.

After fixing residency, the warm draw scope still spent about 69–70 ms
per emulated refresh. A separate on-CPU sample attributed 35.6% of emulation
thread samples to Mali GLES. The guest execution scope includes graphics work
entered by guest callbacks, so its wall time cannot be attributed entirely
to Dynarmic. The Stories scheduler capture showed 5.66 of 6.43 emulation
thread CPU seconds on CPU 7, whose frequency policy allows up to 2.704 GHz.

[GLES precision fix](https://github.com/dmikey/cara/pull/89) ·
[RG Slide fixture and retest APK](https://github.com/dmikey/cara/actions/runs/45079489936)

[MMJ public draw path](https://github.com/weihuoya/citra/blob/302b0c3bc4312bc96489c11584a97664894b343f/src/video_core/renderer_opengl/gl_rasterizer.cpp#L329-L501) ·
[Prior MMJ Gadd lab investigation](https://github.com/dmikey/cara/blob/f41280d/docs/android-draw-processing.md#mmj-baseline-correction-and-live-transfer-probes)

## Physical device observations

### Resident Evil Revelations, November 22, 2026

The installed Android atlas APK was sampled in an early playable ship corridor.
The HUD showed 24 game FPS, 51 emulated refreshes/s and 86% speed in the
captured frame. The operating system reported thermal status 0 during this pass.

| Measurement | Observed |
| --- | ---: |
| SurfaceFlinger intervals | 125 |
| Mean present interval | 19.44 ms |
| Median present interval | 16.65 ms |
| p99 present interval | 33.35 ms |
| Worst present interval | 33.38 ms |
| Native emulation thread CPU time | 5.86 s |
| Native emulation thread sleeping | 3.20 s |
| Native emulation thread runnable | 0.06 s |
| Mali command backend worker CPU time | 2.41 s |
| Mali utility worker CPU time | 1.27 s |

Thread times are from the same approximately 9.1 seconds of scheduling
coverage. The emulation thread spent roughly 5.76 seconds on CPU IDs 6 and 7.
The system trace did not attribute the emulation thread's sleeps to specific
waits. SurfaceFlinger present intervals measure delivery to the display.
They do not establish guest frame time, GPU execution time, or the source of
the missed display intervals. The HUD numbers describe one live sample.

### Dark Moon, first-use shaders in Gadd's lab

On the RG Slide (Mali-G57 r40p0), a normal boot into a new profile used
Accuracy Mode, Dynarmic, the shared GLES atlas, and 1× resolution. The
diagnostic build restored the 47 title programs in 27.50 ms and reached
Gadd's lab with its lab programs still cold.

| 120-refresh window | p99 | Worst refresh | Program link and status | Stage compile and status |
| --- | ---: | ---: | ---: | ---: |
| Lab entrance, ending at 2161 | 161.47 ms | 41,016.35 ms at 2044 | 38.49 s / 21 calls | 2.35 s / 18 calls |
| Later lab transition, ending at 2281 | 177.70 ms | 29,763.88 ms at 2223 | 29.53 s / 16 calls | 0.13 s / 11 calls |

The two windows spent 96.5% of their combined shader-driver time in
program link and status calls. The longest individual link took 3.09 s.
Binary extraction across those windows took 3.61 ms.

Four subsequent warm windows (ending at 2401–2761) ran at
64.26–64.63 ms p50 and 68.30–70.54 ms p99, with a worst refresh of
73.85 ms at 2496. The scene issued 83,411–83,760 draws per 120 refreshes
and recorded zero program builds and zero surface downloads. Submit
calls took 0.89–0.93 s per 120 refreshes, while measured presentation
waits were below 1 ms per window.

A 20 s hardware capture during the conversation showed the non-fragment
queue active for 90.8% of GPU-active cycles, the fragment queue for 64.6%,
and the tiler for 79.5%. The stages can overlap. These device-wide
counters motivate an isolated vertex/tiler test alongside the lighting
work. [Mali-G57 counter definitions](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html)

A separate exact-source comparison found 56 new program pairs between
the title and the lab, with only five previously unseen vertex sources
and 37 previously unseen fragment sources. Forty-two of the added pairs
emit lighting in fragment main. Comparing the five most reused added
vertex sources found entry-point changes and, in one source, six extra
attribute decodes. Each source is about 75 KB. Sharing vertex source
alone still leaves the driver linking each exact vertex/fragment pair.

The atlas emits a reference guest vertex evaluation on the second and
third corners of expanded triangles to keep the interpolation quaternion
in the hemisphere of the first corner. Reducing that repeated work
requires preserving the first corner, attribute decoding, and guest
floating-point behavior. The fragment lighting ablation below also changes
vertex varying liveness and fragment coverage, so it cannot isolate
fragment ALU cost.

### Dark Moon, earlier RG Slide measurements

- **Surface residency.** In a warmed native GLES 3.2 title-screen A/B/A,
  remapping reduced downloads from roughly 33 per two-second window to zero.
  Throughput moved from 16.5 to 19.4 and 19.3 top-buffer swaps per window.
  The unscaled copy path also needed nearest filtering to preserve exact
  RGBA8 bytes around partial guest CPU writes.
- **Native lighting LUTs.** An earlier atlas title-screen probe reduced time
  attributed to LUT synchronization and raised the HUD from roughly 18% to
  22% speed. That run preceded the current renderer and did not establish
  exact checkpoint pixels, save-state behavior, or an equivalent gain in
  gameplay.
- **Packed vertex fetch reuse.** In Gadd's lab, a historical warm comparison
  moved from roughly 73.4 ms per refresh in controls to 68.5–68.6 ms with
  packed fetch reuse. Quaternion slicing alone moved to 72.46 ms. The
  packed-input verifier found no mismatch in that scene. These switches
  remain comparisons until coverage extends to other games and drivers.

[Surface residency investigation](https://github.com/dmikey/cara/commit/3337bd1)
· [Native LUT investigation](https://github.com/dmikey/cara/commit/5aee993)
· [Packed-fetch investigation](https://github.com/dmikey/cara/commit/2756155)

### Dark Moon, probes that stayed off

| Probe | RG Slide result |
| --- | --- |
| Threaded GL state reuse | 13.59 refreshes/s against 13.59 averaged controls |
| Depth encoding strength reduction | 77.04 ms/refresh against 72.78 ms averaged controls, despite an exhaustive match of all 16,777,216 encoding results |
| Program-owned uniform location caching | 69.19 ms/refresh against 68.90 ms averaged controls with packed fetch reuse enabled |
| Fragment lighting ablation | 48.53 ms/refresh against 83.86 ms averaged controls, with intentionally incorrect lighting, alpha, and coverage |

Lighting ablation points to lighting-related host work in that lab scene.
Matched traces had 700 draws per top swap in both arms, no guest geometry
shader draws, and no surface downloads. The result includes effects on
vertex shader linking, coverage, driver work, and implicit waits. Preserve
lighting and measure those costs independently. Earlier driver, submission,
copy, checkpoint, and shader probes are recorded in the
[archived draw processing investigation](https://github.com/dmikey/cara/blob/f41280d/docs/android-draw-processing.md).


The opening FMV, first walk toward the Secret Path, and a stationary dialogue
prompt are distinct workloads. An iPad gameplay trace previously attributed
6.00 ms per refresh to vertex preparation and 16.32 ms to vertex-upload GL
calls. Records showed the upload banks filling and falling back to orphan
uploads. These are wall-clock phase measurements from iPad. The equivalent
RG Slide cost is still unmeasured. See
[the earlier hardware investigation](hardware-investigation.md#monster-hunter-stories-current-leads).

## Reproducing and extending the measurements

1. Keep **Accuracy Mode, native resolution and JIT** for the first comparison.
   Record cold shader behavior separately from warmed play.
2. Start with Stories' FMV and first playable walk, Dark Moon's mansion gate
   and Gadd's lab, and the Revelations ship corridor. Prefer matched input
   or an identical checkpoint for repeated comparisons.
3. In a developer INI, `[Debug] trace_renderer_timing = true` enables grouped
   refresh samples. Each 120-refresh window reports p50, p99, worst refresh,
   applied guest clock, draw requests, vertex upload, submission, surface
   download, shader compilation, LUT work and mailbox fence wait times. The
   phase timings overlap and are not GPU hardware timestamps.
4. Compare `native_lut_buffers` and `gpu_packed_fetch_reuse` individually on
   the atlas, from fresh processes. Keep the cache state consistent. Check
   pixels, frame times, and save/load across the transitions before promoting
   a default. Compare Performance Mode independently with the same rendering
   path to attribute the difference to pacing.
5. Record repeated warm windows, audio behavior, thermal state, and panel
   refresh mode. If Android misses an interval, line up the Cara counters
   with SurfaceFlinger and a Perfetto scheduling capture.

The shared policy and presentation-guard self-tests passed. APK CI and the
physical gameplay comparisons are separate checks. The current evidence does
not establish sustained full speed across the tested RG Slide workloads.
Mali-G52 and Mali-T820 devices require separate tests.

[Arm framebuffer guidance](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-2-how-to-correctly-handle-framebuffers)
