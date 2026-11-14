# Native indexed-draw bounds reduction on M1

This prototype keeps indexed min/max reductions in their source type (`u8` or
`u16`) and widens only the returned bounds. It reads live index bytes on every
draw. It adds no cross-draw cache, memory-generation assumptions, allocations,
GPU staging, shader changes, guest clock changes or event-policy changes.
The legacy empty-index result remains **minimum 65535, maximum 0**, including
for u8 input. The downstream empty-draw behavior is deliberately unchanged.

## Why this native path

At base `9c1f51d`, `AnalyzeVertexArray` scanned live indices into u32 accumulators
(gl_rasterizer.cpp:448–495). The native draw then maps the VBO, copies live loader
spans, unmaps, and separately copies the live index bytes into the IBO
(:1345–1381). `SetupVertexArray` has a 16-byte stack enable-mask; it has no staging
vector allocation or bulk zeroing. `OGLStreamBuffer::Map` maps the GPU ring
(gl_stream_buffer.cpp:46–62); there is no `MapVBO` symbol in this checkout.

Android commit `85ab167e422ea7d958576385eac86c96f0c6c9c0` was inspected. It targets
value initialization of GLES packed `ios_vertex_inputs`, which is not the
native desktop upload path. None of those Android changes were imported.
Skipping guest-memory scans based on page flags would be unsafe: asynchronous
writers need not participate in that tracking. This prototype needs no such
assumption and continues scanning the current bytes each call.

## Correctness and compiled production code

Both optimized and ASan+UBSan tests pass **77,926 cases / 158,480 checks**.
Coverage includes every u8 value and pair, the full u16 domain in three orders,
u16 boundaries, zero/one/vector-tail counts, natural alignment offsets, live
same-address byte mutations without generation notifications, untouched source
bytes and **34 guarded-page cases**. The maintained target is
`cara-vertex-index-range-self-test`; `--bench` additionally enables a CPU-only
scan microbenchmark where the thread CPU clock is available.

The actual production `AnalyzeVertexArray` objects were compiled with the same
M1 release flags and source dependencies from the genuine 9c build. Baseline
has 304 instructions in the complete function, including eight table-shuffle,
11 widening and 64 u32 min/max instructions across its loop paths. Candidate
has 234 instructions, native u8/u16 vector min/max and none of those shuffle,
widening or u32 min/max operations. These are static disassembly counts, not
executed instruction totals or a gameplay saving. No SIMD intrinsics were used.

## Scan-only ABBA results

The following are mean emulation-independent **thread CPU nanoseconds per
bounds-reduction call**. Each case runs baseline/candidate/candidate/baseline
with repeated live input mutations and matching checksums. The shader agent
held compilation during this short scan; no GPU or application was running.

| Indices | u8 baseline ns/call | u8 candidate | u16 baseline ns/call | u16 candidate |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 3.502 | 2.224 | 2.269 | 2.279 |
| 2 | 3.475 | 2.356 | 2.047 | 2.523 |
| 3 | 3.605 | 10.532 | 3.497 | 3.370 |
| 4 | 3.503 | 13.886 | 2.353 | 2.258 |
| 7 | 6.026 | 8.207 | 5.377 | 5.606 |
| 8 | 3.482 | 2.210 | 2.220 | 2.253 |
| 15 | 7.324 | 7.903 | 6.592 | 6.634 |
| 16 | 5.048 | 2.196 | 4.263 | 4.045 |
| 17 | 8.045 | 3.290 | 5.799 | 12.900 |
| 31 | 8.396 | 9.830 | 8.321 | 8.737 |
| 32 | 5.812 | 3.933 | 11.024 | 9.519 |
| 33 | 7.356 | 5.057 | 12.452 | 10.120 |
| 63 | 12.626 | 10.435 | 13.640 | 13.496 |
| 64 | 10.367 | 10.033 | 12.597 | 13.212 |
| 65 | 10.424 | 9.582 | 12.758 | 14.545 |
| 127 | 15.745 | 15.383 | 16.044 | 14.892 |
| 128 | 13.697 | 13.920 | 14.248 | 11.641 |
| 129 | 14.243 | 14.835 | 15.044 | 11.705 |
| 1,024 | 63.987 | 14.456 | 64.713 | 23.696 |
| 8,192 | 496.867 | 87.443 | 491.374 | 167.262 |
| 65,536 | 3950.074 | 646.830 | 3938.395 | 1359.131 |
| 262,144 | 15564.533 | 3084.801 | 15770.508 | 6294.758 |

For the tested 1,024–262,144-index arrays, candidate scan CPU was lower by
77–84% for u8 and 60–66% for u16. These are hot-array scan-only measurements,
not VBO/IBO upload, driver, guest execution, whole-draw or game performance.
Small-count results are mixed: for example u8 count 7 costs about 2.18 ns more
per call and count 31 about 1.43 ns more. No arbitrary size threshold was added.

The first seed-from-index-zero draft left one fewer index in vector loops and
regressed u8 count 32. The final max/zero initialization with an i=0 loop fixes
that case while preserving native-width reductions. The earlier draft receipts
are retained separately in `first-element-seed/`; the table above is the final
version. No gameplay, GPU, physical-device or full application build was run.

## Provenance and next step

Source base: `9c1f51d6259cff7463fbab2100e95666122802d9` in isolated worktree
`/Volumes/Fast/CaraBuild/cara-native-index-range-20261114`. Release production
objects use exact flags from the genuine
`/Volumes/Fast/CaraBuild/profiles/validation-copy-next-20261114/build`, with
source includes mapped to this worktree. The generated include path remains
in that genuine build. No CMake cache was copied or relocated, and those objects
were not linked into an application. Raw build arguments, disassemblies,
checksums, sanitizer logs and complete ABBA observations are in
`/Volumes/Fast/CaraBuild/profiles/native-index-range-20261114`.

A genuine application build, fresh normally created Stories checkpoint, matched
checked replay and uninstrumented timing are required to determine whether the
native scan improvement affects gameplay. A scan microbenchmark cannot answer
that question. Input timing, 100% guest CPU, sound and no-JIT settings must stay
fixed during those later comparisons.
