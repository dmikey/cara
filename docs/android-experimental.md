# Experimental Android GLES build

The Android Actions workflow builds a debug-signed `arm64-v8a` APK with Dynarmic
CPU JIT and Cara's GLES atlas pipeline. It targets testing on the RG Slide
(T820, Android 13). This is an experimental port, not an established compatibility
or performance result. Apple builds continue to force CPU JIT off.

Download the APK from the `cara-android-arm64-gles-dynarmic-experimental` Actions
artifact. Enable installation from your downloader/file manager and open the APK.
The application ID is `org.cara.emu.debug`. The test package targets Android 13
(API 33) and uses the system document picker for game-folder access. Legacy broad
storage permissions are limited to older Android versions. Save in-game and back up important
progress before changing builds. GitHub's temporary debug signing key can change
between builds; reinstalling may require uninstalling the previous test APK.

The dependency fetch is explicit and pinned: `python3 tools/prepare_android_dynarmic.py`.
The inherited MMJ Dynarmic URL is unavailable, so this experiment uses the
ARM64-capable upstream revision recorded in NOTICE (the last revision before
the A32 Context API was removed). This replaces the earlier May 2022 backend
for the Android startup investigation. Its adapter retains shared
page-table ownership and uses coherent atomic exclusive-write callbacks. A checked,
idempotent patch adapts formatter constness to Cara’s bundled fmt version; it
does not modify guest instruction handling.

GLES uses the same atlas shader path selected by the legacy `CARA_IOS_GLES` macro.
Android enables this through `CARA_ANDROID_GLES_EXPERIMENT`; EGL context ownership
and the Android shell remain native to Android. Packed input upload, index reuse
and uniform reuse are enabled for this experiment. A successful APK build does
not establish matching pixels or performance on Mali hardware.

For comparison, use native resolution, sound on, 100% guest CPU and the same
in-game scene. Record emulation speed separately from FPS, graphics issues, audio
and thermal behavior. Dynarmic versus FastInterp changes CPU cost; it does not
isolate GPU performance. No ROMs, firmware or keys are packaged.

## iOS artifact

The No-JIT workflow now also packages `Cara-iOS-gles3-unsigned.ipa`. It contains
`Payload/Cara.app` and must be signed with your own provisioning before installation.
The original unsigned app ZIP and no-JIT audit remain available.

The backend also has an opt-in ARM64 host smoke test covering arithmetic,
context restoration, instruction-cache invalidation, and successful/failed
exclusive stores. This is synthetic CPU validation, not an Android device or
GLES correctness test.

## Renderer control build

The manual Android workflow accepts `renderer=gles32` to build the native GLES
3.2 renderer without the Apple GLES 3.0 atlas compatibility path. It keeps the
same Dynarmic backend, resolution and guest timing. The APK artifact is named
`cara-android-arm64-gles32-control-dynarmic-experimental`. This is a controlled
Mali comparison, not a claimed performance improvement. Both variants use the
same debug application ID, so preserve saves before replacing an installation.
The default renderer remains `atlas` until physical-device testing supports a
change. Matching unstripped native symbols are uploaded separately for profiling.

## RG Slide draw-cost investigation

The November 13 device investigation uses Dark Moon's warmed title screen,
native resolution, sound enabled and 100% guest clock. It does not establish
whole-game compatibility or a matched save-state benchmark. First-use shader
links are excluded from steady windows; they can take tens of seconds on this
Mali driver. The same-build native pipeline cache restores 54 programs without
source linking on a subsequent launch.

Detailed two-second windows isolated about 879 ms in lighting/fog/procedural
LUT synchronization out of 1,438 ms in Draw. Framebuffer attachment setup was
about 73 ms, framebuffer selection 42 ms, texture acquisition 42 ms, raster
state application 71 ms and dirty invalidation 29 ms. Timings are nested wall
phases, not exclusive CPU totals or GPU execution timestamps. The guest
submitted roughly 840 draws per top-buffer swap; this count alone does not
establish redundant guest work.

Retaining game-texture bindings, alone or with vertex-input bindings, reduced
some state calls but produced no useful title-speed improvement in a bracketed
same-APK comparison. Those experiments remain off by default.

