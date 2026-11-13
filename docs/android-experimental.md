# Experimental Android GLES build

The Android Actions workflow builds a debug-signed `arm64-v8a` APK with Dynarmic
CPU JIT and Cara's GLES atlas pipeline. It targets testing on the RG Slide
(T820, Android 13). This is an experimental port, not an established compatibility
or performance result. Apple builds continue to force CPU JIT off.

Download the APK from the `cara-android-arm64-gles-dynarmic-experimental` Actions
artifact. Enable installation from your downloader/file manager and open the APK.
The application ID is `org.cara.emu.debug`. Save in-game and back up important
progress before changing builds. GitHub's temporary debug signing key can change
between builds; reinstalling may require uninstalling the previous test APK.

The dependency fetch is explicit and pinned: `python3 tools/prepare_android_dynarmic.py`.
The inherited MMJ Dynarmic URL is unavailable, so this experiment uses the
ARM64-capable upstream revision recorded in NOTICE. Its adapter retains shared
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
