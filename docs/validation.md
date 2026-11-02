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
