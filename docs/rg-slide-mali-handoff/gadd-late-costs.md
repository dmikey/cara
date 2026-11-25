> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# Late Gadd investigation with state reuse enabled

New physical RG Slide investigations, 2026-11-25. These use the original fixed private APK (`5f1d9868…`) and genuine Gadd archive (`4e3e2041…`), with JIT, fixed guest CLK 100%, 1× resolution, Performance Mode, frame skip off, and persistent UBO, packed-layout reuse, and raster-state reuse enabled. The player app was not changed.

## Late CPU profile

A three-second, 200 Hz DWARF trace starts after restored boundary 601. It recorded 801 process samples, including 563 NativeEmulation samples, with zero lost samples. This diagnostic is separate from the quiet timing brackets.

| Symbol family | NativeEmulation sample share | Scope |
| --- | ---: | --- |
| DrawArrays / DrawElements | 17.05% | Inclusive driver call trees |
| FramebufferTexture2D | 10.66% | Inclusive driver call tree |
| Packed vertex preparation | 5.68% | Inclusive; overlaps memory operations |
| Upload-order search | 5.68% | Inclusive; overlaps comparisons |
| memcmp | 8.70% | Self |
| memcpy | 3.73% | Self |
| memset | 3.37% | Self |
| State Apply | 2.31% | Inclusive |

These families overlap and must not be added. TLS lookup accounts for 3.02% inclusive, and clock calls include instrumentation overhead; these are not all removable gameplay work.

For the late window ending at 721, 871 sampled submit events consumed 11.03 ms thread CPU and 11.59 ms wall time: about 95% active CPU. They were not predominantly sleeping, but this cannot distinguish useful driver work from driver busy-waiting. The full submit scope was 714.53 ms over 120 refreshes, or 5.955 ms per refresh. It is not pure GPU wait. Explicit GPUWait was only 0.72 ms over that window.

Exclusive CPU categories in that diagnostic were graphics 23.249 ms, guest execution 5.628 ms, SVC 2.252 ms, other 0.585 ms, and DSP 0.085 ms per refresh. Do not add nested renderer phase spans to these categories.

The same recorded samples show no NativeEmulation samples on CPUs 0–3, 22 on CPU 4, 13 on CPU 5, 41 on CPU 6, and 487 on CPU 7. Read-only sysfs maximum frequencies are 2.184 GHz for CPUs 0–3, 2.301 GHz for CPUs 4–6, and 2.704 GHz for CPU 7. This trace already runs predominantly on the fastest core. No affinity or priority was changed.

## Separate GPU timestamp diagnostic

The existing timer samples one interval every 16 refreshes and checks query readiness and disjoint status without forcing glFinish. Eight results for restored refreshes 601–720 were returned. All were exact multiples of 16,777,216 ns: 16.777, 33.554, or 50.332 ms. That coarse quantization cannot resolve a roughly 2 ms gap.

These intervals also include GPU idle and submission gaps and exclude LCD composition. They are not isolated shader execution, bandwidth, or causal wait measurements. Evidence is under `late-gpu-1`, including `SUMMARY.json`. No conclusion that all remaining time is CPU-only or GPU-only follows.

## Input-binding retention: closed without a speedup

The existing `atlas_retain_vertex_bindings` switch omits post-draw unbinds on private input units 7 and 8. Each next draw still explicitly binds its selected handles, texture deletion follows GL semantics, feedback unit 9 cleanup remains, and HUD, presentation, and capture use lower texture units. UBO, layout reuse, and state reuse were enabled in both policies.

A fresh OFF / ON / ON / OFF bracket passed all four original-work gates: 165,994 scored draws per arm, identical selected command, payload, upload, index and LUT counters, and zero scored shader builds. Both panels passed the physical output smoke check, and ON2 was visually reviewed. These host-scheduled captures do not prove exact intermediate pixels.

| Metric | OFF mean | ON mean |
| --- | ---: | ---: |
| Graphics CPU per refresh | 22.569889 ms | 22.576375 ms |
| Window median | 32.4083 ms | 32.4117 ms |
| Window p99 | 37.2017 ms | 37.5683 ms |
| Window worst | 38.3817 ms | 38.6650 ms |

