# Stories tick polling and SVC overhead

Read-only source and diagnostic audit, 2026-11-13. No polling shortcut, clock
change, engine edit or device experiment is proposed as an established fix.
Vertex staging is being measured independently before choosing another change.

## What the retained blocks show

Private static analysis maps a coarse `SleepThread` path followed by a fine
`GetSystemTick` polling loop. The fine loop at `0x8586f0..0x8587c4` calls SVC
`0x28` through `0x29d1a0` and invokes a signed 64-bit-to-double conversion helper
twice per iteration. Five retained loop blocks each report 1,907,862 executions;
the retained conversion entry reports 3,835,714.

Those counts support the behavioral attribution. They are not host CPU samples,
milliseconds, whole-run function-call totals or a complete instruction census.
Cache replacement, redecoding and the bounded list of retained entries limit
coverage. Multiplying them by instruction costs cannot establish how much host
time was spent polling, how much speed could be recovered, or whether the
polling is incorrect. In particular, two conversion calls per iteration do not
justify replacing the loop with a single wait.

## Current timing contract

Source references are to the functions named below; line numbers can move.

- [`SVC::GetSystemTick`](../emulator/src/core/hle/kernel/svc.cpp) reads the
  **running core's** `Timer::GetTicks()`, then calls `AddTicks(150)`, and returns
  the earlier value in r0/r1 through the generic SVC ABI wrapper. It does not
  query a host wall clock. The comment attributes the 150-tick charge to an older
  o3DS measurement; this audit did not repeat that hardware experiment.
- ARM [`handle_Swi`](../emulator/src/core/arm/fastinterp/handlers/arm_misc.inc)
  and Thumb [`handle_ThumbSwi`](../emulator/src/core/arm/fastinterp/handlers/thumb16.inc)
  add the SVC instruction's decoded ticks, flush pending execution ticks into
  the timer **before** entering HLE, then reload PC and leave `ExecuteBlock`.
  [`ARM_FastInterp::Run`](../emulator/src/core/arm/fastinterp/fastinterp.cpp)
  checks the live downcount before executing more guest work.
- [`Timing::Timer`](../emulator/src/core/core_timing.cpp) applies downcount and
  CPU-clock scaling in `AddTicks`; `Advance` processes due events. A tick query
  can exhaust a slice even though the SVC itself does not block. Removing the
  existing exit/budget boundary can therefore alter event visibility and the
  number of polling iterations before an event runs.
- [`SVCContext::CallSVC` and `SVC::CallSVC`](../emulator/src/core/hle/kernel/svc.cpp)
  enter the HLE diagnostic category, take the global recursive HLE mutex, and
  dispatch through the SVC table and
  [`SVCWrapper`](../emulator/src/core/hle/kernel/svc_wrapper.h). The return path
  writes two registers through the core interface. Register marshaling and
  dispatch are potential host bookkeeping costs, not extra guest execution.

Preserve the running-core timer choice, returned tick sequence, 150-tick charge,
prior instruction accounting, mutex protection, register ABI, flags and event
boundaries. Switching to global ticks, removing the charge, increasing it to
escape the loop faster, or skipping guest iterations changes this contract.

## Existing host CPU evidence

The warm windows of private capture
`stories-user-slot2-trace-20261113/phase-costs/phase-diagnostic-frame-timing.json`
report these exclusive CPU categories per emulated refresh:

| Category | Thread CPU |
| --- | ---: |
| CPU execution including memory/backend/cache work | 10.091 ms |
| All HLE SVC work | 0.267 ms |
| GPU command processing including nested renderer work | 10.171 ms |

There are approximately 1,706 HLE entries per refresh. The diagnostic clock
calibration reports **111 ns per clock read**. Category transitions read that
clock and retain instrumentation overhead; these are instrumented measurements,
not exact production costs. The HLE number covers all SVCs, not only tick queries.
It bounds the likely scale of a pure HLE-dispatch improvement in this capture;
it does not bound the interpreter cost of the guest's surrounding arithmetic.

## Safe candidates and required measurements

1. Attribute sampled host CPU separately to the polling range, conversion helper,
   other guest execution, and HLE. Preserve nesting so conversion time is not
   added twice. Obtain a baseline with category clocks disabled as well.
2. If HLE dispatch is material, consider a shared SVC `0x28` fast dispatch inside
   the existing protected HLE path. It could reduce table/ABI indirection or
   repeated core lookups while retaining the same timer operation and backend
   return boundary. Compiler output may already eliminate some overhead; no
   benefit is established from source inspection.
3. If surrounding arithmetic dominates, evaluate generic interpreter operand or
   sequence optimizations that preserve every guest result and tick boundary.
   Do not introduce a Stories address check or a whole-helper replacement that
   silently omits guest instructions or timing.

