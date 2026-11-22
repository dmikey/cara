# Cara rendering on the RG Slide

The connected RG Slide reports **Arm Mali-G57, OpenGL ES 3.2**, with the Arm
r40p0 driver. Its display exposes 1280 × 960 modes at 60 and 120 Hz. These
measurements apply to that device and driver. A Mali-T820 needs its own test,
including a check of the GLES version and extensions the device exposes.

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

### Monster Hunter Stories

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
not establish sustained full speed on the RG Slide or any result on a T820.

[Arm framebuffer guidance](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-performance-2-how-to-correctly-handle-framebuffers)
