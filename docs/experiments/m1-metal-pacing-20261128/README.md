# M1 Metal: separate graphics CPU from pacing dips

The user authorized continuing with background daemons present. The following comparisons are loaded-host observations, not quiet-host results. Proper-host installed-app checks, gameplay locks, competing-compiler checks, exact source/binary/ROM identities and owned process cleanup stayed required. No service was stopped or reprioritized. No iPad, iPhone or Android changes.

Both previously verified row-gather fixes stayed ON. JIT, 100% guest CPU, all four guest cores, cap100, native1×, SDL sound and all required GPU/reference/quaternion work remained unchanged. No skipped draws or software vertex fallback. The 3,000-refresh normal boot uses the existing A schedule and captures both panels at 800/1600/2400/3000. Score2401..2800 is the Gadd lab, not mansion traversal. Startup/cold shader time is separate.

## Loaded GL/Metal bracket

Source36c38a1; one GL/Metal/Metal/GL bracket. Every case exited0, reaped normally, group empty, GL errors0; selected tick/core-PC/work rows, guest files and same-renderer both-panel captures matched retained references. Cross-renderer lower panel is exact; upper-panel numerical differences remain (retained viewport correction check RGB MAE0.122), so do not claim exact cross-renderer upper-panel parity.

| ms/refresh | GL first | Metal first | Metal final | GL final |
| --- | ---: | ---: | ---: | ---: |
| Mean | 16.6060 | 16.7442 | 16.7983 | 16.6074 |
| p99 | 17.5213 | 21.6283 | 20.1170 | 17.6129 |
| Worst | 18.0003 | 66.4798 | 78.3986 | 18.6350 |
| Owner CPU | 9.0527 | 13.6981 | 13.8588 | 9.0503 |
| Process CPU | 16.1465 | 17.8785 | 18.1052 | 16.1133 |

Metal is about0.14–0.19ms slower on capped mean and1.73–1.99ms higher process CPU here. Owner CPU alone is not equivalent between renderers: GL also has its render worker/driver. Do not add process and owner CPU, or CPU and overlapping GPU/wall spans. External observed service CPU was2.28–2.90 core-equivalents over capture2400 through process exit, not the exact400-refresh ROI; short-lived service PIDs are unsampled.

At Metal's66.48/78.40ms worst refreshes, owner CPU was7.67/7.15ms and process CPU10.93/10.89ms. The largest dips are not explained by graphics CPU alone.

## Locate the wait before changing the queue

The existing bounded queue-wait timeline recorded1,149 waits with zero dropped rows. There was just one late capacity wait at refresh2400, about0.302ms, and none at2704..2707 despite the long2706 frame. Increasing queue depth would not address the observed pause.

Separate diagnostic children then measured all native Metal phases and exclusive CPU categories; these modes were never combined. The per-refresh phase observer covers all phases including vertex reads and flushes, retains at most512 diagnostic frames, takes snapshots after RunLoop and writes CSV after exit. This bounds diagnostics only, never gameplay work. All largest individual Metal host phases at the reproduced59.23ms2526 frame were at most1.48ms.

Reading existing saved limiter timing, without adding clock calls, aligned66.649/65.366ms sleeps with75.478/74.261ms frames at2526/2706. FrameUpdate ends the prior frame, begins the next and increments the counter: sampleF saved sleep belongs to ledgerF-1. Both CSV indices are explicit. The unchanged normal limiter caps requested sleep debt at25ms for cap100. An actual65ms sleep therefore includes oversleep/scheduling delay; it is not a65ms requested guest delay. This identifies the location, not yet an OS mechanism.

## Remaining measurable CPU target

Publication subcategories found preparation0.02279, commit/tracking0.08270, authority0.73274, invalidation0.05158, notice0.03031 and reservation0.00569ms per refresh. A second split of ALL authority publication calls measured validation0.54597 and map updates0.40826ms. These include draws/fills beyond byte publication, so they cannot be added to or equated with the narrower0.73274ms parent.

A future safe target is a scope-local immutable raw-resource extent proof that removes repeated Covers work while retaining EVERY producer Status check. Mixed/unknown producers must fall back. This is design only: no complete RAM-write version contract supports bypassing vertex snapshots or memcmp, and receipt/command-buffer lifetime still needs auditing before any proof cache. Tiny invalidation cost does not justify a speculative broad invalidation rewrite.

## QoS experiment closed: no convincing benefit

Actual requested QoS was33 for the GL owner/UI and21 for the private Metal worker. An opt-in copies the UI class/relative priority into the owned worker. SDK getter requires three arguments and returns an error code; the initial wrong-signature build is preserved and corrected before native execution.

Source4626230; one fresh GL/OFF/ON/ON/OFF/GL comparison, CPU categories and detailed timers OFF. ON validated requested class33; OFF21. All six selected correctness and cleanup gates passed. OFF process CPU18.2292/17.8006 versus ON17.7967/17.7809ms: the both-ON/both-OFF margin0.00392ms is much smaller than OFF drift0.42867ms. Worst frames still60.92–74.08ms. Keep this new inheritance flag OFF; do not promote the favorable first pair alone. Existing separate FastInterp QoS work is untouched.

