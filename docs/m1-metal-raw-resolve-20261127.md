# M1 JIT Metal: raw-buffer resolution CPU cost

Status: **experimental, default OFF; native diagnostic correctness passed**.
No end-to-end Metal speedup or advantage over OpenGL has been established.

## Measured lead

A finer private M1 owner-thread CPU profile separated the previous broad binding
and transfer categories. Byte resolution measured 2.176 ms/refresh and draw
encoding 2.119 ms/refresh. Raw vertex capture measured 1.114 ms, byte publication
1.065 ms, surface/LUT acquisition 0.874 ms. These are exclusive thread CPU
categories with diagnostic clock overhead; they are not GPU durations or
additive components of wall time. The exclusive sum was 18.417 ms versus an
18.505 ms owner clock, leaving 0.087 ms outside those scopes.

Observer correction: the first finer report's `metal_bind_uniforms` field was
misnamed. Source inspection confirmed `Measure(7)` is per-level texture decode,
including the base level. Its 0.607 ms belongs there, not to uniform packing.
The original report is preserved; the private observer now calls that category
`metal_bind_mip_decode`. Uniform packing remains within binding's remainder.

The old stack sample showed the byte-resolution path issuing many small blits
and repeatedly reading source-buffer metadata/status. The new change targets
the latter; it does not replace or omit the GPU copy commands.

## Candidate

Exact opt-in: `CARA_METAL_REUSE_RAW_RESOLVE_PROOF=1`. Multi-span resolutions may
remember metadata for at most two retained raw producers during that one call.

- Native ownership enumeration still checks every clipped interval.
- Every remembered span still checks source bounds literally.
- Queue/device/tracked storage and immutable backing metadata are validated
  before the first use; producers and failure status are rechecked before
  encoding the assembled blit plan.
- The snapshot retains the writer leases and buffers. No proof survives the
  function call. Other producers retain the original path.
- Single-owner/direct-borrow routes retain their original behavior.
- Every original copy's source, offset, destination, size and order remain.
  Guest memory, shader code, vertex/quaternion work, draws, transfers, queue
  adaptation, validation receipts and synchronization are unchanged.

The production function body on this branch matches the hardware-tested private
source exactly after removing its observer-only CPU scope. The branch as a
whole on latest main has not been rebuilt by this receipt.

## Actual M1 diagnostic pair

Two fresh normal-boot Dark Moon USA processes used four actual JIT guest cores,
fixed guest CLK100, cap100, native 1x, SDL sound, identical A presses through
refresh 3000, no software vertex fallback or skipped draws. Measured categories
cover 2401–2800; selected native LCD captures are 800, 1600, 2400 and 3000.
Both arms used the same binary and instrumentation. Fused retile, retile-owner
memo, scan-status reuse and persistent writer completion caching were OFF.

| Diagnostic metric, ms/refresh | OFF | ON |
| --- | ---: | ---: |
| Byte-resolution CPU | 2.154 | 1.762 |
| Owner-thread CPU | 18.131 | 18.068 |
| Process CPU | 22.584 | 22.893 |
| Mean wall | 19.004 | 19.464 |
| p99 wall | 21.899 | 35.111 |

The local resolution category fell by 0.392 ms (18.2%). Overall owner CPU fell
only 0.063 ms, while process CPU and frame tails increased in this single
ordered diagnostic pair. Background activity, frequency/cache/scheduling and
profiling overhead were uncontrolled. Do not promote this flag or describe the
18.2% as a whole-emulator speedup. No statistical regression or causality for
the tails follows from this pair either.

All 3,001 observed tick/global-tick, four core-PC and selected work rows matched.
Both LCDs matched exactly at all four checkpoints, and guest SD file hashes
matched. Each scored work window issued 278,153 draw requests, 4,782 command
lists and display transfers, 3,985 texture copies and 6,376 fills, with zero
software vertex or skipped draws. Both native groups exited 0 and reaped
without signals. ON emitted the actual raw-proof reuse witness; OFF did not.
These are selected state/work/output checks, not complete VRAM or per-draw
payload equivalence. [Small receipt](evidence/m1-metal-raw-resolve-20261127.json).

## Quiet comparison interruptions

The fixed filesystem-daemon load was idle on the next visit. A first matched
OpenGL control was interrupted by a sync burst before the measured interval.
The reviewed follow-up explicitly logged unscored startup separately while
requiring quiet from capture 2400 through exit, with app/lock/compiler guards
continuous. Its first OpenGL control was interrupted in that interval at a
reported background 65.9% CPU against the declared 50% threshold. No Metal arm
or valid complete scored comparison started. Both partial receipts remain;
only owned jobs were stopped, and no system service or user app was signaled.

The observed post-reboot workload also included `FPCKService`, `corespotlightd`
and `mdworker_shared` helpers. The host-quality watchlist now includes these;
earlier admission snapshots must not be relabeled as exhaustive quiet proof.
The threshold has not been relaxed to obtain a result. New test data resides
outside Documents on the build volume.

The first observer build falsely flagged short-lived SDK helpers in its exact
isolated Popen process group as foreign because their intermediate parent
exited during inventory. The failed receipt and cleanup are preserved. The
reviewed build monitor now accepts that exact isolated group plus live ancestry
for separately grouped Ninja children. It still rejects foreign groups and
checks all installed Cara apps/locks. The same source then built successfully.

## Remaining work

Complete a fresh OpenGL / Metal OFF / Metal ON / Metal ON / Metal OFF / OpenGL
comparison only after sustained quiet, with strict same-renderer LCD/state/
work/file checks and explicit cross-renderer differences. Keep the existing
upper-screen precision/edge discrepancy visible; no tolerance or swizzle may
be used to claim exact parity. Compare both ON readings against both OFF
controls, their drift, total process CPU, tails and cost-shift indicators.

Draw encoding remains about 2.1 ms of diagnostic CPU. Its per-draw resource
validation and buffer/texture/sampler bindings are the next measured area to
split. The normal OpenGL worker overlaps submission with emulation; Metal still
prepares and encodes substantially on the emulation owner. Moving a wait or
reducing setter counts alone is not proof of a performance gain.

Full local evidence: `/Volumes/Fast/CaraBuild/cara-m1-metal-results-20261127.noindex/`
(`fine-cpu-build-1` failed monitor, `fine-cpu-build-2`, `fine-cpu-diagnostic-1`,
`raw-resolve-build-1`, `raw-resolve-diagnostic-1`, `matched-quiet-1`, and
`raw-resolve-matched-1`). Private instrumented source is preserved on
`archive/m1-metal-profile-20261127` at `686b7ca`.
No ROM data, guest saves or captured game images are committed.
