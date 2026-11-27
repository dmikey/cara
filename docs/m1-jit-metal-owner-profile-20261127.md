# M1 JIT Metal CPU investigation, 27 November 2026

Status: measured private renderer investigation; public OpenGL/GLES defaults unchanged. This report does not establish that Metal is faster.

## Reproduction and source

Physical Apple M1, four actual Dynarmic guest cores, guest CLK 100%, cap 100%, native 1x rendering, SDL sound, normal Dark Moon USA boot, 3,000 refreshes. A down at refreshes 60 through 2850 in steps of 150, released six refreshes later. Measure refreshes 2401-2800; capture both LCDs at 800, 1600, 2400 and 3000. No software vertex fallback or skipped draw requests. All installed Cara app manifests and read-only gameplay locks checked before and during owned jobs; competing applications/builds stop only owned jobs.

Exact audited private revision: `f56f2b73c64161cc3feedd4d2df2bb2ba83e71c9`, based on main `432b376` plus retained optional retile and diagnostic/capture corrections. Main at investigation start: `292147c`. Shared Metal production sources have no incoming main changes between those main revisions. The private run is not a claim of testing the latest public package. System compiler cache retained and unknown; fresh application directories do not mean driver-cold.

## CPU attribution

The normal OpenGL worker rejects the detailed category-clock mode. The first attempted GL profile was rejected before gameplay with `GL transactions conflict with diagnostic: CARA_CPU_CATEGORY_TIMING`. The normal worker was preserved. A corrected diagnostic run uses uninstrumented GL as an observed-work reference and enables exclusive category clocks only in Metal. Do not treat its cross-backend elapsed difference as matched-overhead performance proof.

| Exclusive owner-thread category | CPU ms/refresh |
| --- | ---: |
| others | 0.156 |
| cpu_execution_including_memory | 4.456 |
| hle_svc | 0.150 |
| gpu_command | 1.215 |
| dsp_hle_tick | 0.034 |
| metal_bind | 3.335 |
| metal_record | 0.619 |
| metal_encode | 2.257 |
| metal_transfer | 3.343 |
| metal_publish | 1.474 |
| metal_present | 0.678 |

The measured Metal owner clock is 17.811 ms/refresh. Category clocks themselves add overhead; CPU time excludes sleeping and differs from inclusive host API spans. Do not add process CPU, thread CPU, wall time or GPU envelopes.

The scored window issued 278,153 draw requests, 4,782 PICA lists, 4,782 display transfers, 3,985 texture copies and 6,376 fill requests in each backend. All 3,001 observed guest tick/global-tick, four core-PC and selected work rows matched, as did guest SD files. Counts and selected states are not complete command payload or VRAM equivalence.

## Stack samples

A separate four-second wall-sampling diagnostic of the Metal owner produced 1,528 observations. Inclusive stacks overlap. Binding occurred in 369 samples; raw vertex capture in 120; recording in 280; byte resolution in 245; buffer-to-buffer blit calls in 104; pending publication in 115; surface-writer status in 85. These are stack incidences, not additive CPU fractions or GPU utilization. Sampling starts after the frame-2400 directory is observed and symbol processing adds delay; no precise refresh-span claim follows. The first sampler timed out at 12 seconds and produced no report. The retained second attempt allowed 60 seconds and completed cleanly.

The driver blit stack includes `AGX::BlitUtil::ResourceOverlappingTracker::checkDependentBlits`. This is direct evidence of CPU work issuing/checking individual blits, not proof that those checks can be removed. The surface-writer stack is a separate lead for reusing fully validated terminal completion.

## Images

Both native-size screens rendered. Preserve the existing cross-renderer upper-screen precision/edge differences; the lower screen is exact at the retained checkpoints. Do not use tolerance, channel permutation or reduced filtering as a correctness fix. Same-renderer optimization arms must match both screens exactly.

## Rejected joint retile/queue probe

The first quiet OFF/ON pair tested the retained two-source GPU retile plus starting the existing adaptive queue at 32 instead of 8. OFF mean/p99/owner CPU: 17.557/19.495/15.800 ms. ON: 23.115/55.511/17.861 ms. Observed state/work/files and both LCDs matched exactly. Stop this candidate without more ON trials; it has no demonstrated benefit. One ordered pair cannot identify causality or establish statistical regression.

