> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# RG Slide raster-state reuse: physical matched result

2026-11-25. Existing restored private APK5f1d9868; original genuine fixed Gadd save and identical shader cache. JIT, Performance Mode, fixed guest CPU100%,1×, UBO ON and packed-layout uniform reuse ON in every arm. Only `atlas_reuse_raster_state` changed in fresh OFF/ON/ON/OFF. No build, source change or player app update.

## Result

Exclusive emulation-thread graphics CPU: **23.645806→22.232708ms/refresh**, saving **1.413097ms (5.98%)**. Both ON cases beat both OFF cases by more than control drift at all three120-refresh windows. State-Apply elapsed scope: **2.510431→1.743431ms/ref**, saving0.767ms. These phase metrics overlap; do not add them.

| End | OFF graphics CPU ms/ref | ON graphics CPU ms/ref | nearest margin / OFF drift |
| --- | --- | --- | --- |
| 481 | 23.078833 / 23.367833 | 21.645000 / 21.789333 | 1.289500 / 0.289000 |
| 601 | 23.439750 / 23.763750 | 22.168417 / 22.204000 | 1.235750 / 0.324000 |
| 721 | 23.932417 / 24.292250 | 22.717667 / 22.871833 | 1.060583 / 0.359833 |

Early medians improve beyond drift in windows481/601; late721 does not. Only first-window p99/worst clearly improve beyond drift. This does not establish a general tail improvement or full speed: late medians remain near33.6ms. Scope instrumentation is identical in both policies, including its own TLS and clock overhead. Reuse still enters the same Apply scopes and does not remove those instrumentation calls. No uninstrumented release measurement or statistical confidence claim.

## Correctness and source scope

All four arms passed original full-work comparison:165,994 scored draws, identical selected guest command/payload/upload/index/LUT counters and zero scored shader builds. Every arm restored the original genuine same-build archive. Captures passed both-panel smoke checks, and ON2 was visually reviewed. This fixed scene has a legitimately maroon lower LCD. Host-scheduled captures do not prove exact guest-aligned intermediate pixels or every title.

The existing flag makes Apply compare cull/depth/stencil/blend/color/scissor/viewport fields against its tracked current state. Baseline forces those setters every Apply. Changed desired values still execute original setters. Program, texture, buffer and framebuffer binding comparisons remain active; no draw, transfer, shader effect, GL error/status check or guest timing work is skipped. Global reuse also enables the layout-uniform cache, so that separate cache is explicitly ON in both arms.

Constructor and state-load reset invalidate the full raster snapshot. SubApply updates only its applied subset and does not incorrectly make an invalid full snapshot valid. HUD/presentation/remap restore tracked OpenGLState through Apply. Direct remap dither/rasterizer-discard modifications have explicit restore; exceptional scissor restore invalidates cached raster state. Direct context changes or new external raw-state writers require the same invalidation contract. This result is the measured stock Atlas path, not a claim for every render worker or context. No default was changed in this experiment.

## Remaining source and profile leads

Earlier low-rate DWARF profiles identify draw dispatch into Mali as the largest remaining CPU family (~18.63% inclusive NativeEmulation samples in the original profile,21% in the later independent layout profile). These are separate diagnostic runs, not measured recoverable milliseconds or GPU timing. Repeated framebuffer attachments remain hot but the closed FBO-cache trial did not improve the workload. Do not repeat it. Packed vertex preparation is~4–6% inclusive and overlaps its children. Zero-tail comparisons have a separately measured small result; no vertex copies were removed by that change.

The retained layout-profile memcpy/memset-only caller report has105 selected samples:57memcpy and48memset. Roughly32% of memcpy call-chain weight is attributed to PrepareIOSPackedVertices; other samples include UBO and stripped Mali driver callers. This percentage is within memcpy, not total thread time, and incomplete unwinding prevents an exact full attribution. Direct packed guest-range copies and zero-fill are visible source operations. Eliminating them requires a live immutable snapshot/content proof covering changed guest bytes and ownership, rather than trusting repeated guest addresses. Smaller SamplerInfo SyncWithConfig is only0.32% inclusive in that diagnostic sample; it is a weaker next target.

Evidence: `state-bracket-1/COMPARISON.json`, four arm gates/receipts/configs/restored logcat and captures, exact existing source, `layout-profile-1/copy-callers.txt`. The runner script fixed the schedule before the first case; STATE-PLAN.json records it after the run had started and is not claimed as an independent preregistration.

## Cleanup

FINAL-DEVICE.json verifies both Cara PIDs absent, Daijisho foreground, player APK unchanged73edfbc0…, retained private APK unchanged5f1d9868…, and no active private `files/cara-emu` root. Every test root is archived. Last archived O2 configuration is literally UBO=True, packed_layout_uniform_reuse=True, atlas_reuse_raster_state=False, JIT=True, fixed CLK100, adaptive_frame_skip=False. These are archived test settings, not a changed player configuration. No global setting or player save was written. Both new options remain private review candidates.
