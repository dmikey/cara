# Guest frame production and GPU completion

## Scope

Investigate work produced by the guest CPU and PICA command stream before moving
to Metal. The target is full emulation speed at 100% guest CPU with audio and no
CPU JIT. A reusable engine change must preserve guest-visible rendering, waits,
interrupts, memory effects and game progression across titles.

## Measurement: Stories grass replay

Measured 2026-11-13 on a physical M4 iPad using FastInterp and native GLES3. Runs
start from the same private comparison checkpoint, lock touch input and replay
stick movements over 1,200 emulated refreshes. The first 300 refreshes are warmup;
the remaining 900 are measured. Audio output was enabled and verified nonzero.
The original clock experiment was uncapped to measure throughput; the later
internal-setting experiments capped emulated time at 100%.

### Fixed guest clocks

The clock comparison used 100/50/50/100 run order. Final pixels and the recorded
render workload repeated identically within each clock group. Across clocks,
pixels and workload differ, so this is a workload diagnostic.

| Per emulated refresh | 100% guest CPU | 50% guest CPU |
| --- | ---: | ---: |
| Elapsed time | 24.025 ms | 13.954 ms |
| Emulation-thread CPU | 22.042 ms | 12.966 ms |
| Accelerated draws | 918.961 | 493.486 |
| Submitted vertices | 2,281,176 | 1,224,618 |
| Input/index subimage uploads | 302.933 | 244.154 |
| Input/index bytes uploaded | 2,962,045 | 2,919,709 |

A separate instrumented capture reports exclusive CPU categories:

| Exclusive thread CPU per refresh | 100% guest CPU | 50% guest CPU |
| --- | ---: | ---: |
| CPU execution, including memory/decode | 10.635 ms | 5.954 ms |
| GPU command processing, including nested renderer work | 11.663 ms | 7.187 ms |

These scopes partition CPU time. They must not be added to overlapping wall-time
fields such as `guest_cpu_ms_per_frame` or `draw_ms_per_frame`. Diagnostic clock
and bookkeeping overhead is included. This split does not isolate dispatch from
memory or decode work.

Direct request counters in a subsequent capture show:

| Per emulated refresh | 100% guest CPU | 50% guest CPU |
| --- | ---: | ---: |
| PICA command-list entries | 14.550 | 7.804 |
| Indexed/nonindexed PICA draw triggers | 956.517 | 513.624 |
| Applied top-buffer swaps | 1.000 | 1.000 |
| Applied top swaps with distinct nonzero eye addresses | 0.000 | 0.000 |

The 3D slider was zero and host presentation used the left top buffer plus the
bottom screen. Intermediate render targets can still receive additional work.
GSP applies dirty shared-memory presentation updates at VBlank, coalescing queued
updates. Applied swap counts therefore cannot establish unique images or count
all guest-produced frames. A PICA draw trigger is also distinct from a displayed
frame and may belong to a shadow, filter, antialiasing or other intermediate pass.

### Internal frame-limit experiment

