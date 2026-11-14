# ROM-free GLES LUT upload oracle

`cara-gles-lut-self-test` is an explicit, excluded-from-default-build iOS GLES test app. Its bundle ID is **`com.dmikey.cara.gleslutoracle`**, with its own data container. It opens no ROM, BIOS, guest session or Cara save. The target is available in both iPhoneOS and iOS-simulator GLES configurations; physical-device execution is not part of the evidence below.

The app creates an offscreen EAGL ES3 context and exercises the actual `RasterizerOpenGL` lighting/fog and procedural LUT upload paths, plus `AllocateIOSLUT`, `UploadIOSLUT` and the production staging/ring APIs. It tests RG/RGBA views, independent zero/one/signed values, nonzero offsets, row crossings, partial/unchanged updates, forced ring wrap and two renderer constructions with each `stream_buffer_hack` value. Renderer reconstruction is not a complete emulator save/load test.

A highp sampler/float/int shader encodes fetched float bits into RGBA8; dithering is disabled. No floating-point color-render-target extension is needed. GLAD transfer observers check that every client upload reads a registered live owned CPU span with unpack PBO zero, and that default upload calls perform no GL mapping. Incoming state deliberately includes a mapped four-byte unpack PBO, pack/array buffers, texture/sampler bindings, active unit, alignment, row length and skip values. Actual state is compared before/after each tested operation; GL errors fail the oracle.

The [ES 3.0.6 buffer-mapping rules](https://registry.khronos.org/OpenGL/specs/es/3.0/es_spec_3.0.pdf#page=51) disallow using mapping-return pointers as GL data arguments. A driver need not visibly reject that undefined behavior. Accordingly, this oracle does **not** claim the previous path failed pixels or GL-error checks. Its four deliberate negative controls instead verify detection of a wrong valid texture offset and an intentionally skipped upload, in both views. Only the default owned-CPU-staging path is covered; no optional PBO upload mode is implemented or claimed.

## Build, run and retrieve

Given a normal GLES simulator configuration, build only the separate oracle target. Choose an already booted dedicated simulator; do not erase it or reinstall Cara. The following removes only this oracle's previous JSON so a stale pass cannot be mistaken for a new result:

```sh
set -eu
CARA_LUT_BUILD=/absolute/Fast/gles-simulator-build
CARA_LUT_SIM=YOUR_DEDICATED_SIMULATOR_UDID
CARA_LUT_BUNDLE=com.dmikey.cara.gleslutoracle
cmake --build "$CARA_LUT_BUILD" --target cara-gles-lut-self-test -j2
test "$(plutil -extract CFBundleIdentifier raw -o - "$CARA_LUT_BUILD/bin/cara-gles-lut-self-test.app/Info.plist")" = "$CARA_LUT_BUNDLE"
xcrun simctl install "$CARA_LUT_SIM" "$CARA_LUT_BUILD/bin/cara-gles-lut-self-test.app"
CARA_LUT_DATA=$(xcrun simctl get_app_container "$CARA_LUT_SIM" "$CARA_LUT_BUNDLE" data)
test -d "$CARA_LUT_DATA/Documents"
rm -f -- "$CARA_LUT_DATA/Documents/gles-lut-oracle.json"
xcrun simctl launch "$CARA_LUT_SIM" "$CARA_LUT_BUNDLE"
```

The app runs asynchronously and atomically writes its own `Documents/gles-lut-oracle.json`, then displays PASS or FAIL. Use a bounded wait for a newly created report; a crash, missing report, `passed:false` or GL error is failure. Inspect the GL strings, per-hack `modes`, checked texels/components, transfer/state checks, zero default GL maps and four detected negative controls. After reading the report, terminate only the oracle bundle. For iPhoneOS CI, the same CMake target is compile-only; no installation is implied.

## Verified simulator result

The maintained source with the lifecycle/setup correction compiled, linked and ran on the dedicated simulator. Saved JSON reports `Apple Inc.` / `Apple Software Renderer` / `OpenGL ES 3.0 APPLE-24.0.2`, with fragment highp precision 23:

| Check | Result |
| --- | ---: |
| Positive texels / components | 73,246 / 292,984 |
| Actual texture transfer calls | 94 |
| Multirow / nonzero-X calls | 24 / 26 |
| Poisoned-state checks | 54 |
| GL maps during default uploads | 0 |
| Deliberate negative controls detected | 4 |
| Each hack mode, two constructions | 30,464 texels / 40 transfers |

Initial harness launch trapped on iOS 27 because it lacked scene adoption. The corrected app uses public `UIWindowSceneDelegate` lifecycle and a scene manifest. Constructor setup also explicitly cleans unpack state before tiny constructor-owned CPU uploads; hostile state remains immediately around the actual operations under test. These are harness corrections, not additional production upload defects.

Private receipts are under `profiles/gles-lut-upload-20261114/`: `gles-lut-oracle.json`, `runtime-receipt.json`, `validation-summary.json` and the preserved first-launch crash. Actual source was `25cd2a6` (production staging plus oracle and setup correction). Cara Documents metadata was unchanged. An OS-generated SplashBoard cache delta occurred during simulator boot and was left intact; the entire Cara container is not claimed byte-identical. The oracle was terminated and the simulator returned to shutdown.

This establishes the specified GLES upload/state/readback behavior on Apple Software Renderer. It establishes no physical-iPad result, hardware-rendered game compatibility, complete session save/load behavior or performance improvement.
