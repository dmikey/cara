> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# Slide authoritative color-fill materialization

Private Android Atlas experiment on physical RG Slide (Unisoc T820, Mali-G57). JIT, fixed guest CLK 100%, GLES at 1×, Adaptive Frame Skip off. Persistent UBOs, raster-state reuse, layout reuse and packed-fetch reuse stay on in both arms. No public default or player data change.

## Change and proof

The retained transfer trace showed two lazy color-fill materializations per refresh initializing 400 rows, then separately the remaining 16 rows of the same 256×416 attachment. At the first materialization, the same fill allocation still authoritatively contains every byte of the destination. The private candidate initializes the complete attachment at that point, and publishes those already initialized host bytes as valid. Later real guest draws, transfers and alias invalidations retain their existing order and behavior.

Expansion is restricted to a fully invalid, registered, noncustom RGBA8 color surface at 1×, mip 0, equal width/stride, exact whole source/destination address interval, valid whole source and successful existing `CanFill` proof. The existing fill conversion and scissor implementation perform the work; ownership maps, guest fill commands, shader mathematics, draw counts, guest ticks and fence semantics are unchanged. Default is off. Depth is excluded.

Frozen private source: `ecd968d5cf14fde655774b4fc7e47df3cc46adab`, based on diagnostic JNI commit `7e84146bc2cc938b2e884621a08e539ab44f0704`. `SOURCE-FROZEN.json` and `CANDIDATE.patch` bind the exact files. Portable byte oracle passed 2,000 cases / 64,000 subsequent writes and invalidations / 520,238,164 byte checks, with 20 negative guard families. This is host-model proof, not physical GL output proof.

Guarded build attempt 9 passed in 25.135 s with 47 proper-host installed-app quiet checks. APK SHA-256 `c55d0f71f2868cc28c77e30520d53eb83517d55bbd2597950b4b328d44962792`; engine fingerprint `1f3965dc5aa72015fe0accad6ca84ce55e39f16732beee9c2ca3a6b604043e25`.

## Genuine scene and controls

The changed core fingerprint requires a genuine new normal-boot save. `physical-3/seed/RECEIPT.json` binds successful native save, title/backend and new fingerprint; archive SHA-256 `d86d6089ef95fc0845afb23fa62efd3f1e745d13f3cb8972506e98b55a0287dc`. It is a Gadd desk/ghost lab phase, distinct from the older retained ghost closeup. No timing comparison between those seeds is valid. No state header bypass or foreign state was used.

All arms clone the same local shader archive SHA-256 `5734cd2bf6b0d277d000bb9484b125081f62bb94d35ad1beabb5ad531c9489df`. Before any candidate run, unscored W0 showed a fragment-build transition in window 601. `RUNTIME-PLAN.json` therefore prospectively scores warm windows 481 and 721, separately reports 601, and still requires every original workload count including those shader builds to match. The driver's private cache is preserved and its warmth uncontrolled. Fresh process does not mean cold driver.

## Physical correctness evidence

C0 OFF and recovery C2 ON both completed restore and normal Exit Game, allowing bounded trace export. All 5,386 guest GPU trace rows are identical, including clocks, sequence and submission fields. All original work counters at 481/601/721 are exact. Frame 720's 393 sampled draw payload records, vertex/index/element and VS uniform hashes and attachment metadata are exact. Two actual GLES debug observers report no GL errors.

Across traced refreshes 719–720, fill materializations fall from **22 to 18**, while the actual initialized byte total remains exactly **5,562,368 bytes**. Four full 416-row clears replace four 400-row clears plus four 16-row tail clears. Every copy has a valid capture-generation/frame join, with GPU trace counter one behind the VideoCore counter. The trace is complete with no copy/detail/bucket/join truncation or unknown allocation identity.

Both physical screenshots were inspected: Gadd, lab and ghost effects render and the lower maroon panel is exact. Top screenshots were captured after the timing endpoint while emulation continued, so their camera/animation phases differ. Exact frame-aligned top pixel parity is **unverified**; do not describe this candidate as pixel-perfect verified. Trace and byte proofs do not substitute for that missing physical pixel check.

The bounded validation-call wall sum was 1.061924 ms OFF versus 1.060502 ms ON over two refreshes, CPU 1.081465 versus 1.074695 ms. These are instrumented return scopes, not physical GPU time or a performance benefit.

## Preserved setup and export failures

The first source-staging include anchor failed before compilation and is retained. Seed attempt 1 incorrectly applied the historical seed's draw-count range to the new normally booted scene; it stopped before saving. Seed attempt 2 reached Gadd but used a template folder missing the confirmation images and stopped before writing an archive. Both are retained; genuine attempt 3 passed after correcting setup alone. Candidate C1 completed its windows and activation but missed the manual normal-exit export deadline; it is a failed observer attempt and receives no timing verdict. C2 is the disclosed recovery, with the successful destructor trace export preserved. An unused auto-exit draft's strict image preflight failed; it did not modify the actual C2 harness or engine.

## Quiet performance

The declared O1 / F1 / F2 / O2 quartet completed once, all full gates PASS. Actual policy activation was witnessed in both ON arms; candidate remains OFF after this result.

| Warm window end | OFF O1 / O2 p50 (ms) | ON F1 / F2 p50 (ms) | OFF mean → ON mean |
|---|---|---|---|
| 481 | 27.96 / 28.07 | 28.01 / 28.16 | 28.015 → 28.085 (+0.070 ms) |
| 721 | 27.22 / 27.17 | 27.61 / 27.02 | 27.195 → 27.315 (+0.120 ms) |

Neither window shows both ON cases faster than both OFF cases. P99 and worst tails are mixed, with no consistent improvement. The frame-720 tracked draw-state mask chunks are also identical, despite four fewer color-fill materializations: this does not prove any internal driver render-pass reduction.

Window 601 compile return time is 523.60 / 8.43 / 9.08 / 8.09 ms in order, with the same 31 shader compilation requests. This is the deliberately separate shader/driver-warmth transition. Its first-control cost is not counted as a fill benefit. Primary windows have zero shader compilation in every case. `quiet-bracket-1/ACTUAL-REPORT.json` retains every p50/p99/worst index, phase and CPU counter plus OFF drift.

Decision: **close this candidate default OFF; no consistent matched warm speed benefit**. Do not repeat or broaden to depth/tile variants on this evidence. No extra hardware-counter pair was run because the prospectively declared gate required a quiet benefit first. Earlier GPU saturation observations are from earlier exact seeds and are not relabeled as measurements of this candidate. The code cuts API materializations while touching exactly the same bytes, but the trace's roughly 0.53 ms per refresh return scope and unchanged draw-state sequence did not translate into a whole-workload win.

Restore the original fixed private APK and retained best UBO/state/layout/packed-fetch profile/cache after the quartet; preserve candidate source, APK, genuine matching seed, trace and failed setup receipts. `RESTORE-STABLE.json` records actual final device state. The player app remains unchanged.