The original [Stories code author's report](https://gbatemp.net/threads/monster-hunter-stories-fps-cap-30-60-and-improved-rendering-codes.538299/)
identifies a 200-fps internal cap and publishes 30/60-fps controls. The published
codes target EU 1.0 and leave USA compatibility uncertain. A bounded, read-only
probe verified the mapped pointer and an actual `200.0` field in the tested USA
copy, title `00040000001BC500`, before any write.

A temporary Gateway data file guarded the exact observed pointer and original
value, changed the cap in RAM, and left the ROM and in-game saves untouched.
The comparison checkpoint was restored before each case. At frame 300 the probe
confirmed `60.0`; guest CPU and maximum emulation speed both remained 100%.

| One replay per setting, per emulated refresh | Original 200 cap | Internal 60 cap |
| --- | ---: | ---: |
| Emulation-thread CPU | 21.465 ms | 9.247 ms |
| Accelerated draws | 918.961 | 284.863 |
| Elapsed time, including pacing | 23.360 ms | 18.423 ms |

This is evidence that guest frame-production policy can produce substantial
extra CPU and rendering work. It does not establish an engine-wide fix, unchanged
animation timing, or a general gain across scenes. Final pixel hashes differ;
game progression and visual behavior still need assessment. The experimental
presentation build also had both submission and aggregate frame pacing active;
CPU time remains useful, while its elapsed timings are not a final pacing result.

The rendering-preset experiment separately reduced draws, but the exact-value
check failed: the requested `0x089F036A` became `0x009F036A` after guest processing.
It was stopped rather than treated as a validated preset. The author describes
these as compound FSAA-related flags; a later poster labels the preset as
stereoscopic disabling. Its quality and handshake behavior require investigation.
No combined-setting result is established. The temporary cheat file was verified
empty afterward; later engine comparisons restore the untouched checkpoint.

## Engine implications

- Host display pacing and guest render production are separate. A host guard
  spaces actual `presentRenderbuffer` submission starts at the configured period
  (about 16.7 ms at 100%). It preserves guest draws and interrupts. Use one pacing
  owner per presentation path and exclude host pacing sleep from adaptive CPU
  workload estimates.
- Local `GPU::VBlankCallback` already calls renderer `SwapBuffers()` once per
  emulated VBlank. `SetBufferSwap` updates framebuffer registers rather than
  invoking scene rendering. Repeated buffer addresses do not prove unchanged
  pixels, since the guest can write new contents to the same allocation.
- Local P3D and transfer completion signaling is synchronous, without an explicit
  guest GPU processing interval. A game can observe completed GPU work and become
  runnable again within the same guest slice. Trace submission, completion,
  wait/wake and VBlank guest timestamps before selecting a timing model.
- [Azahar #2095](https://github.com/azahar-emu/azahar/pull/2095) adds GPU completion
  timing and interrupt-queue fixes, benefiting Luigi's Mansion 2. Its 3D model is
  approximate. [Azahar #2274](https://github.com/azahar-emu/azahar/pull/2274) later
  disables timing simulation by default because it remains experimental and can
  reduce game frame rates. This is prior art to validate, not a ready speed fix.
- [GBATEK's FINALIZE research](https://mgba-emu.github.io/gbatek/#3ds-gpu-internal-registers---finalize-interrupt-registers)
  specifies byte comparison/masking and optional AUTOSTOP. The legacy path signals
  on `trigger_irq` and continues. Count real writes/draws/jumps after IRQ before
  changing command termination. Stopping every list at any IRQ is insufficient.

The temporary Stories setting must not become a universal draw skip or a hidden
per-title address patch. The core investigation concerns correct completion
ordering, bounded queueing/backpressure, command termination, and reuse of
provably equivalent work.

## Cross-title validation

| Title | Required coverage | Current status |
| --- | --- | --- |
| Monster Hunter Stories | Moving grass replay; game progression, audio and intermediate passes | Clock/request diagnosis and temporary cap experiment captured |
| Luigi's Mansion: Dark Moon | Slow opening with lighting/effects | Requested engine comparison pending |
| The Legend of Zelda: A Link Between Worlds | Known gameplay/control baseline | Requested engine comparison pending |
| Xenoblade Chronicles 3D | Opening and known moving scene; depth/lighting/audio | Requested engine comparison pending |

Use 100% guest CPU, 100% maximum emulation speed, native resolution, sound and no
JIT. Keep title settings at baseline for engine comparisons. Report real scene
progress, compatible checkpoint identity, CPU time, guest command counts, audio
state and presentation spacing. Preserve in-game saves and keep ROM bytes,
checkpoints, screenshots and raw dumps outside Git.

Private evidence is retained under `/Volumes/Fast/CaraBuild/profiles/` in
`clock-scaling-20261113`, `clock-scaling-requests-20261113`,
`stories-settings-probe-20261113`, and `stories-internal-settings-20261113`.
See also [performance prior art](performance-prior-art.md).
