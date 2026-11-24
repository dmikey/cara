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