The first analysis gate incorrectly demanded `CARA_REGULAR_STAGING`, whose source guard is `NativeMetalCost::Enabled()` and is intentionally off during quiet timing. Original failed CASE/RESULT files remain preserved. A retained-data-only review confirmed clean native completion and exact selected outputs. No quiet activation-count claim is made and no run was repeated to repair the interpretation.

## Evidence and commands

Local evidence root: `outputs/cara-m1-jit-metal-current-20261125/`. Keep ROMs, guest data and dump-derived images private. Retain `native-profile-20261127-1` (admission failure), `native-profile-20261127-2`, `metal-samples-20261127-1` (sampler failure), `metal-samples-20261127-2`, and `retile-quartet-20261127-1/RETAINED-REVIEW.json`.

Reviewed runners: `run_native_compare_20261127.py profile`, `sample_metal_20261127_2.py`, `retile_quartet_20261127.py`. Each creates fresh directories; do not rerun a closed path or replace a receipt. Existing fixed source/build/hash gates bind executable, bundled libraries and observers.

## Completed-writer cache result

The OFF/ON/ON/OFF test completed all four 3,000-refresh Metal runs plus one matching OpenGL reference. Every same-renderer checkpoint LCD, observed guest-state/work row and guest SD file matched. No software vertex fallback or skipped draws; all owned groups exited cleanly. The cache remains OFF: it did not show a consistent saving.

| Case | Mean ms | p99 ms | Owner CPU ms | Process CPU ms |
| --- | ---: | ---: | ---: | ---: |
| OFF 1 | 18.158 | 21.345 | 16.318 | 21.019 |
| ON 1 | 23.194 | 54.894 | 19.497 | 25.536 |
| ON 2 | 17.987 | 21.896 | 16.174 | 20.898 |
| OFF 2 | 24.294 | 33.192 | 18.947 | 24.701 |
| OpenGL reference | 16.812 | 20.318 | 9.609 | 17.409 |

OFF mean drift was 6.137 ms and owner CPU drift 2.629 ms. Neither ON run beats both controls beyond drift; the faster ON alone is not a performance result. Adaptive queue history, scheduling, unknown device caches and shader specialization can affect these process-level measurements. Do not promote this flag based on one favorable cell. Evidence: `writer-cache-quartet-20261127-1/RESULT.json`.

## Threading difference that remains relevant

The actual OpenGL log confirms the normal owned command worker: 128 batches and 8 MiB in flight. Metal's sampled owner thread runs `RunLoop -> Dynarmic -> guest SVC -> PICA dispatch -> native bindings/recording/transfers`. Thus lower Metal API overhead alone does not guarantee a shorter emulation critical path. OpenGL overlaps queued driver submission with emulation; Metal still performs substantial preparation and encoding on the owner. This source/stack observation does not quantify a hypothetical worker speedup. Moving encoding needs immutable draw/resource leases, ordered publication, CPU readback barriers and fault propagation; simply moving a wait to a new thread is not a fix.

## Host contention and evidence limit

After the timing probes, a proper-host snapshot showed `apfsd` at 97% CPU, `fileproviderd` at 70%, `duetexpertd` at 57% and `bird` at 37% (macOS percentages use approximately 100% per logical CPU). A later snapshot showed `apfsd` at 193%. This is a concrete source of host contention that the earlier emulator/compiler-only monitor did not exclude. It does not prove that these services caused any particular frame or that benchmark artifacts caused their activity. Consequently the noisy brackets do not justify promotion or a precise renderer-speed ranking.

New large artifacts and guest replay directories live under `/private/tmp/cara-m1-metal-20261127/`, outside the Documents workspace. Keep only source, small receipts and aggregate evidence in the repository. Do not stop user/system sync services to obtain favorable timings. A new performance admission must check both installed apps/locks, foreign compiler/render jobs, and sustained background filesystem/sync activity before launching and throughout timing. Correctness-only runs may proceed under contention, but must be labeled and must not become performance evidence.

## Narrow retile owner-proof reuse result

The within-transfer two-owner metadata memo compiled as private revision `f92f49bac027f0aea08621703d01cb18fa4c2479`. The OFF/ON/ON/OFF test kept fused retile ON in both arms, stock adaptation and startup width 8, and completed-writer caching OFF. Activation was observed in both ON processes; all selected guest state/work/files and both LCDs matched exactly.

