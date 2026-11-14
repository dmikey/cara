# Stories surface copies on native M1, 2026-11-14

A fresh full native build restored a newly created Secret Path checkpoint at
100% guest CPU with sound, the normal 100% speed limit and unchanged hardware
timing. This is a new 3D gameplay scene, not the original grass Slot 2 or the
older Lilia dialogue. Native desktop OpenGL costs do not establish iPad/GLES
performance. Neither CPU nor shader execution used emulator JIT.

The source was `0e744a1`, engine fingerprint
`27a18f1cec5081210eed2f957358cdba5007846fc59bff32354a636624471a2a`,
with ordinary 20-byte decoded records and LTO off. The genuine checkpoint was
saved at refresh 5400, local tick 24130074607. No compatibility check was bypassed.
Its SHA-256 is
`77d8faeae8e7db0aac9b094a78d0950b58975a1fca909cb496ca9bd9e55dae8b`.
The executable SHA-256 is
`78bdfdafe45603de2b9acea846e23ec4aca7130f9e04871cac2dfd2c00108c01`.

## Returned copy attribution

[Successful-copy tracing](surface-validation-copy-tracing.md) was enabled for
original VideoCore frames 120 inclusive through 720 exclusive. Common counters,
category/PICA/sampled phase profiling were off. The final report was written at
721, with observed copy frames 120–719. Both boundary snapshots and the guarded
restore were verified. The process exited normally.

| Source layout → destination layout | Calls | Logical guest bytes | Host wall ms | Host thread CPU ms |
| --- | ---: | ---: | ---: | ---: |
| 256×912 → 256×512 | 14,992 | 5,551,194,112 | 795.780 | 770.206 |
| 64×256 → 64×128 | 5,997 | 196,509,696 | 271.152 | 268.000 |
| 32×128 → 32×64 | 5,997 | 49,127,424 | 256.888 | 253.532 |
| 16×64 → 16×32 | 3,998 | 8,187,904 | 173.429 | 171.390 |
| Total | 30,984 | 5,805,019,136 | 1,497.249 | 1,463.128 |

All four aggregate buckets are RGBA8, tiled, native resolution, with matching
source/destination stride. There were no clock failures, saturated totals,
unknown identity uses or aggregate-bucket overflow. All calls were clocked;
these are synchronous `CopySurface` costs, including driver enqueue/wait, not
GPU completion timings. Trace collection overhead remains outside these scopes.
The bytes are logical copied spans, not a bandwidth measurement.

The matching 600-refresh window contains 51.64 copies per refresh, with
instrumented copy calls averaging 2.495 ms wall and 2.439 ms thread CPU per
refresh. These figures do not describe the entire rendering pipeline, predict
savings from removing a copy, or explain the full slowdown. No skipping was
introduced. The trace does not distinguish the desktop copy-image shortcut from
its framebuffer-blit fallback.

The 512 chronological details cover only the beginning of the window (frames
120–130); 30,472 later details were omitted while totals continued. All retained
source ranges were valid and every retained destination had a last-observed GPU
invalidation. Dominant repeated groups show different invalidation serials
between copies. This prefix is not an unbiased sample or a complete ownership
history.

The large 256×912 source spans `0x18070800–0x18154800`, exactly the union of two
256×512 aliases offset by 400 rows. Its dominant copy span is 256×400 pixels,
409,600 bytes. Each smaller source covers two adjacent destination halves.
These are overlapping layout families; dimensions alone cannot establish mipmap,
atlas or redundant-copy use. Surface identity does not identify pixel provenance.

`InvalidateRegion` skips the GPU owner itself. Consequently a source with no
recorded invalidation can still have been rendered into. Stable allocation IDs
or null invalidation serials cannot establish unchanged content.

## Quiet blit-state comparison

A separate same-binary off/on/on/off comparison varied only
`CARA_BLIT_STATE_SCOPE`. State reuse stayed on in both arms. Each run restored
the same genuine checkpoint and immutable filesystem basis, warmed 120 refreshes
and measured 600. Successful-copy tracing and other timing observers were off.

| Run | Wall ms/refresh | Thread CPU ms/refresh |
| --- | ---: | ---: |
| A1 off | 128.417126 | 79.200243 |
| B1 on | 128.032640 | 79.247701 |
| B2 on | 128.348359 | 79.228796 |
| A2 off | 127.220911 | 78.996716 |

The enabled mean was 0.139769 ms more CPU (0.177%) and 0.371481 ms more wall time.
Baseline wall drift was larger than the mean wall difference. There is no
established speedup or precise wall effect; the option remains **off**.
The API-call reduction proved by the pixel/state oracle did not translate to a
measured gameplay benefit here.

All four runs matched captured ARM/VFP state, timers, ticks, HID, full final
refresh-721 RGBA and persistent file bytes. The final RGBA SHA-256 is
`cc1fec21b25b62f9fba455e5c041dbecdc2e21907a96c566ac5c445d0deeebe7`.
This is not every-intermediate-frame or complete-RAM equivalence. Audio opened
through stereo CoreAudio; listening quality and underruns were not independently
measured. All comparison processes exited normally, and the task folder was
restored to its immutable basis. Original user saves and the iPad were untouched.

## Next discriminating test

Attach the GPU-owner identity and operation origin to invalidation records:
draw color/depth, display transfer, texture copy or fill. Draw records also need
framebuffer layout, dirty rectangle, actual guest scissor bounds/mode and write
masks. Distinguish framebuffer validation from base-texture and mip acquisition.

Draw damage currently uses a viewport-clamped rectangle while guest scissor
rejection happens in the fragment shader. It can conservatively mark untouched
pixels dirty, but this trace cannot quantify that case. An INCLUDE-only damage
intersection needs generated-shader pixel/depth/stencil, tiled interval and
partial-write alias-coherence tests before use. Do not narrow pre-draw validation
or skip copies on address equality or last-observed metadata alone.

Private reproducibility files are under `profiles/validation-copy-next-20261114`,
including the full-build receipt, self-test results, `successful-copy-trace` and
`blit-state-abba`. The default-off blit experiment and interval selection test are
compiled by native CI; the metadata interval test runs without a GPU context.
The GPU oracle was executed locally against the full build. Cross-game and
physical-iPad performance acceptance remain separate work.
