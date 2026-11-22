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
