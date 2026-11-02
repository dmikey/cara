# Validation

## Local development evidence — November 2, 2026

- Source baseline: `weihuoya/citra` revision `302b0c3bc4312bc96489c11584a97664894b343f`.
- Build tools: CMake 3.31.10, Ninja 1.13.2, Xcode 27.1, iOS 27.1 SDK.
- iOS ARM64 app compiled and linked with statically linked emulator libraries.
- Installed and launched as a development build on a physical iPad Pro 11-inch (M4), iPadOS 27.0.1.
- A locally supplied Zelda: A Link Between Worlds USA dump booted and rendered its opening animation on both screens.
- Physical-device samples: 540 displayed frames in 33.911 seconds (15.924 FPS), later 900 in 89.426 seconds (10.064 FPS). These are early boot averages, with CPU interpretation, software graphics, and audio off.
- macOS probe: 278 frames in 30 seconds; longer run 921 in 120 seconds, returning success.
- The linked iOS binary had no Dynarmic, JitX64, mmap, mprotect, or vm_protect symbols. Its dynamic dependencies were Apple system libraries only. Development signing used no JIT entitlement or JIT-enabling debugger.

The measurements vary by scene and do not establish full-game performance. Gameplay, save/reload reliability, hands-on controls, device rotation, and long-session behavior remain unverified. Game screen captures and private device/signing receipts are retained locally, outside this repository.

## Continuous integration

The workflow builds from the emulator and dependency source committed inside Cara. Source validation rejects external git submodules and excluded game/system assets; no MMJ repository or dependency mirror is fetched. It produces unsigned artifacts and audits the actual linked products. The native I/O self-test exercises write, flush, tell, seek, resize, read, invalid seek, and reopen behavior without a ROM.

A green CI result proves compilation, linkage, the no-JIT audit, and the host I/O regression check. It does not prove iOS simulator execution, physical-device installation, or commercial-game compatibility.

## Renderer work — November 2, 2026

A deterministic 600-frame host comparison recorded 1,200 per-screen pixel hashes. The original software path took 52.2723 seconds (11.4784 FPS), combiner evaluation changes took 35.6809 seconds (16.8157 FPS), parallel scanline rendering took 26.6222 seconds (22.5376 FPS), and additional state/combiner/blend fast paths took 23.2904 seconds (25.7617 FPS). All 1,200 hashes matched the original in each case.

On the physical M4 iPad, the same 600-frame benchmark improved from 29.0166 seconds (20.6778 FPS) to 14.5803 seconds (41.1513 FPS) with the optimized software path.

The first experimental native GLES3 GPU run completed 600 frames in 5.56822 seconds (107.754 FPS, uncapped). The device reported `Apple M4 GPU` and `OpenGL ES 3.0 Metal - 105`. This is a GLES3 frontend using Apple's driver, not a direct Metal renderer. CPU JIT remained excluded. Early screen captures showed the game's opening animation. Longer-scene rendering and game compatibility are still being checked; this short benchmark is not a full-game/native-speed guarantee.

The native GLES3 adaptation uses 2D float textures for lookup tables, GLES 3.0-compatible shader generation, and framebuffer blits in place of newer copy-image operations. Unsupported shadow image operations and format reinterpretations require further compatibility work. The software path remains the reference/fallback build.

## Direct iOS GPU presentation and frame budget

The GLES3 frontend now presents a GPU renderbuffer directly through a native screen layer. Normal play no longer reads each frame back into CPU memory or builds UIKit images. Diagnostic captures read back only every 60 frames. The combined two-screen surface preserves the 400x480 layout and maps touch coordinates to the lower 320x240 region. A physical-iPad screenshot confirmed orientation and both screens; the frame-3600 top-screen PNG was byte-identical to the earlier readback path.

An uninterrupted 3,600-frame run of the earlier GLES3 frontend completed in 71.4814 seconds (50.3627 FPS, uncapped, zero pauses). With opt-in timing enabled, comparable runs measured 49.6843 FPS before direct presentation and 58.0952 FPS afterward. The final 600-frame window improved from 26.31 ms to 22.74 ms per frame; readback fell from 3.98 ms to 0.057 ms and native presentation took 0.22 ms. Interpreted PICA shader work still consumed 9.07 ms per frame in that window. The post-change 95th-percentile frame time was 26.89 ms, above the native 16.71 ms budget. These measurements do not establish a locked frame rate.

A larger per-draw vertex cache matched all 1,200 host screen hashes but barely reduced shader calls in the title scene; it was not retained. Further work should target GPU vertex processing and remaining CPU/driver costs, then validate real gameplay and sustained performance. CPU JIT remains excluded.

For local, uncapped timing, launch a development app with `CITRA_BENCHMARK_FRAMES=3600` and `CARA_FRAME_TIMING=1`. `Documents/citra/frame-timing.json` records 600-frame windows with mean, median, p95, maximum, over-budget frame count, shader calls, and time in interpreted PICA shaders, GPU readback, and native presentation. Timers are opt-in and add measurement overhead; shader/readback/presentation timings are wall-clock scopes on the emulation thread, not hardware GPU timestamps. Normal play is paced to the emulated display rate of approximately 59.834 Hz.