## Next isolated timing request

Private source3a25a40 adds default-OFF CARA_NATIVE_METAL_LATENCY_ACTIVITY. It holds an Apple UserInitiatedAllowingIdleSystemSleep|LatencyCritical activity only during bounded active emulation and ends the token on normal/exception exit. It changes no limiter, guest clock, graphics work or queue depth. Production pause/background lifetime would need separate integration. Apple documents this as a request for higher timer/I/O precision, not a guaranteed speedup: https://developer.apple.com/documentation/foundation/processinfo/activityoptions/latencycritical

The first OFF/ON test keeps both row fixes1 and QoS inheritance0. Both children use the same pacing-only observer: existing saved timings, no new clocks, reserved48KiB, one postexit CSV. Full3000-row coverage/order, selected state/work/files and both panels are required. The isolated pair passed: mean16.8074→16.6661ms, p9923.1869→16.9942ms, worst61.5564→20.4163ms, slowest rolling120 average17.2520→16.6992ms. Maximum actual saved sleep52.7783→11.4920ms; sleeps above25ms2→0. Refresh2526/2706 sleeps52.778/45.986→11.492/11.222ms. CPU process18.0330→17.6652ms is one ordered pair and not a beyond-drift CPU win. Both panels at all four captures, selected guest state/work/files, class21 worker and owned exit/cleanup matched. This supports a final no-observer GL/Metal/Metal/GL confirmation, now complete below.

## Repro and preservation

Authoritative local evidence root: /Volumes/Fast/CaraBuild/cara-m1-metal-results-20261127.noindex.
Run the frozen test_latency_activity.py once only after its exact LATENCY-ACTIVITY-SOURCE-READY.json source/binary/harness/reference/ROM gates. Older closed brackets are not rerun or releases reused. All owned jobs stop on the first failure; no automatic native retry.

Preserved failures: ROM st_dev changed after reboot (same fullSHA verified, rebound before release); optional diskutil Mounted field absent (UUID/mountpoint checks corrected offline); publication local-struct array bound compiler failure; wrong QoS getter signature; missing replay import path before preflight. Each original failure/source remains retained. No failure was hidden by rerunning a native case.

Old quiet source-ready manifest4e61d154 remains historical/unconsumed; current binaries differ and must not reuse it. Loaded bracketcfafe45 and QoS2695f970 releases are consumed/closed. Docs/evidence/m1-metal-pacing-20261128.json binds actual local receipts. No ROMs, saves or screen payloads are committed.

## Final counter-free renderer confirmation

Source3a25a40, source-ready44366e717b18a2c1e792d4097c1e55e84fb93bb0fca1ecd44baa2fe414cad597. One GL/Metal/Metal/GL bracket, scoped activity1 for Metal, QoS inheritance0, both row fixes1; pacing CSV, CPU categories and detailed phase clocks OFF. All four complete correctness/cleanup gates passed. Exact same-renderer both-panel captures and full selected work/state/files matched. No cross-renderer upper-panel exactness claim.

| ms/refresh | GL first | Metal first | Metal final | GL final |
| --- | ---: | ---: | ---: | ---: |
| Mean | 16.60755 | 16.66557 | 16.66555 | 16.60772 |
| p99 | 17.63004 | 16.95396 | 16.97308 | 17.10583 |
| Worst | 20.90729 | 20.56308 | 20.34379 | 17.64838 |
| Worst rolling120 average | 16.70323 | 16.69984 | 16.69808 | 16.67713 |
| Owner CPU | 8.97924 | 13.68283 | 13.61991 | 8.94435 |
| Process CPU | 16.01496 | 17.85689 | 17.78160 | 15.94655 |

The repeated Metal runs no longer show the60–78ms oversleeps. Mean is within0.058ms of GL but remains slower. Observed Metal p99 is lower in both runs, but its0.13275ms between-controls margin is smaller than GL p99 drift0.52421ms: no beyond-drift Metal-over-GL claim. Process CPU still costs1.77–1.91ms more than GL; this request improves pacing and is not GPU offload. The original ordered OFF/ON CPU change is not a confirmed graphics CPU saving.

Observed background service work remained1.80–2.04 core-equivalents over capture2400→exit, with the same limited sampling scope described above. Services and volumes were left alone. All children exited0/reaped/group-empty with cleanup[]. The consumed bracket is closed in LATENCY-RENDERER-CLOSED.json and must not rerun unchanged.

Decision: keep CARA_NATIVE_METAL_LATENCY_ACTIVITY=1 together with both row fixes in the private M1 preset; preserve the previous preset, keep the new QoS inheritanceOFF and public/installed GL/GLES defaults unchanged. No Apple mobile activity policy is inferred from this macOS result. Remaining work targets measured authority validation and interval updates while preserving every producer Status and failure receipt. The public renderer is not switched by this experiment.
