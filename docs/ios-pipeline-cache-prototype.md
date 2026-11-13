# On-device iOS GLES pipeline cache prototype

Based on Cara `056385c`; preserves the Citra shader generator and GPLv2-or-later
attribution. No downloaded game caches, CPU JIT, executable-memory generation,
backend replacement, or device installation is part of this change.

The experiment is **off by default**. Set `CARA_IOS_PIPELINE_CACHE=1` in the app's
launch environment to test. An absent variable follows the previous iOS path.
The earlier desktop `*.cache` format is never read on iOS by this experiment.

Each title gets a `.cache.cara-pso1` file in the app cache directory, populated
only from pipelines observed while that title runs. After two seconds without a
new pipeline, a checkpoint is written to a temporary file and renamed. Normal
renderer teardown also checkpoints. A force quit before a checkpoint can lose
recent records. No change is made to save files or games.

On the next launch, the current rendering context/thread loads the store and
prewarms the observed vertex/fragment program pairs. A supported program binary
is tried before stage compilation. Rejected binaries fall back to the saved
GLSL sources. Drivers exposing zero binary formats take the source path and
never call the binary extraction/loading functions. Normal links request the
retrievable hint when binary formats exist. Successful prewarmed programs let
runtime shader-stage lookup defer stage compilation until it is actually needed
by another, previously unseen program pair. Uniform and sampler bindings are
reapplied after binary load and source link.

A first-ever launch has no recorded title pipelines. The renderer's existing
fixed shaders are compiled during its usual startup, and game-specific states
still compile when first encountered. This cannot predict an unseen game's
shaders. Source prewarming moves repeated-launch work into startup; it does not
eliminate compilation or guarantee a shorter total launch. No Apple driver
binary-format capability or frame-time benefit is claimed without device proof.

The identity includes a CMake-generated SHA256 over generator/decompiler,
source preparation, packed-vertex/input layout, shader manager/bindings,
rasterizer declarations, register layout and binary-resource source inputs.
CMake tracks edits and regenerates that fingerprint. It additionally includes
GL vendor, renderer, version, GLSL version, the OS build and advertised binary
formats. Persisted sources are paired by hashes of their exact bytes rather than
Pica register-layout keys. The whole store is transactionally decoded before GL
calls. Wrong schema/identity, malformed lengths, trailing data, truncation and
checksum mismatches produce an empty cache and ordinary compile fallback.

Limits: 128 records, 256 KiB per shader source, 1 MiB per program binary, 32 MiB
per title store, and one temporary file. Runtime recording respects the byte
budget before append. The limit is per title, not an aggregate across all
installed games. The OS may evict this cache. FNV checksums detect accidental
corruption; they are not signatures authenticating externally supplied files.
Transform-feedback diagnostic programs and unknown future variants are not
cached by this experiment.

## Verification

`tests/ios_pipeline_cache_test.cpp` runs without a ROM or OpenGL context and
checks source-only and binary round trips, every truncation and single-byte
corruption of a fixture, stale identity, oversized source/binary/count rejection,
and preservation of already accepted records after a failed decode. Run with
Clang address/undefined-behavior sanitizers:

```
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/ios_pipeline_cache_test.cpp -o /tmp/cara-ios-pipeline-cache-test
/tmp/cara-ios-pipeline-cache-test
```

For iPad proof, compare cold title, restarted warm title, corrupt cache, and
cache-off launches with the same ROM, scene, native resolution, 100% guest clock
and no CPU JIT. Logs report advertised binary-format count, startup binary hits,
source links, startup duration and checkpoint size. Verify output and captures,
not just link success. Parent task owns physical iPad testing; this isolated
prototype does not establish M4 or A19 results.

Local validation completed on 2026-11-13: full `cara-ios` arm64 build with
`CARA_CPU_TARGET=apple-m4`, `CARA_ENABLE_JIT=OFF`, and GLES3 succeeded; sanitizer
fixture passed; `tools/audit_no_jit.py ios --renderer gles3 --build-dir build-ios`
passed with no forbidden symbols and Apple system dynamic libraries only. These
are compile/host-fixture/audit results, not device gameplay measurements.

## Initial physical-device findings (2026-11-13)

The M4 iPad reports zero GLES program-binary formats. This device therefore uses
saved-source prewarming; binary-cache reuse is not available through this driver.
The first opt-in implementation regressed Luigi's Mansion: Dark Moon substantially:
a 60-refresh boot window averaged 69.6 ms. Review found full shader-source hashing
in the per-draw pipeline lookup. This prototype must remain disabled until that
lookup is corrected and cold/warm/off runs verify behavior on the device.

A same-build, cache-off checkpoint replay completed at 17.0 ms per refresh with
sound and fixed 100% guest CPU. A conditional BKPT diagnostic also appeared in
successful runs; it does not establish a checkpoint restore failure. No cache
speedup has been established by these initial tests.

The hot-path correction returned the cold-boot window ending at refresh 420 to
16.7 ms, but the experiment then stopped advancing before its intended endpoint.
It was stopped and the app reopened with caching and completion experiments off.
Cache persistence failure handling is under investigation; this is not a passed
cold/warm validation and must not be enabled by default.