Before promotion, compare ARM and Thumb tick-query loops with identical returned
r0/r1 sequences, untouched registers and flags, timer/downcount traces, and event
ordering. Include slice exhaustion immediately before/after a query, conditional
ARM SVC execution, supported clock scales, and another runnable thread. Then use
matched same-scene device runs with diagnostic clocks disabled, unchanged guest
work and required image/progression checks. There is currently no measured
same-timing SVC optimization gain.

## First on-device disjoint-range measurement

A fresh replay recreated the Secret Path camera/position from Slot 2, then saved
a compatible checkpoint for build `e81c6b491628b9ce8ee58a050b8d5fec577a88ce10099d0e6d8aa0c77e85997f`.
All runs used that same checkpoint, fixed 100% guest CPU, sound, existing timing
and FastInterp without JIT. Baseline/diagnostic/baseline wall means were
20.762 / 21.205 / 20.776 ms per refresh. Final pixels and GPU draw/vertex totals
matched exactly across the three runs.

The diagnostic sampled twelve complete four-refresh windows (48 refreshes),
with zero clock failures, frame resets, unassigned intervals, overflowing thread
entries or straddling blocks. Thread and bucket CPU sums reconcile exactly.
These are **raw instrumented costs**, not production timing or removable work:

| Disjoint location | Raw CPU ms per sampled refresh |
| --- | ---: |
| Fine polling-loop body | 0.891 |
| Signed64 to double helper, all callers | 0.513 |
| Double to signed64 helper, all callers | 0.255 |
| Signed64 to float helper, all callers | 0.286 |
| Tick-query guest wrapper | 0.318 |
| Forward-copy helper | 0.339 |
| Sphere/plane helper | 0.175 |
| Other guest locations, including dispatch/memory | 11.543 |
| SVC external spans, including nested HLE/renderer work | 9.716 |

All named polling/conversion work was on guest thread 1. The SVC bucket includes
6.470 ms on thread 1 and 3.241 ms on thread 11, principally identifying where
service-side work is entered rather than separating its renderer components.
It must not be described as 9.716 ms of kernel dispatch or scheduler overhead.

The sample totals 24.037 ms of instrumented Run-span CPU per sampled refresh.
Its 1,191,098 runtime clock reads and 117.714 ns/read calibration imply a rough
2.921 ms/refresh clock-read budget, not subtracted. Range classification, branch
and bookkeeping overhead are additional. This is too intrusive to treat tiny
helper timings as exact production costs or sum global helpers into an exact
loop-inclusive cost. A lower-transition episode probe is the next measurement.

Scheduling and other work outside Run spans remain unmeasured in this probe.
No wall/CPU remainder is labeled as scheduler time, and no loop skipping or
hardware timing adjustment has been made.

## Whole polling-episode measurement

The next build (`ec662a37137b88d5a20eada52bf2ed181500339e754a4af9b10643594d41a4de`)
recreated the Secret Path position and captured a new compatible checkpoint;
it did not bypass the original Slot 2 build fingerprint. The episode starts at
`0x8586f0`, ends at `0x8587c4`, and follows only the explicitly allowed loop,
conversion helpers, tick wrapper, and exception dispatcher. Helper calls and
the tick SVC retain ownership of the calling episode.

The same 100% guest CPU, sound, no-JIT and unchanged hardware timing settings
produced control/probe/control means of 20.824843 / 21.873052 / 20.939639 ms per
refresh. Final RGBA SHA-256 was identical in all three runs:
`27abdd96894604882b8b5374ad6e6acc45333c25b1e527fa6b623ae115e22004`.
Draw, vertex, command-list and swap workloads also matched.

All twelve four-refresh windows completed. No episode aborted, no sampled
work escaped its allowed ranges, and no clock, reset, assignment, capacity or
straddling-block failures occurred. Raw instrumented CPU costs were:

| Owned execution span | CPU ms per sampled refresh |
| --- | ---: |
| Polling episode, including its helpers and tick SVC | 0.689109 |
| Other guest execution, including interpreter and memory work | 9.940260 |
| Other SVC spans, including synchronous renderer/HLE work | 9.599421 |
| Total inside measured Run spans | 20.228790 |

The polling span contains 32,451,541 ns from fully sampled completed episodes
and 625,669 ns from five sample-boundary partial episodes. The totals reconcile
to 33,077,210 ns over 48 sampled refreshes. Episode start/completion/reentry
counts include unsampled execution and must not be used as the denominator
for an average sampled episode cost.

Clock calls fell to 37,535, with a rough calibrated read budget of 0.071371 ms
per sampled refresh. The complete observer still increased the run mean by
0.990811 ms (4.745%) against the mean of the controls: allowlist checks and
unsampled ownership tracking also cost CPU. No overhead estimate is subtracted
from the reported buckets. The narrower probe supports locating the polling
work, but does not establish exact production costs.

