> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# RG Slide exact vertex-input comparison trial

The zero-tail comparison candidate reduced emulation-thread graphics CPU from 20.304 to 20.047 ms per refresh in this matched Gadd lab comparison. The 0.258 ms (1.27%) mean reduction is small. Both candidate CPU readings are below both controls by more than the observed control drift at each of the two scored windows. Reuse-comparison host time fell by 0.094 ms per refresh. P99 and worst-frame results are mixed, so this does not establish smoother gameplay or a route to10 ms graphics CPU by itself. The candidate remains private and off by default.

## What changed

The new physical DWARF CPU profile located memcmp self cost at 6% of active emulation-thread samples and exact upload-order Find at 3.70% inclusive. The previous snapshot-lease experiment had increased preparation cost by comparing before materializing, then searching again; that experiment was read and was not repeated. This candidate keeps all materialization, source reads, padding initialization, upload lengths, GPU vertex programs, draw order and effects. It shortens only exact comparisons of a suffix already certified zero in both owned inputs.

The producer records the final payload byte before row rounding after fully clearing/constructing the packed snapshot. A retained upload entry receives that certificate only after the full owned shadow assign. Replacement invalidates it before any new write, whole-entry move/swap preserves it with the shadow, and queued publication is excluded. Both full logical lengths and all original index/resource/eligibility checks remain required. Unknown or out-of-range proof takes the old complete comparison. The comparison covers the larger of the old/new active prefixes; the remaining suffix is zero in both. Source details are in SOURCE-REVIEW.md.

Physical diagnostic verification executes the original full memcmp beside every shortened comparison and throws on disagreement. It completed with 423,486 verified comparisons and zero disagreements, on the normal boot and the first later saved lab sequence. The independent host oracle passed 10,802 whole-buffer comparison and exact search cases. Requested lengths avoided are a logical accounting measure, not measured DRAM traffic: memcmp may stop before examining the requested range on mismatches.

Local source branch `private/slide-zero-tail-20261125`, commit `254849df26b7730a8c30349bf601ccbaa11fd8ad`; use CANDIDATE-ARTIFACT.json for the actual full revision and signed APK hash. EXACT-CANDIDATE.patch is the exact production+diagnostic delta against the tested fixed62b648c2 source. No push, merge or shipping activation occurred.

## Seed and setup

The new engine fingerprint 912ce4f20d91a028279dfcee9711f11a9b9c3348a6eb43b937c2b04a8a87a7a9 requires a new genuine same-build save. The original4e3e… state remains preserved. A normal boot through the same title/slot flow produced the early Gadd close-up using the stock Save State UI and an empty private slot. New seed SHA 4e889e85df83173d044a026ac30de939fbd4c3ef569e15231e3ee908c79e75b9,31,165,508 bytes; header, CPU backend, ROM executable, compressed digest and guest files are verified in physical-3/seed. This is the same Gadd lab sequence, not the byte-identical old engine archive. Every scored arm loads this exact new archive with the same guest disk files/cache.

Setup recoveries are retained. First seed confirmation was held by a pose-specific classifier while the game continued, yielding a later lab state. The first verifier load was blocked by a template containing an old slot timestamp; the corrected slot-label-only cue and unchanged state hash gate loaded successfully. This later seed provided full comparison correctness evidence, but advanced into the escape effect and had intervening shader builds, so no timings from it were used. A second early setup also stopped unsaved under the pose check. Removing that unnecessary threshold allowed the third setup to save immediately after the verified stock menu and empty-slot check. Its actual native menu screenshot was reviewed and shows the Gadd close-up. The first Android build succeeded but renderer LOG_INFO was compiled out, so activation audit failed before device use; a bounded Android CaraPerf counter fixed observability in build7. These failures did not change the comparison oracle or workload gates.

## Prospective timing plan

