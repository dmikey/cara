# Android mainline and Apple GLES tuning audit

Reviewed main e25df5729bf86e5c3bc71f4dfd11993dece7cbc2, 2026-11-25. Source inspection and retained physical evidence; no new GPU or FastInterp performance experiment in this audit.

## Already shared or enabled

Android's Atlas build defines both CARA_IOS_GLES and CARA_ANDROID_GLES_ATLAS in emulator/CMakeLists.txt. Much of the Apple GLES renderer is therefore the same compiled implementation rather than a second Android copy.

- GPU packed vertex input: Android EGL initialization.
- GPU surface remapping: JNI default true, existing coherence/eligibility fallbacks retained.
- GPU depth reinterpretation: Android EGL initialization.
- Resident vertex-upload and order reuse, 80 MiB bank and prepared index cache: Android EGL/JNI defaults.
- RG Slide persistent UBO, packed-layout uniform reuse, tracked raster-state reuse and packed-fetch decode: validated device-specific policy on main.
- Unused-quaternion specialization: exact tested Slide and G Cloud hardware/driver Performance policies, separate shader source keys and cache behavior, explicit opt-out; both devices physically verified.
- Async/program learning machinery exists on Android. Its cold first-use path still synchronously builds a program when an exact compatible linked fallback is unavailable. The recent G Cloud census observed no background worker during the costly burst.

Sources: emulator/CMakeLists.txt; emulator/src/ios/main.mm viewDidLoad; emulator/src/android/jni/egl_android.cpp Initialize; emulator/src/android/jni/main_android.cpp BootGame; emulator/src/video_core/renderer_opengl/gl_shader_manager.cpp.

## Intentional differences, not lost fixes

Apple sets CARA_VS_UNIFORM_REUSE=1. Android exposes atlas_reuse_vs_uniforms, default false. The retained Slide bracket with persistent UBO reduced writes 165,994 to 101,192 and the uniform region by 0.116 ms/refresh, but whole-workload CPU changed by only 0.042 ms within control drift. docs/rg-slide-mali-handoff/gadd-late-costs.md explicitly closes this route off. Do not rerun or promote merely to match Apple's defaults.

Apple gates some vertex/index retention and its 80 MiB bank on >=6 GiB physical memory; Android currently enables resident uploads/prepared indices and an 80 MiB bank directly. This is a memory-budget difference worth profiling on 4 GiB devices, not evidence that allocating more memory or blindly matching Apple improves speed.

Native LUT buffers, whole-image replacement, native vertex/geometry delivery, redundant binding retention and full-fill coalescing have platform-specific retained results. The Adreno and Mali driver traces show that moving time from upload to presentation or submission waits is not a whole-workload win. Rejected experiments remain off.

## Confirmed separate FastInterp opportunity

iOS explicitly enables these environment controls in main.mm; Android JNI/EGL does not set them, and the corresponding core defaults are off:

- CARA_FASTINTERP_ALU_OPERANDS
- CARA_FASTINTERP_TICK_BUCKETS
- CARA_FASTINTERP_CHAIN_BUDGET
- CARA_FASTINTERP_CACHE_WAYS=8
- CARA_FASTINTERP_RAM_BURST
- CARA_FASTINTERP_AL_DISPATCH

Implementations are portable core code in fastinterp.cpp, fastinterp_decode.cpp and arm_tick_counts.cpp. Their presence and Apple enablement do not prove a gain on Cortex-A76/A55 or establish cross-title correctness on the Android build. A separate fixed-CLK, full-work FastInterp comparison should isolate CPU/guest-memory time, ticks, faults, input/audio and both screens; keep GPU settings constant. These flags do not accelerate Dynarmic/JIT and are not enabled as part of the regular Cara install.

## Mainline integration boundaries

The regular app must be org.cara.emu with Cara label and painted mainline icons/variants. A preview package or test flags are not the shipping configuration. Keep the ordinary library, controller, audio, save-state and dual-display code from the same main revision; preserve unrelated org.citra.emu user data. Archive owned test data before removing the disposable preview app.

No additional renderer default or FastInterp flag was changed by this audit. Shader stutters are reduced, not eliminated: the G Cloud's lit links remain a major cost. The result is not a claim of Android/iOS performance parity.

## Release build versus runtime worker policy

android.yml compiles the optional ordered GLES worker with caraOrderedGles=ON for its unsigned production artifact. That does not turn the worker on: main_android.cpp only calls StartSubmissionWorker if Debug:ordered_gles_worker is true, whose default is false. The Mali hardware/default selector reads that runtime configuration value, not compile-feature presence. Do not disable a release feature or claim a regression merely from the CMake flag. The local regular build uses the default OFF compile configuration matching the physical direct-lane tests; either configuration requires the actual resolved startup settings to establish runtime equivalence.
