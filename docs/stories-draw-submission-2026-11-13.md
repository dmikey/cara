# Stories draw submission, 2026-11-13

Original Stories Slot 2, 100% guest CPU, sound and unchanged guest timing.
These are instrumented host-thread measurements, not hardware GPU timings or
uninstrumented gameplay speed results. No renderer code changed.

## Evidence

Private run directory:
`/Volumes/Fast/CaraBuild/profiles/stories-episode-cache-20261113/draw-guest-next/phase-draw-profile/`.
Copied summaries: `stories-draw-cost-next.json` and `stories-draw-phase-next.json`,
originally generated under `/private/tmp/`.

Source: `DrawCostProfile` in
`emulator/src/video_core/renderer_opengl/gl_rasterizer.cpp` and phase sampling in
`emulator/src/common/frame_timing.h`. Summarizers:
`tools/summarize_draw_cost.py` and `tools/summarize_phase_cpu.py`.

## Submission distribution

Draw-cost session 2 has 18 complete snapshots. First-to-last boundary differences
cover 1,114,112 calls and 2,648,914,227 submitted vertex entries, with 34,815
successful samples, approximately one in 32 separately per path/bucket. Zero
clock failures. The first boundary and trailing partial interval are excluded;
this does not share the phase summary's exact 900-refresh boundaries.

| Accelerated array vertex count | Full calls | Full vertex entries | Sampled submission CPU share | Mean sampled CPU / wall, us per call |
| --- | ---: | ---: | ---: | ---: |
| <=64 | 22.040% | 0.146% | 19.123% | 3.530 / 4.009 |
| 65–256 | 6.579% | 0.368% | 2.737% | 1.694 / 1.543 |
| 257–1024 | 16.447% | 5.037% | 9.196% | 2.275 / 2.124 |
| 1025–4096 | 30.921% | 28.981% | 25.970% | 3.417 / 3.267 |
| >4096 | 20.724% | 65.384% | 41.605% | 8.169 / 8.016 |

Software arrays contribute 3.290% of calls and 1.370% of sampled submission CPU.
No indexed or feedback submissions occur in the covered interval. Accelerated
draws larger than 1024 entries account for 67.575% of sampled submission CPU.
Tiny draws also show a meaningful fixed-cost component. Shader/state/content
vary by bucket: this does not prove causal vertex scaling or batching safety.

## Sampled phase intervals

The phase summary covers 900 measured refreshes after excluding 300 warmup
refreshes, in 15 windows. Sampling is approximately one in 64 calls separately
per phase. All sampled phases have zero clock failures.

| Phase | Successful samples | Mean wall / CPU, us per sampled call | Sampled CPU / wall |
| --- | ---: | ---: | ---: |
| Draw submission | 13,776 | 4.608 / 4.205 | 91.254% |
| Vertex subimage | 524 | 3.798 / 3.567 | 93.922% |
| Stream map | 4,966 | 1.978 / 0.173 | 8.753% |
| Uniform upload | 14,244 | 0.719 / 0.314 | 43.715% |
| Stream unmap | 4,966 | 0.269 / 0.146 | 54.316% |

Submission and vertex updates include substantial host-thread CPU execution.
Stream mapping has the largest sampled wall-minus-CPU interval, 1.804 us/call;
that does not prove GPU waiting. Descheduling, driver behavior and measurement
overhead can contribute. Stream map/unmap can nest in uniform upload: do not sum
these as exclusive phases. No readback/orphan samples occur. Texture diagnostics
were not selected; zero texture rows do not mean zero acquisition work.

Both profilers retain clock overhead and cover different sampling intervals and
boundaries. Draw-cost CPU includes wall-clock reads; tiny calls can report CPU
exceeding wall. Its aggregate sampled CPU is 141.669 ms versus 141.235 ms wall.
No overhead subtraction, variance or confidence intervals are available. Do not
extrapolate sampled totals, divide them by refreshes to claim complete phase
cost, or interpret CPU-minus-wall as exact waiting. Diagnostic frame times are
not uninstrumented speed comparisons.

