> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# Packed-fetch reuse composed with UBO and raster-state reuse

The retained GPU packed-fetch optimization still helps the **late** original Gadd window with UBO/state/layout reuse enabled. It does not speed every phase or lower graphics CPU. It is enabled only in the private profile with its matching learned cache; public defaults and the player app are untouched.

## Exact workload and source

Physical RG Slide / Unisoc T820 / Mali-G57, same existing private APK SHA `5f1d98687382e12752c49d8e2663beef5a3f3a8548b5e6db3cd16427f7837bdc`, original genuine fixed save SHA `4e3e204118e1bdf88b9119bb088166540eefa0d79cec94a812bf51e963b30dc3`, JIT, fixed guest CLK100%, 1×, frame skip off. UBO/state/layout reuse on in all arms; no new build. All three 120-refresh windows and every original guest draw execute.

Only `gpu_packed_fetch_reuse` differs. The existing `PackedVertexFetchReuseGLSL` reads one or two uvec4 texels for an attribute, selects aligned float words with fixed swizzles, and reinterprets them as float vectors. Unaligned and integer inputs retain their scalar conversion; component defaults are unchanged. Guest vertex program, reference quaternion evaluation/sign correction, fragment program, all vertex/index bytes, draw order, CPU/guest timing, and all GL checks are unchanged. The prior physical G57 fixture already verified2160vectors; it was not repeated. Existing historical control/candidate/control GPU results are documented in `docs/rg-slide-mali-handoff/dark-moon-entry327.md`.

`IOSIdentity()` includes packed-fetch-reuse0/1 before archive decoding. Candidate preparation produced a separate5,224,732-byte archive SHA `82397f3499d1cac5f2425b2d929850924ed608870fa00e0621164d36b6d5b887`; its actual header contains policy1, geometry0, and the separable vertex role. Its17 actual vertex source entries contain the local fetch function and aligned float fast path. No header/source editing or stale-policy cache reuse. Controls cloned the unchanged original policy0 cache; both candidates cloned the same verified policy1 archive. All scored windows recorded zero shader compile/link scopes/calls.

## Quiet bracket

One prospectively declared O1/F1/F2/O2 bracket. Existing phase counters enabled equally; no GPU sampler, synchronous GLES debug or shader census. Every arm passed all original guest-work, UBO activation, file/save/cache binding and screen-smoke gates. Counts include54,840 /55,252 /55,902 draws in the three windows per arm; uploads, index/vertex cache work, LUT bytes, PICA lists and all compared original fields match exactly.

| Window | O1 p50 | F1 p50 | F2 p50 | O2 p50 | ON−OFF mean | OFF drift |
|---|---:|---:|---:|---:|---:|---:|
|481|31.21|31.08|31.56|30.69|+0.370ms|−0.520ms|
|601|31.66|31.13|31.79|31.17|+0.045ms|−0.490ms|
|721|33.88|32.30|32.84|33.95|**−1.345ms/−3.97%**|+0.070ms|

Both late ON medians are below both OFF medians by more than OFF drift. Early/middle show no improvement. Across all three window medians, the average benefit is only0.310ms. Late p99 mean39.745→37.255ms, but OFF p99 drift3.31ms exceeds the2.49ms difference; tail improvement is not established. Worst refreshes likewise do not establish a stable benefit. Graphics CPU **increased** about0.254–0.309ms/refresh across the three windows, so this is not a CPU-saving result. Same instrumentation and one bracket limit inference.

## Separate hardware-counter pair

A fresh OFF/ON pair collected the existing Mali hardware counters at100ms and CPU clocks at100ms, with actual monotonic window timestamps. These are instrumented diagnostic runs, not quiet timing results. All workload/zero-compile gates passed. Only full GPU sampling intervals contained within each 120-refresh window are included. Device-wide GPU counters include compositor work and can lag CPU submission; they are not exact per-draw attribution.

| Window | Core cycles/primitive OFF→ON | Difference | GPU active Mcycles/s OFF→ON | Emulation-thread CPU/wall OFF→ON |
|---|---:|---:|---:|---:|
|481|663.06→645.77|−2.61%|756.48→721.27|96.73%→97.93%|
|601|701.80→683.73|−2.57%|794.50→747.07|96.61%→97.62%|
|721|762.21→735.63|−3.49%|834.40→794.91|93.50%→96.99%|

Early and middle already occupy almost all of one CPU thread, so a smaller GPU vertex cost need not shorten those phases. The late control has more GPU pressure; relieving it leaves the CPU closer to its limit. This explains the phase-specific result without claiming universal CPU/GPU headroom.

External bytes/primitive increased415.04→453.09,473.02→503.00,497.60→521.69. This is not an output-traffic reduction, and bus saturation is not established. The packed float route reduces sampled shader work without changing geometry-stage topology or introducing a GS.

Actual CPU frequency tracks show the top core at2.704GHz throughout both diagnostic runs; direct thread samples place NativeEmulation on core7. CPU percentages use exclusive thread CPU divided by exact consecutive monotonic window endpoints, not whole-SoC CPU or median frame times. Profiler overhead is included. GPU live clock remains inaccessible through shell/run-as sysfs and privileged Perfetto devfreq/frequency-event collection. Using rated850MHz, late OFF/ON active rates correspond98.17%/93.52% of rated-cycle capacity, **not directly measured wall-busy percentages**. Active cycles include pending/stalled jobs. No clock, governor, root or permission changes.

## Screens, preparation and final state

Actual candidate screenshot was opened: ghost-lab geometry, lights and translucent ghosts are present; the lower panel shows its expected maroon field with controls. This is host-scheduled visual smoke plus the retained conversion fixture and full-work gates, not guest-frame-aligned pixel/depth proof.

Unscored preparationW0 completed the original workload and atomically checkpointed its cache. An unnecessary final Exit Game accessibility attempt failed because Android could not obtain idle UI; that failure, menu screenshot and full logs are preserved. Source shows the normal two-second dirty-cache checkpoint; the actual policy1 archive was pulled, inspected and hashed instead of repeating cold preparation. The immutable seed/cache and player profile were never changed.

After all runs, restored the stable private APK and active root. Kept UBO/state/layout and packed-fetch reuse ON in the **private** profile with the matching verified cache. Preserved the prior control config/cache under device `packed-fetch-preserved-control`, all case roots, original/new saves and candidate artifacts. Active config SHA `eb53da888c38a090c416b75c9df83ac7b3f606bff84cf721b108eaf9ba78e965`. Private PID absent; player APK SHA `73edfbc01189477a9ecba57dad95dd22d40be47fa08a56483287898db7761cf9` unchanged. `PRIVATE-ACTIVE-RECEIPT.json` PASS. No public-default change or engine code commit.
