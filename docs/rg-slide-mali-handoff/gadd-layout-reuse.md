> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# Packed layout uniform reuse: physical RG Slide result

2026-11-25. Fresh OFF/ON/ON/OFF bracket using the existing restored `5f1d9868…` private APK, original genuine fixed Gadd save, identical shader hints, JIT, fixed guest CLK 100%, UBO ON. No rebuild, source change, shader cache change, or player app change. All four arms passed the retained workload gate: 165,994 scored draws each across three 120-refresh windows ending at 481/601/721, zero shader builds, identical guest graphics counters. Both screens passed the retained screenshot smoke check; the final ON image was visually reviewed. The lower screen is legitimately the same maroon panel in this scene. Captures are host scheduled, not exact guest-aligned pixel proof.

## Measured result

Mean exclusive emulation-thread graphics CPU fell **24.088153 → 23.695931 ms/refresh**, a **0.392222 ms (1.63%)** reduction. Both ON arms were below both OFF arms by more than OFF drift in each of the three windows. This is an instrumented result: identical phase counter overhead exists in all arms, and no uninstrumented release result is claimed.

| End refresh | OFF CPU ms/ref | ON CPU ms/ref | closest margin / OFF drift |
| --- | --- | --- | --- |
| 481 | 23.656833 / 23.515083 | 23.146000 / 23.316667 | 0.198417 / 0.141750 |
| 601 | 24.084167 / 24.017333 | 23.607083 / 23.658833 | 0.358500 / 0.066833 |
| 721 | 24.614417 / 24.641083 | 24.355750 / 24.091250 | 0.258667 / 0.026667 |

Only the middle window p50 cleared OFF drift. p99 and worst were mixed, so this does not establish improved tails or full speed. Shader-setup wall spans did not improve beyond control drift; they are not an isolated uniform-location timer.

## Actual execution and lifetime review

The separate eight-second 200 Hz DWARF activation profile recorded 2,220 process samples with zero loss, including 1,563 NativeEmulation samples. It sampled the `IOSLayoutUniform` cache branch (0.13% inclusive); `glGetUniformLocation` had no samples, versus 1.60% inclusive in the earlier 1,567-main-thread-sample baseline. This supports actual branch activation, not zero total API calls or an exact hit count. `glProgramUniform4iv` remained sampled. Profile cost is excluded from the timing bracket.

The existing cache keys by actual linked program (or separable VS handle), resolves `cara_vertex_layout[0]` on first use, compares all 16×4 GLint values, and performs the original upload when the layout changes. Program switching selects a different entry. Shader-manager programs remain alive without in-place relink/eviction in this renderer; a reconstructed renderer owns a fresh cache. No guest payload, source bytes, vertex work, order, or transfer was removed. This test does not prove safety for future program-name recycling or external relink changes. Default remains OFF pending mainline review.

Evidence: `layout-bracket-2/COMPARISON.json`, per-arm `*-GATE.json` and receipts, `layout-profile-1/inclusive.txt`, actual `perf.data`, config receipts and screenshots. Earlier `layout-bracket-1/O1` was interrupted before a result and is preserved, not counted as another experiment.