## State reuse investigation

The existing `CARA_GL_STATE_REUSE=1` option is the best-supported isolated test.
It avoids roughly 20 otherwise unconditional raster API calls per full
`OpenGLState::Apply` and caches linked-program vertex-layout uniform location and
values. Drawing invokes full state application twice. Canonical controls leave
this option disabled.

Source review found constructor/init invalidation and correct partial tracking:
`SubApply` updates only applied fields and never validates an invalid full
snapshot. Production raw tracked-raster initialization is guarded. iOS
presentation saves/restores raw scissor and color-mask changes. Linked programs
remain resident for shader-manager lifetime, matching the layout cache lifetime.
This supports testing, not automatic promotion. The earlier small comparison is
in [stories-gpu-trace-2026-11-13.md](stories-gpu-trace-2026-11-13.md).

Set `CARA_VERTEX_SELF_TEST=1` with `CARA_GL_STATE_REUSE=1` in a fresh process.
`emulator/src/ios/main.mm` runs `RunGPUStateSelfTest(error) &&
RunGPUVertexSelfTest(error)` after making the context current, writing
`Documents/cara/gpu-vertex-self-test.json`. No separate state-self-test flag
exists. Failure short-circuits vertex tests. The state oracle checks full and
unchanged apply, partial apply/restore and manual mutation with invalidation.

## Completed baseline/reuse/reuse/baseline comparison

Independently checked receipts:
`/Volumes/Fast/CaraBuild/profiles/stories-episode-cache-20261113/draw-guest-next/state-reuse-comparison/comparison.json`
and sibling `state-reuse-oracle/state_reuse_oracle-gpu-vertex-self-test.json`.

| Run, chronological | Wall ms/refresh | Emulation-thread CPU ms/refresh |
| --- | ---: | ---: |
| Baseline A | 20.793390 | 18.985729 |
| State reuse A | 20.631548 | 18.819503 |
| State reuse B | 20.536617 | 18.723995 |
| Baseline B | 20.962779 | 19.134828 |

The two reuse runs average 0.294002 ms/refresh less wall time, a 1.4082% reduction
relative to the baseline mean. Detailed phase CPU/category diagnostics were off;
pacing was enabled. Both comparison flags `renderer_rgba_identical` and
`matched_workload` are true. All four final raw captures have SHA-256
`27abdd96894604882b8b5374ad6e6acc45333c25b1e527fa6b623ae115e22004`.
All share engine fingerprint
`ec662a37137b88d5a20eada52bf2ed181500339e754a4af9b10643594d41a4de`
and checkpoint SHA-256
`9b4f4260f12a83da727cb6dc4df44aad1d35fef927156cec312252fbc5b911a6`.
This is final-frame/workload equivalence, not comparison of every intermediate
frame or a guarantee of artifact-free audio.

The separate fresh-process oracle comparison explicitly selects state reuse;
its combined state/vertex report has `passed: true`, empty detail, and no reported
fixture failure. This is a small measured improvement in this Stories replay.
Cross-game validation remains pending; the default has not been promoted.
No new batching or texture-retention mechanism was introduced.

## Upstream code research and next isolated port