| Case | Mean ms | p99 ms | Owner CPU ms | Process CPU ms |
| --- | ---: | ---: | ---: | ---: |
| off-1 | 18.326 | 21.184 | 16.188 | 20.941 |
| on-1 | 26.711 | 77.717 | 19.421 | 26.261 |
| on-2 | 28.688 | 67.807 | 20.120 | 26.937 |
| off-2 | 17.515 | 19.730 | 15.452 | 20.022 |
| gl-final | 17.708 | 55.712 | 9.688 | 17.918 |

Both ON processes were slower and used more CPU than both controls in this bracket. This does not isolate why the change regressed under the observed host conditions. Keep it private and off. Do not enable the wider-queue retile route, persistent completed-writer cache, or this metadata memo on the basis of reduced source operations.

## Scan-local status reuse: candidate correctness passed, timing blocked

The isolated candidate is `CARA_METAL_REUSE_SCAN_STATUS=1`; default remains OFF. It preserves every clipped interval's `Covers` check, remembers at most two producer identities during one lane-local immutable region scan, and rechecks both producers before returning. Additional producers retain per-span status checks. No Pending observation is retained across calls and no GPU command, wait, validation receipt, draw or transfer is removed.

The actual changed C++ compiled and passed eight synthetic CPU contract cases in each of separate OFF and ON processes. Cases include pending/complete states, producer failure arising during a scan, a rejected clipped interval, initially failed producer, failure on the last covered stripe, a third producer and clipped endpoints, empty/invalid reads. A 16-span alternating two-producer case preserves all 16 coverage checks while reducing status queries from 16 to 4. These are synthetic contracts, not real GPU fault injection. Boost headers emitted existing deprecated-builtin warnings; the compile and both executables exited successfully.

An audited full private M1 JIT Metal build at `cde0aa06fd1081290f43551820ace838fd1960af` completed the OFF/ON normal-boot Luigi replay and the matching OpenGL reference. Both Metal processes retained the standard adaptive queue, guest CLK100, cap100, 1x, SDL sound, and all actual vertex/draw/transfer work. Retile, retile-owner memo and persistent writer-completion caching were OFF. Actual scan reuse emitted its once-only activation witness in ON and none in OFF. All 3,001 observed guest state/work rows, guest SD files and both LCDs at all four selected indices matched exactly between Metal arms. The OpenGL reference matched observed guest execution/work/files; existing cross-renderer upper differences remain. All three native groups exited 0, reaped without forced cleanup.

This proves selected correctness on this replay, not a performance win, full VRAM equivalence or broad title compatibility. The entire run is explicitly correctness-only under known host contention. A separate scored quartet has not started: read-only host admission found background filesystem/sync activity at approximately 238% CPU against a declared 50%-of-one-CPU admission threshold. No system daemon or user app was signaled. Keep the candidate OFF until a quieter matched performance bracket shows both CPU benefit and acceptable frame tails.

[Small native and component receipt](evidence/m1-metal-scan-status-20261127.json). The production C++ delta applies directly to main without the private retile or diagnostic patches. Reproduce the CPU contract with `python3 tools/tests/test_metal_scan_status.py`. The branch's changed production C++ and fixture bytes equal those compiled in the private gate. The native build includes retained observation overlays; the latest-main branch as a whole has not been rebuilt or device-tested by this receipt.

Local full receipts: `/private/tmp/cara-m1-metal-20261127/scan-status-build-1/RESULT.json`, `scan-status-correctness-1/RESULT.json`, and `HOST-ADMISSION.json`. The scored runner `outputs/cara-m1-jit-metal-current-20261125/scan_status_quartet_20261127.py` now applies the added background-service gate before and throughout every owned replay. It must use a fresh output directory; do not overwrite these closed correctness receipts. Its thresholds are an admission policy for this investigation, not a hardware claim about CPU frequency, GPU utilization or player performance.

Exact private native source is preserved on [`archive/m1-metal-profile-20261127`](https://github.com/dmikey/cara/tree/archive/m1-metal-profile-20261127) at `cde0aa06fd1081290f43551820ace838fd1960af`. It contains generic observation overlays and rejected default-off experiments, not a release or a replacement for main. No ROM bytes, guest saves or captured game images are included.