The fine polling episode alone does not account for the approximately 4 ms
shortfall in this scene. This does not prove its timing is hardware-perfect,
nor rule out other waiting loops. Outside-Run scheduler CPU remains unmeasured;
the SVC bucket and wall/CPU difference are not scheduler measurements. No
polling instructions, ticks, SVCs or hardware events were skipped.

Evidence: `stories-episode-cache-20261113/episode-comparison/comparison.json`
and `episode-profile-cpu-summary.json` in the private device profile archive.


## Decoded-block retention experiment

Chained execution increments block execution counts but bypasses normal cache
lookup recency updates. A default-off `CARA_FASTINTERP_CHAIN_RECENCY=1` experiment
examines execution-count deltas only on allocation misses, preferring inactive
ways and aging out stopped workloads. It does not add per-hit hooks or skip
instructions, ticks or service calls. Synthetic hot-conflict fixtures improved,
but the same-build Stories device comparison did not.

The same compatible checkpoint above, 100% guest CPU, sound, no JIT and no
hardware timing changes produced this probe-off ABBA comparison:

| Run | Wall ms/refresh | Emulation-thread CPU ms/refresh | Blocks decoded, whole 1,200-refresh run |
| --- | ---: | ---: | ---: |
| Control A | 21.069189 | 19.287512 | 2,623,500 |
| Chain recency A | 21.250842 | 19.426616 | 2,658,982 |
| Chain recency B | 21.369391 | 19.547645 | 2,658,982 |
| Control B | 21.185242 | 19.400926 | 2,623,500 |

Timing excludes the first 300 refreshes. The candidate average was 0.182901 ms
slower (0.866%), with 35,482 more decoded blocks (1.352%). All four final RGBA
hashes and graphics workloads matched. Audio stayed enabled and running, but
underrun counters were nonzero; this is not a clean-audio result. The candidate
remains disabled by default. This establishes neither a general cache-policy
regression nor hardware timing equivalence outside the tested trace.

The first attempted comparison was rejected by a harness field-name mismatch
(`cache_chain_recency_enabled` versus the engine's `cache_chain_recency`). The
harness was corrected to require the emitted field, with missing/wrong-mode
regression coverage. The complete ABBA above was rerun with the same installed
engine binary; no rejected-run timing is included in its averages.

Evidence: `stories-episode-cache-20261113/cache-comparison-v2/comparison.json`
in the private device profile archive. Native cache-conflict/hot-loop traces
matched with the policy off/on at four and eight ways. ARM/Thumb profiler
on/off equivalence and the 22,000-case differential suite passed locally before
the device runs; this does not claim GitHub CI results for unpushed changes.

### Per-set replacement diagnostics

Separate cache-counter runs locate the churn on core 0. Both policies allocate
in 2,043 of 2,048 sets, but only 280 sets replace live blocks. In the control,
128 sets account for **93.9%** of live replacements; the top 64 account for
67.8%. Set 881 is the largest with 51,147 replacements (1.96% of the total),
followed by set 830 with 48,012. This is concentrated conflict/working-set
pressure under the current placement, rather than evidence that the whole
16,384-entry cache is full or that one set explains the problem.

| Whole diagnostic capture | Control | Chain recency |
| --- | ---: | ---: |
| Allocation misses / decoded blocks | 2,623,500 | 2,658,982 |
| Live replacements | 2,611,861 | 2,647,343 |
| Empty allocations / final residents | 11,639 | 11,639 |
| Sets replacing live blocks | 280 | 280 |
| Top 128 sets' share of live replacements | 93.9% | 93.2% |

Neither run reports cache invalidations, mode redecodes or execution-counter
wrap/reset discontinuities. The candidate improves 125 sets by 34,644 misses
in total, while 145 sets worsen by 70,126: the net increase is 35,482. The same
high-churn sets remain prominent, consistent with the unchanged address-to-set
mapping. These observations do not establish that another hash or replacement
policy will improve this workload. The uninstrumented ABBA result above remains
the performance evidence, and the candidate stays off.

Victim/new-PC records are limited to the first and last 64 live replacements,
not a frequency census or random sample. All 64 final candidate victims have
zero observed execution-count change since the preceding full-set miss; that
confirms the policy selected interval-inactive candidates in that small tail,
not that those blocks were unnecessary or would remain cold. Legacy interval
activity is unavailable, and cumulative victim counts do not establish current
hotness. The resident top-block list likewise omits evicted history. Decode CPU
milliseconds have not been measured, so miss counts alone cannot quantify its
frame-time cost or forecast a speedup.

Validated counters, PC-to-set sample hashes and bounded-sample checks are retained
in `stories-episode-cache-20261113/cache-diagnostic/cache-replacement-comparison-validated.json`
alongside the two original diagnostic reports in the private profile archive.
