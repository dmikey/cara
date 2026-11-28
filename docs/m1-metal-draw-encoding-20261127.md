# M1 JIT Metal draw encoding: measured breakdown

Private investigation, 2026-11-27. Public rendering defaults and installed apps
are unchanged. No overall Metal advantage over OpenGL is established here.

## Workload and measurement

Normal-boot Luigi's Mansion: Dark Moon USA, identical scripted A presses through
3,000 refreshes, four JIT guest cores, fixed CLK100, cap100, 1x, SDL sound,
full reference/actual GPU vertex work, no software fallback or skipped draws.
The CPU window is 2401–2800. Actual native LCD checkpoints are 800, 1600,
2400 and 3000. This is the retained Gadd lab replay, not a mansion gate traversal.

The earlier broad draw-encoding scope measured about 2.1 ms/refresh. Eight added
exclusive thread-CPU scopes identify where that work goes. Their additional
clock reads add overhead; do not add their numbers to the old 2.1 ms or treat
the instrumented replay as a quiet benchmark.

| Exclusive diagnostic category | CPU ms/refresh |
| --- | ---: |
| Validation | 0.233 |
| Uniform/stream/layout upload | 0.282 |
| Vertex/index texture upload and lookup | 0.831 |
| Encoder setup and stale bindings | 0.296 |
| Raster state | 0.243 |
| Vertex bindings | 0.250 |
| Fragment bindings | 0.216 |
| Actual draw call | 0.368 |

All original executable draw code is restored byte for byte by removing the
inserted scope statements. The selected LCDs matched retained captures. The
first build wrapper referenced the wrong patch filename and failed before any
compiler started; its failed receipt remains. The corrected wrapper built the
same reviewed observer source successfully.

## Upload path: equality costs more than transport

A separate existing detailed host-span profiler, without CPU-category clocks,
reported these cumulative values over the whole 3,000-refresh process:

- 3,029,793 vertex-cache hits and 43,435 misses (98.6% hit rate).
- 3,275,911 exact byte comparisons covering 24,783,292,160 bytes: 0.981 s of
  instrumented host spans. These are not GPU time or exclusive CPU time.
- Hashing: 0.082 s; transport into new cached textures: 0.0033 s.
- 43,051 evictions from a maximum of 384 entries.

These lifetime counters include startup, load and other scenes. They must not
be divided by the 400-refresh CPU window or assigned to every individual draw.
The existing alignment, recent-entry and per-refresh ordinal hints are already
on through the shared Apple Metal defaults. This is not evidence that uploads
are rebuilding every texture or that the GPU is saturated.

## Closed experiment: repeated rejected probes

An isolated default-off candidate reused an already failed ordinal/recent byte
comparison if the same entry appeared in the later hash bucket. Its ON replay
never emitted the actual activation witness, so the gate correctly failed.
The exact selected state, work, LCDs and files still matched offline, and both
native processes exited cleanly. No gain or quiet comparison follows from this.
Normally a failed byte candidate has a different hash, so a later revisit would
require a collision or an incompatible shape. Route closed; no retry.

## Ordered eviction: local saving canceled by hit maintenance

A second isolated candidate maintained bounded intrusive oldest/newest links
in existing cache nodes. It retained every successful exact-byte comparison,
copy, cache limit and ownership rule. Both full OFF/ON diagnostic replays passed
selected state/work/LCD/file checks; ON emitted its actual eviction witness.

| Diagnostic CPU, ms/refresh | OFF | ON |
| --- | ---: | ---: |
| Victim selection | 0.03878 | 0.01917 |
| Remaining vertex upload/lookup | 0.77906 | 0.79627 |
| Combined vertex upload and eviction | 0.81783 | 0.81543 |
| Owner thread | 18.59462 | 18.64008 |

Maintaining the ordering links nearly canceled the scan saving. One ordered
pair under background load does not establish a statistical regression either.
Keep this experiment OFF; it is not a justified performance promotion.
Each measured window still issued 278,153 draws, 4,782 command lists and display
transfers, 3,985 texture copies and 6,376 fills; skipped/software draws were zero.

## Host contention identified

The original high-CPU apfsd PID571 accumulated about 183 CPU-minutes over 139
minutes of uptime. The user terminated it. The replacement PID17159 was briefly
idle, then resumed about two cores of CPU load. New unified-log records identify
`com.apple.apfs.defrag.disk7s1` and `com.apple.apfs.defrag.disk7s2`: APFS
maintenance on `/Volumes/Backups` and `/Volumes/Files`, which share disk6s2.
This is a dispatched maintenance-job witness, not a completed stack analysis
or evidence of a broken filesystem. No agent stopped a system service.
Time Machine was not running; internal storage had 36 GiB available and Fast
421 GiB. The earlier observed internal disk traffic alone did not identify the
cause and must not be relabeled as proof of this external-disk maintenance.

The bounded 121.8-second quiet admission failed without starting any benchmark.
A separate final all-candidates-OFF OpenGL/Metal/Metal/OpenGL baseline also
failed its first quiet admission, without starting a native process. Its exact
source/binary-hash preflight is frozen in `BASELINE-JIT-SOURCE-READY.json`; it
will need fresh sustained quiet before any replay.
The background-service gate remains 50% of one logical CPU. It was not weakened
to obtain a result. Existing upper-screen precision/edge differences between
Metal and OpenGL remain unresolved; same-renderer selected parity cannot imply
cross-renderer pixel equality.

## Sampler reuse: correctness passed, no meaningful gain

A third isolated default-off candidate retained immutable sampler identities
on one encoder, resetting them on pass close/abort and clearing stale slots.
It avoided a setter only for the same sampler on that same encoder. All other
bindings, validation, GPU work and queue policy remained intact.

Both 3,000-refresh OFF/ON diagnostic replays exited cleanly; selected state,
work, both native LCDs and guest files matched. ON emitted the actual reuse
witness and OFF did not. Combined vertex/fragment binding CPU changed only
0.48190 to 0.47691 ms/refresh. Owner CPU changed 18.62843 to 18.64832 ms,
while process CPU changed 22.52944 to 22.48949 ms. Mean wall time was
18.70132 versus 18.70755 ms. This single ordered pair under background load
establishes no overall benefit or statistical regression. Keep it OFF.
All three candidates remain private-build experiments; none is promoted into the
production renderer by this investigation.

## Remaining larger opportunity

The larger upload opportunity needs a stronger ownership contract, not weaker
memcmp checks: the current binding storage is reused and mutated on subsequent
draws. Pointer equality therefore cannot prove unchanged bytes. Any future
snapshot identity must cover every CPU/GPU write and alias, constants, padding,
read-plan offsets and save/load invalidation before bypassing the second check.

## Reproduction and evidence

Local evidence root:
`/Volumes/Fast/CaraBuild/cara-m1-metal-results-20261127.noindex/`.
Use `profile_draw_cpu.py` for exclusive scopes and `profile_vertex_upload.py`
for the separate detailed counters. Never enable both observer types together.
`diagnose_vertex_lru.py` binds exact source/binaries before the OFF/ON pair;
`vertex-lru-QUIET-ADMISSION.json` preserves the blocked comparison.
Private source and original failures remain archived at
`archive/m1-metal-profile-20261127`, revision
`6b2bd51` (draw scopes, rejected-probe, ordered-eviction, then sampler reuse).
The production sampler and eviction defaults remain OFF. No ROM/save/image
payload is committed. See the small machine-readable evidence alongside this doc.
The review branch contains documentation and receipts only; failed or neutral
experiments have not been represented as production fixes.
