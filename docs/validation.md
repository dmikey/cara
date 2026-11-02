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

The workflow builds from the pinned public upstream source and Cara patches, rather than relying on a developer's prepared checkout. It produces unsigned artifacts and audits the actual linked products. The native I/O self-test exercises write, flush, tell, seek, resize, read, invalid seek, and reopen behavior without a ROM.

A green CI result proves compilation, linkage, the no-JIT audit, and the host I/O regression check. It does not prove iOS simulator execution, physical-device installation, or commercial-game compatibility.