An Android-only native-LUT experiment keeps Cara's packed atlas vertex path but
samples the existing streaming buffers through GLES 3.2 buffer textures. It
removes the additional buffer-to-2D-texture LUT upload. Buffer mapping, explicit
flushes, unmapping and wrap invalidation remain intact; guest commands, clock
and hardware event timing are unchanged. A bounded RG32F ring stays within the
GLES 3.2 minimum addressable buffer-texture size. Shader binaries use a separate
cache filename. The non-Android GLES path retains 2D LUT textures.

On this RG Slide the first warmed native-LUT title capture reduced LUT time to
about 6 ms per two-second window and raised the overlay from about 10 FPS / 18%
speed to 13 FPS / 22%. The title appeared intact; exact checkpoint pixel equality,
save-state restoration and broad gameplay validation are outstanding. This is
a measured title-screen gain, not a claim of full-speed gameplay. The feature
is default-off, additionally gated on GLES 3.2 capability and a loaded TexBuffer
entry point. Developers can select it at process startup through the Android
configuration's Debug/native_lut_buffers setting. The trace_renderer_timing and
trace_renderer_bookkeeping debug settings expose the phase reports.

Buffer-texture addressing and capacity rules follow the
[Khronos GLES 3.2 specification, section 8.9](https://registry.khronos.org/OpenGL/specs/es/3.2/es_spec_3.2.pdf).


## Exact surface copies and native GLES residency

The November 14 RG Slide investigation exposed a byte-coherence failure after
partial CPU writes in both Android GLES renderer variants. The generated-data
oracle first remapped a GPU-owned image correctly, then invalidated sixteen
bytes for a CPU write and validated the destination through the production
cache path. An untouched RGBA8 byte at offset 292 changed from 188 to 187;
there was no GL error. This was not a reason to tolerate approximate guest
memory or disable the oracle.

The shared framebuffer-copy helper selected `GL_LINEAR` even when source and
destination rectangles had identical dimensions. Selecting `GL_NEAREST` for
these unscaled copies passed the unchanged oracle on this device. Scaled color
copies retain linear filtering. The observation identifies an exact-copy
problem in this driver/path combination; it does not establish the driver's
internal arithmetic or a general GLES conformance finding.

The Android debug setting `verify_surface_coherence=true` runs the generated,
ROM-free production-cache test after initialization and before guest execution.
Failure stops launch. It covers exhaustive packed 16-bit colors, linear/tiled
RGB8 and RGBA8 aliases, partial destinations, CPU-write fallback, later GPU
owner writes, split producers, retained small writes, and selective cleanup.
Diagnostic failures report formats, layouts, first mismatching byte and GL
error. The repair is commit `bc23e1f`; native GLES APK and ARM64 Dynarmic checks
passed in [Actions run 43619360906](https://github.com/dmikey/cara/actions/runs/43619360906).

Native GLES 3.2 now also receives the existing residency-remapper enablement;
it was previously set only in the atlas EGL initialization. The Android debug
setting `gpu_surface_remap` selects the same cache mechanism in either renderer.
It does not change GPU-completion events, guest clock, scheduler or frame timing.

A same-APK on/off/on comparison used Dark Moon's warmed title screen on the RG
Slide, native resolution, JIT enabled, 100% guest CPU, sound enabled, single-context
presentation and a 100% speed limit. Each row averages the last ten steady
nominal two-second windows, excluding shader compilation. The title animates;
these are matched title-screen runs, not a restored gameplay checkpoint.

| Native GLES 3.2 measurement | Residency on A | Residency off B | Residency on C |
| --- | ---: | ---: | ---: |
| Top-buffer swaps per window | 19.4 | 16.5 | 19.3 |
| Surface downloads per window | 0 | 33 | 0 |
| Surface download time | 0 ms | 1167.4 ms | 0 ms |
| GPU remap time | 14.5 ms | 0 ms | 15.6 ms |
| Draw time | 1093.0 ms | 450.3 ms | 1098.3 ms |

The bracketed residency-on throughput is approximately 17% higher. It remains
about 9.7 refreshes per second and does not establish full-speed Luigi gameplay.
The draw phase rises when readbacks are removed; synchronization/driver costs
can move between phases. These nested wall timings must not be summed as
exclusive CPU time or interpreted as GPU execution timestamps. Android's audio
service reported an active output track; listening quality was not verified.

The atlas renderer remains the default and was faster in the earlier warmed
native-LUT title test. Native GLES residency is a validated comparison mechanism,
not justification to switch the default renderer or replace the public APK.
The device logs, generated-data oracle results, configurations, screenshots and
raw comparison windows are retained with the RG Slide profiling artifacts.