There is no supported gain. Keep input-binding retention off and do not repeat this bracket. No source or build was changed. Evidence: `binding-bracket-1/COMPARISON.json` and the four arm gates.

## Avoided duplicate work

The historical packed-staging trial already removed resize zeroing and most redundant clears without improving frame time; it was not repeated. Normal Draw already suppresses redundant same-FBO attachments with `framebuffer_info` and `SameFramebufferSurface`; that optimization should not be implemented again. The closed multi-FBO cache trial changed object/pass behavior and was slower. Driver attachments can remain costly when actual targets and lifetimes change.

## VS uniform reuse with persistent UBO

This separate interaction completed its declared four-arm bracket against the same saved workload. The lifetime proof and prospective admission rules are in `VS-SOURCE-REVIEW.md`. All original guest workload counters matched, with 165,994 draws and zero scored shader builds per arm. Both panels passed their smoke checks; ON2 was visually reviewed. The earlier historical single-pair VS trial did not use this persistent-UBO baseline and had different draw counts; its performance numbers are not combined with this test.

Actual persistent Map/write calls fell from 165,994 to 101,192 per arm, a 39.0% reduction. Rotations fell from 77 to 43. Page reuse was witnessed, with zero waits and zero sync fallbacks. This validates activation of the reduced-upload path, not a speed claim.

| Metric | OFF mean | ON mean | Interpretation |
| --- | ---: | ---: | --- |
| Uniform scope per refresh | 0.676611 ms | 0.560181 ms | Improvement exceeds OFF drift in each window |
| Graphics CPU per refresh | 22.312222 ms | 22.269847 ms | Does not exceed OFF drift in any window |
| Window median | 32.1467 ms | 32.1533 ms | Flat |
| Window p99 | 37.5567 ms | 37.0933 ms | Does not exceed OFF drift |
| Window worst | 39.4367 ms | 39.2050 ms | Does not exceed OFF drift |

The uniform region saves 0.116431 ms per refresh. Whole-workload CPU saves only 0.042375 ms, within control drift, and late medians remain near 33.9 ms. Keep VS reuse off for now. Do not repeat this bracket or call the reduced writes a gameplay gain. Exact values and work gates are in `vs-bracket-1`.

## Remaining gap and next evidence

The latest late-window controls still spend about 22.9 ms of exclusive graphics CPU per refresh, leaving roughly 12.9 ms to remove for the requested 10 ms graphics budget. Frame medians remain about 33.8 ms; 16.7 ms whole-refresh performance has not been achieved.

The largest measured remaining driver families are draw dispatch and actual framebuffer changes, with substantial exact vertex comparisons and packing. A larger distinct route is native vertex attributes with corrected quaternion work retained in a geometry stage: the retained 12-link driver replay improved 6.28 to 1.59 seconds and a synthetic opposite-sign triangle matched pixels, but neither proves warm gameplay improvement. Another lead is render-pass continuity and real attachment ownership transitions, rather than another redundant-state cache. Those need a separate measured workload and correctness gate. No larger renderer change was started here.

## Saved policy and cleanup

`LATE-FINAL-DEVICE.json` verifies both Cara processes stopped, the player APK unchanged (`73edfbc0…`), the original private APK unchanged (`5f1d9868…`), and no running private game. The final owned control data was copied back to the active private root so a future manual launch uses the enabled policy; the original archive remains preserved. Future strict replay runners must archive that owned active root before staging another case.

Raster-state reuse stays enabled in the saved private policy, with layout reuse and persistent UBO enabled, VS reuse disabled, and binding retention at its original disabled default. The device copy is `/sdcard/Android/data/org.cara.emu.malipreview/files/fixed-next-state-layout-ubo-on.ini`, SHA256 `60264f56862f881f04dd372983c9a3ed996cdd3d472a09ece31e6d728780b176`. `PRIVATE-STATE-ON.ini` retains the same policy locally. The same exact configuration is also active at `files/cara-emu/config/config-mmj.ini` (matching SHA256), so future private launches keep state reuse on. This is not a changed player setting or public default. No build or install was required, and no further experiment is running.