A control scout found shader work in windows601,961 and1081; adjacent windows 721/841 were shader-free and both screenshots showed the actual lab animation. TIMING-PLAN.json froze those 240 refreshes and OFF/ON/ON/OFF order before the scored arms. Scouts are excluded. UBO stays ON, JIT Dynarmic on all guest cores, GLES Atlas, 1×, CLK 100, cap 100, Performance Mode and frame skip OFF. Both renderer timing and sparse phase clocks are instrumented identically. Original-full-compare verification is OFF during timing. Driver cache state is unknown; the observed controls bound short-term drift, without statistical certainty beyond this four-run test.

All four arms restored successfully and matched all declared guest work of 47,419/46,584 draws per120refresh window, 94,003 draws per arm, plus PICA lists, index uploads/cache, packed routes, staging/clear/source-copy bytes and LUT update counters. All scored windows have shader_compiles=0 and compile_ms=0. Both candidate arms admitted 92,424 shortened comparisons between counter markers 600/840 and avoided 117,660,042 requested comparison bytes over that 240 refresh span, identically. No verifier comparisons ran in timing. Counters are sparse cumulative observations aligned to first draws at those markers, not a count of every raw memory transaction.

| Arm | End | Graphics CPU ms/ref | Reuse-check span ms/ref | p50 ms | p99 ms | Worst ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| OFF1 |721|20.684|1.951|28.45|60.10|74.02|
| ON1 |721|20.520|1.867|28.29|58.49|74.15|
| ON2 |721|20.283|1.835|28.38|60.58|68.99|
| OFF2 |721|20.608|1.942|28.90|59.52|70.47|
| OFF1 |841|20.003|1.935|34.50|42.02|45.51|
| ON1 |841|19.747|1.858|34.27|43.07|61.29|
| ON2 |841|19.637|1.839|34.13|41.52|46.68|
| OFF2 |841|19.923|1.950|34.72|44.07|61.78|

At 721 the conservative CPU margin is 0.088 ms/ref versus 0.0766 OFF drift; at 841 it is 0.176 ms versus 0.0807 drift. Reuse-check margins also clear their respective OFF drift. Median improvement clears drift only in the second window. Neither p99 nor worst-frame improvements clear drift in both windows. Mean window medians 31.643→31.268 ms and p99 means 51.428→50.915 ms are simple means of window summaries, not pooled percentiles. Do not claim a general latency improvement from them. Workload differences from the previous UBO report arise from a different genuine same-build saved point; timings must not be compared across those saved states.

## Both screens and cleanup

The four gadd.png captures show the expected Gadd/ghost/electricity lab animation. The lower LCD is legitimately maroon, RGB66/40/49, with shell controls. The candidate ON2 screenshot was visually reviewed. These are host-timed visual checks; exact guest-frame pixel equality and all-render-target coverage are unverified. The independent exact comparison oracle establishes the skipped suffix decisions rather than a per-pixel game proof.

The candidate APK was preserved at the path recorded in CANDIDATE-ARTIFACT.json, along with its genuine seed, source, logs and build receipts. The first restore install was rejected for older versionCode, then the Android debug downgrade flag restored exactly the prior signed fixed APK5f1d9868…. Final device receipt confirms original player APK73edfbc… unchanged, both Cara PIDs absent and Daijishō foreground. All private profiles are archived, and no player saves/settings were accessed.

The larger remaining CPU cost remains actual Mali draw submission (18.63% inclusive emulation-thread CPU samples in the prior profile), with framebuffer attachment 8.62%. The measured FBO cache failed, and no equivalent guest draw/vertex work can be omitted from this evidence. This candidate trims a measured host comparison cost but leaves the larger CPU/GPU draw problem open.

[Timing values and drift](bracket-1/COMPARISON.json) · [Mean summaries](SUMMARY.json) · [Source proof review](SOURCE-REVIEW.md) · [APK/source/seed](CANDIDATE-ARTIFACT.json) · [Final device state](FINAL-DEVICE.json)