[Azahar PR #2318](https://github.com/azahar-emu/azahar/pull/2318), merged August
11, 2026, contains implementation commits `b4b6fb2eebe8e896d783a4e9cd73bf8c2b80eaa3`
and `96cf35bf5d72ec2ec8edb1e1395ac7e27b876d29`. Its public patch was inspected
locally at `/private/tmp/azahar-2318.patch`. It introduces register metadata,
command/write batching and vectorized shader-stream comparison. This is real
portable optimization code, not a Stories-specific fix or a measured Cara gain.
The upstream [release notes](https://github.com/azahar-emu/azahar/releases)
attribute an average 0.5–1 ms/frame reduction to their command-processing change;
that result does not transfer to Cara's backend or this replay.

Cara already has masked backing-register writes, scalar packed-float queues,
raw special-register payload handling, program/swizzle mirroring, LUT dirty
notifications and an optional unchanged-shader-state experiment. Cara still
processes each extra command word through `WritePicaReg`. Unlike newer Azahar,
Cara synchronously notifies its renderer rather than accumulating ordinary dirty
register ranges. The upstream deferred synchronization architecture therefore
cannot be transplanted as an isolated loop optimization.

A minimal future port should begin default-off with repeated-register extra-data
streams for program/swizzle uploads, preserving the original scalar header and
using the old handler for ordinary registers, immediate attributes, draws,
interrupts and jumps. Float-uniform stream batching can follow independently.
Do not bundle previously unsuccessful unchanged-write suppression with the new
experiment. Per-write pointer progression, diagnostics and callbacks must remain
observable, or diagnostics must explicitly select the original scalar path.
No engine implementation was made during this source-frozen build.

Local semantic differences requiring explicit preservation:

- Cara mirrors VS program/swizzle to GS whenever exclusive configuration is off;
  upstream additionally tests geometry-shader use. Preserve Cara's condition.
- Capacity-crossing program/swizzle streams currently accept the valid prefix;
  whole-range rejection changes behavior. Fall back to scalar handling.
- Lighting index wrap follows per-word bitfield increments. Do not replace this
  with an upstream range assertion that rejects wrapping uploads.
- Backing registers receive masked values, but special stream handlers consume
  raw command payloads. Mask zero still permits special-register effects.
- Pre-advancing a whole batch changes command-position observations. Keep jumps,
  IRQs, immediate draws and post-IRQ remaining-word accounting scalar.
- Shader generation trace increments and profiling counters belong to logical
  writes. SIMD copying must preserve them or use scalar diagnostic fallback.

Before target testing, compare scalar/candidate state after every logical write:
all registers, uniform queues and values, shader bytes/offsets/dirty state/hashes,
LUT contents/indices, and ordered renderer/draw/IRQ/jump/debug/trace effects.
Exercise all 16 masks; grouped and repeated extras; incomplete float24 triples
and float32 quads across chunks; invalid indices; capacity edges; all VS/GS
exclusive/use-GS combinations; unchanged words, signed zero and NaN payloads.
Include lighting/fog/proctex wrap, header/extra-word jumps, multiple IRQs and
post-IRQ work, alignment, truncated payloads and aborted lists. Keep the actual
original handler as the differential oracle, add independent expected cases,
and run ASan/UBSan. Only then compare matched target workload, pixels and sound
without changing timing or skipping draws.

## Other upstream findings

The Monster Hunter blur workaround in
[Azahar PR #1525](https://github.com/azahar-emu/azahar/pull/1525), implementation
`ea29b0f` / merge `275d818`, writes a transparent fragment for unimplemented GAS
mode instead of discarding it. Cara already does this in
`emulator/src/video_core/renderer_opengl/gl_shader_gen.cpp` around line 1689.
The later [Vulkan port, PR #2165](https://github.com/azahar-emu/azahar/pull/2165),
is not a new GLES optimization. Neither establishes a fix for this performance
bottleneck.

A useful reverse-engineering reference is the MH4U project's
[screen-filter research](https://github.com/CompaLarios18/MH4UltimateDC/blob/main/docs/research/screen-filters.md).
It describes locating MT Framework filter classes through DTI initialization,
constructors and virtual tables, with filter names such as `uBloomFilter` and
`uDOFFilter`. This can guide attribution of Stories' own passes using its own
binary; MH4U addresses and layouts are not transferable. Skipping those passes
changes the picture and is outside this optimization's scope.

Searches of Citra/Lime/Azahar, Panda3DS and Mikage did not establish a public
Stories-specific performance fix ready to transplant. The direct
[Panda3DS Stories report](https://github.com/Panda3DS-emu/Panda3DS-Games-List/issues/99)
records a launch failure, while
[Azahar/Lime issue #195](https://github.com/azahar-emu/azahar/issues/195) records a
weapon-upgrade stall without a merged remedy. Mikage's
[2022 technical report](https://mikage.app/year-in-review-2022/) discusses
control-flow analysis, internal shader bytecode and resource ownership; these
are architectural references, not Stories measurements. This search is bounded
and does not establish absence of other useful work.


## Scalar double RAM experiment

A new compatible checkpoint recreated the same Secret Path position for build
`990b1ac66652edd30d2e65eb314397e0b60c267b1184d2a2e5525c143ec2d54d`.
Original Slot 2 and previous signed apps remain preserved; no fingerprint check
was bypassed. `CARA_FASTINTERP_RAM_BURST=1` now also combines scalar double loads
and stores when all eight bytes use the same ordinary mapped RAM page. Null,
tracked, MMIO, crossing and wrapping accesses retain ordered word fallbacks.
The switch also enables pre-existing RAM burst paths, so this comparison does
not isolate the added scalar-double optimization.

At fixed 100% guest CPU, sound, no JIT and unchanged timing, with the first
300 of 1,200 refreshes excluded from timing:

| Run | Wall ms/refresh | Emulation-thread CPU ms/refresh |
| --- | ---: | ---: |
| Control A | 20.613750 | 18.880771 |
| RAM burst A | 20.573631 | 18.836555 |
| RAM burst B | 20.892620 | 19.068847 |
| Control B | 21.016520 | 19.163210 |

The point estimate is 0.082010 ms (0.394%) faster, while controls drift by
0.402770 ms. This short comparison does not establish a reliable speed gain;
the experiment remains off by default. All four final RGBA hashes matched
`27abdd96894604882b8b5374ad6e6acc45333c25b1e527fa6b623ae115e22004`,
and graphics workload matched. Sound was enabled and running; nonzero underrun
counters remain, so this is not a clean-audio result.

The targeted 4,224-case actual-handler suite passed, including exact registers,
predication, bits and ticks; unaligned/cross-page/wrap; ordered MMIO and throwing
second-word partial completion; unmapped and tracked RAM. The broader 22,000
ARM/Thumb differential suite passed with RAM burst enabled. Native and iOS
builds and the iOS no-JIT audit passed locally. CI is wired but these changes
have not been pushed or validated on GitHub.

Evidence: private `stories-scalar-double-20261113/comparison/comparison.json`,
`scalar-double-native-test.log`, and `scalar-double-full-differential.log`.


## Named filter trace rejected

The own-image class/CFG work supplied eight disjoint instruction-body ranges
for bloom, DOF, multi-blur, common filter setup/quad, and draw state helpers.
An on-device trace collected all 48 requested sampled refreshes with zero clock
failures and unassigned intervals, but reported 668 decoded blocks straddling
configured boundaries and `coverage_complete=false`. Its per-function CPU
costs are rejected. Instruction/function extents are insufficient to guarantee
the interpreter's decoded block boundaries fit the profiler's intervals.
The static class mappings remain hypotheses for attribution; no filter cost,
active-pass ownership or redundant rendering conclusion follows from this run.
Private evidence: `stories-scalar-double-20261113/named-filter-profile/`.


## Corrected LDM-PC return comparison

The rejected function trace led to a confirmed generic decoder/liveness defect:
LDM-PC uses the register list for its PC write, while the old block-end check
looked for rd==15. Unreachable decoded instructions after an unconditional
return could influence backward dead-flag elimination. An authored synthetic
ADDS/LDM-PC/unreachable-CMP witness returned the right PC/value but wrong NZCV.

Precise mode now preserves flags across every LDM-PC possible exit. It shortens
only unconditional AL LDM-PC blocks when chain-budget mode is enabled. It keeps
conditional and chain-budget-off block shapes to preserve scheduling boundaries.
A broader interim version changed tiny-budget event lateness and was rejected.
The final 112-case timing probe found zero legacy/corrected differences in PCs,
consumed ticks, instruction markers and event lateness across tested modes,
predicates, cold/warm caches and budgets. Six separate chain-budget-off cases
verified corrected flags with unchanged spans/PCs/ticks. Native targeted tests
(136), full differential cases (22,000), scalar-double regression cases (4,224),
iOS build and no-JIT audit passed. Precise mode defaults on; explicit zero is a
comparison/diagnostic escape, not a recommended game setting.

A fresh compatible checkpoint recreated Secret Path in build
`e2de13d1fc475d99cf0c747e2def5f0d6370399ed0d8a51b3a51b24f1fe501b0`.
At 100% guest CPU, sound and unchanged clock/event policies:

| Run | Wall ms/refresh | Emulation-thread CPU ms/refresh |
| --- | ---: | ---: |
| Legacy A | 20.901386 | 19.137904 |
| Precise A | 20.813455 | 19.029317 |
| Precise B | 20.862442 | 19.047667 |
| Legacy B | 20.973072 | 19.111285 |

The point estimate is 0.099281 ms (0.474%) faster; this short run is not a
statistical confidence interval or evidence of a large speed unlock. All final
RGBA hashes and graphics workload matched; allocation/decode counts remained
2,623,500 in every run. This is primarily a demonstrated correctness fix with
a small measured timing difference. No required guest work was skipped.

Private evidence: `stories-precise-return-20261113/comparison/comparison.json`,
`precise-return-final-native-test.log`, `precise-return-final-differential.log`,
`ldm-budget-probe-final.summary.json` and `ldm-chain0-flags-proof.json`.


## Corrected named-function trace and combined stack

With precise return mode enabled, the same eight-range probe completed all 48
sampled refreshes with zero straddling blocks, clock failures or unassigned
intervals and `coverage_complete=true`. Raw body-only CPU ms per sampled
refresh: bloom 0.053108, common filter setup 0.003482, filter quad 0.005889,
draw-layer state 0.039327, viewport state 0.025763, target state 0.030914; DOF and
multi-blur bodies had zero sampled CPU. This does not assign callee/SVC/GPU work
to a pass or prove those passes never run outside the sparse windows. Shared
helper bodies also include other callers. No overhead estimate is subtracted.
The trace had 239,456 clock reads at 91.2698 ns/read; classification/bookkeeping
cost is additional. Evidence: `stories-precise-return-20261113/named-filter-profile`.

The combined stack keeps the established packed GPU inputs, an 80 MiB upload bank,
upload/index reuse, surface+byte remapping, selective cleanup, uniform reuse,
ALU operand specialization, tick buckets, eight-way cache, chain budget and bounded
pacing. Precise return handling is enabled in every run. State reuse and RAM
bursts are the remaining tested variables. All tests use 100% guest CPU, sound,
100% speed cap, no JIT and unchanged hardware timing policy.

| Configuration | Wall ms/refresh | Emulation-thread CPU ms/refresh |
| --- | ---: | ---: |
| Corrected baseline A |20.710050|18.900159|
| State reuse only |20.608592|18.763991|
| State reuse + RAM A |20.472015|18.674131|
| State reuse + RAM B |20.494088|18.697098|
| Corrected baseline B |20.954117|19.108637|

The two combined runs average 20.483051 ms versus 20.832083 ms for the bracketing
controls: 0.349032 ms or 1.675% less wall time. The repeated combined runs differ
by 0.022073 ms; controls drift 0.244067 ms. This is a modest checkpoint-specific
measurement, not proof of 100% speed or a cross-title performance profile.
State-only has a single run here, so the incremental RAM contribution remains
less well constrained than the full-stack comparison. The rejected chain-recency
policy, used-input pruning and timing experiments remain disabled.

All five final RGBA hashes matched `27abdd96894604882b8b5374ad6e6acc45333c25b1e527fa6b623ae115e22004`;
graphics workload matched. Sound was running with nonzero output; underruns
remain at this sub-full-speed workload. A separate full-stack run passed GPU
state and vertex, surface-coherence and uniform-reuse oracles, loaded a compatible
checkpoint and produced the same final pixels. The combined settings were left
running interactively on the iPad; experimental defaults are not globally
promoted by this test.

Evidence: private `stories-precise-return-20261113/stack-comparison/comparison.json`
and `stack-oracle/`. Source commit `cbed54810339a20f819a1948057f24e96251e7fc`
passed all four jobs in GitHub run 43484126572: iOS software, iOS GLES3, macOS
software and Go relay race checks. The published code has no ROM/firmware data.


## Decode cost and successful-lookup traffic

A temporary diagnostic build (`f7221d5437073a4c6d501b23ba7d7a99515aa0199c1eb5c0b288c7d17e722f1c`)
recreated Secret Path with the same combined stack, 100% guest CPU, sound,
100% speed cap and unchanged guest timing. Off/on controls preserved exact
register/tick/event/cache-victim traces over 1,024 native collision slices;
the full 22,000-case differential suite, iOS build and no-JIT audit passed.

All three device runs had identical final pixels and graphics workload.
Baseline/probe/baseline measured 20.558649 / 20.393665 / 20.383700 ms per refresh
in the 900-frame timing window. The probe being below the mean controls is
ordinary observer/run variation, not a speed improvement.

The lifetime diagnostic covers all 1,200 completed refreshes, including warmup
and state restoration; it cannot isolate the 900-frame benchmark interval.
Frame observations include both endpoints (0 through 1200), with no gaps or
resets. Core 0 was active; the other cores were unused.

| Counter | Whole-run value |
| --- | ---: |
| Allocation-miss decodes | 2,623,500 |
| Mode redecodes | 0 |
| Chain-fallback Lookup calls | 127,553,678 |
| Chain-fallback PC-tag hits | 125,042,682 (98.031%) |
| Run LookupOrAllocate calls | 30,788,399 |
| Run PC-tag hits | 28,164,899 (91.479%) |
| Successful chained dispatches | 443,921,870 |
| Dispatches through fallback Lookup | 125,042,682 (28.168%) |

Together the two lookup entry points receive about 131,952 requests per
completed refresh. Tag comparisons total 631,842,158, or about 526,535 per
completed refresh. These counts do not measure lookup CPU time.

The ordinal-hash sampler selected 40,667 allocation-miss decodes (1.550%).
All samples completed, with zero drops, invalid clock deltas or clock failures.
Raw sampled means were 384.239 ns CPU and 350.210 ns wall per decode. Empty
calibration brackets averaged 136.688 ns CPU and 125 ns wall; neither is
subtracted. The overhead-inclusive, sample-weighted estimate is 0.840 ms CPU
per whole-run refresh. This estimate assumes representative ordinal sampling,
includes clocks and other observer effects, excludes unsampled mode-redecode
cost and is not an exact production or benchmark-window cost.

The data makes re-decoding a secondary target rather than an explanation of
the entire 4–5 ms shortfall. The much higher successful-lookup traffic warrants
a separate hit-path experiment that preserves full-PC/mode/slot validity and
NRU replacement updates. No lookup speed gain is established by these counts.
The tested app and compatible full-stack checkpoint were restored after the
capture; instrumentation is not the user-facing performance result.

Private evidence: `stories-decode-cost-20261113/comparison/comparison.json`,
`cache_profile-fastinterp-blocks.json`, and `decode-cost-analysis.json`.
