# LUT GPU residency

A new renderer's zero-filled CPU shadows did not prove that its lighting, fog or procedural-texture
lookup tables had been uploaded. The first stream-buffer mapping does not
invalidate existing contents. Consequently, a zero table could skip its first
upload and retain the default offset zero while a different nonzero table was
uploaded at that same offset. Sampling the zero table then returned its
neighbor's values.

The renderer now initializes lighting/fog/procedural-texture offsets to `-1`, meaning no GPU range
has been assigned. First synchronization uploads a table even when its values
match the zero CPU shadow. Successful uploads assign the normal nonnegative
texel offsets. Existing dirty notifications, unchanged-resident reuse, table
conversion, ring invalidation and forced-state synchronization are retained.
This adds no class fields, serialized guest state, clock changes or driver flags.
Renderer reconstruction after a state load gets fresh residency markers.

## Production-path GPU regression

`cara-mac-lut-self-test` creates a native OpenGL context and real
`RasterizerOpenGL` instances. A test-only friend invokes the actual private
`SyncAndUploadLUTsLF` and `SyncEntireState` methods. A fragment shader samples the
renderer-owned `samplerBuffer`; readback is compared against independently
specified zero, one, negative-one and positive difference-storage values,
never copied from the renderer CPU shadow. Upload/setup/draw/readback errors and invalid ranges fail
the test. No ROM, firmware, game checkpoint or window presentation is required.

On Apple M1 / OpenGL `4.1 Metal - 91.7`, the original renderer failed:

```text
initial zero lighting beside nonzero table: GPU texel 0
expected (0,0), got (1,0), offset=0
```

Separate unchanged-proctex probes also failed in both GPU views: a zero RG
color map returned nonzero noise data, and a zero RGBA color table returned the
same buffer's nonzero RG noise bytes through its RGBA view. A complementary
probe covered initially zero noise beside a nonzero color map.

With the same final test object and only the renderer object changed, the fixed
renderer passed **34,560 sampled texels**. Each of two repetitions constructs
fresh renderers for 14,080 lighting/fog checks, 1,024 procedural RG checks,
2,048 procedural RGBA checks and 128 initially zero noise checks. Cases cover
initially zero tables beside nonzero neighbors, unchanged resident reuse,
partial edits, nonzero-to-zero transitions, signed fog differences, actual
forced-state synchronization/restoration, and poisoned full-ring wraps that
must republish every LUT. RG and RGBA offsets are checked in their own texel
units. Procedural difference conversion is preserved; the oracle does not
redefine PICA interpolation arithmetic. Reconstructing the renderer exercises
the fresh residency path; this is not a full game save-state test.

The standalone oracle was compiled and linked with the native engine flags and
existing dependency archives. The explicit CMake target is available in native
Mac OpenGL builds:

```sh
cmake --build <native-build> --target cara-mac-lut-self-test
<native-build>/bin/cara-mac-lut-self-test
```

Context creation must succeed; unavailable contexts are failures, not skipped
passes. The test is excluded from ordinary builds and does not affect shipping
runtime. The fix is in the shared desktop/GLES upload logic, but this evidence is
native M1 correctness proof, not a physical-iPad result or a performance gain.
Unchanged-write notification elision is outside this change.
