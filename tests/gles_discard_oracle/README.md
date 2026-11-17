# Standalone GLES discard/stencil probe

This ROM-free UIKit app creates an actual EAGL OpenGL ES 3 context. It links no
emulator libraries, reads no Nintendo inputs, and uses its own bundle and data
container (`com.dmikey.cara.glesdiscardoracle`). It is an API diagnostic, not an
emulator-generated-shader, PICA hardware, gameplay, or performance test.

Build on macOS with an installed Xcode simulator SDK and CMake/Ninja. Keep build
and temporary files on the chosen artifact volume:

```sh
cmake -S tests/gles_discard_oracle -B "$PROBE_BUILD" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$PROBE_BUILD" --target cara-gles-discard-oracle -j1
```

Inspect an **existing dedicated simulator's** UUID/state and free disk space
before running. Do not create/reset a simulator, download a runtime, replace the
Cara app, or use a physical device for this procedure. Record and restore the
initial simulator boot state; preserve Cara's data. Sign the small probe app
with an ad-hoc simulator signature, install only this distinct bundle with
`xcrun simctl install`, and launch it with `xcrun simctl launch`. Copy
`Documents/gles-discard-oracle.json` from this probe's own container obtained by
`simctl get_app_container`. Terminate only the probe, and return a previously
shutdown simulator to shutdown. The app remains installed in its isolated
container; no uninstall/reset is needed.

The report deliberately distinguishes `measurement_completed` from each case's
`matches_spec_expected`. A completed diagnostic can contain unexpected outcomes;
never convert those outcomes into a pass or discard them. Each case preserves
raw before/after RGBA8 pixels, stencil bytes, and depth classifications as base64,
plus observed counts, expected counts, actual viewport/scissor queried directly
before the tested draw, geometry and vertex count. GL errors are checked and
counted by stage; a GL/setup/readback error aborts with an error report.

The 156 cases use the same thirteen state combinations as the native minimal
probe: KEEP/ZERO/REPLACE/increment/decrement/invert/wrap operations, stencil/depth
failure controls, masks, disabled stencil, and color/depth write combinations.
Both an oversized full-screen triangle and an explicit two-triangle quad are
used. For each geometry, modes cover no discard, unconditional discard, a
hardcoded rectangle discard, rectangle discard plus host scissor, empty host
scissor, and host scissor without discard. The known reference areas are 192,
108 and zero pixels; no observed baseline is substituted for those expectations.

GLES depth/stencil `ReadPixels` is not used. A separate RGBA8 probe framebuffer
shares the D24S8 renderbuffer. Eight stencil bit tests use KEEP and write mask
zero; depth tests classify the two fixture values using EQUAL and depth writes
disabled. These are classifications, not a raw 24-bit depth dump. Repeated reads
in reversed plane order must preserve both planes. Every case first verifies
actual color/depth/stencil sentinels. The tested draw's color is read **before**
any depth/stencil classification draws.

On the M1 simulator's Apple Software Renderer, minimal conditional discard
restricted stencil to 108 pixels for both geometries; the native CGL context's
192-pixel stencil observation did not reproduce here. All 78 explicit-quad cases
matched their fixed expectations. The oversized-triangle host-scissor controls
had 17 unexpected cases despite the queried state matching: these are retained
as diagnostic failures, not attributed to a driver or silently normalized. The
initial 65-case report with the first failing host control was also preserved.
This evidence supports no claim about a physical Apple GPU or the cause of the
native conditional-discard observation.
