# Guest GPU completion investigation

Private prototype based on Cara `abc3266`, 2026-11-13. P3D completion timing is
experimental and defaults off. No title addresses, cap cheats, host sleeps or
rendering skips are added. The read-only post-IRQ probe is separately selectable.
New serialized model/event state requires a newly compatible checkpoint.

## Implemented: post-IRQ logical-submission probe

Set `CARA_PICA_POST_IRQ_PROFILE=1` with FrameTiming enabled, or use the benchmark
configuration field `pica_post_irq_profile: true`. Normal play defaults off.
`ProcessCommandList` begins one logical submission; command-buffer jumps retain
that submission identity. Valid executed register writes after the first legacy
`trigger_irq` are counted, including extra parameters and jump destinations.

Per-window deltas and cumulative logs expose:

- `pica_post_irq_submissions`: submissions containing at least one IRQ trigger.
- `pica_post_irq_writes`: actual subsequent register writes, including later IRQs.
- `pica_post_irq_draw_requests`: subsequent indexed/nonindexed draw triggers.
- `pica_post_irq_jumps`: subsequent command-buffer trigger writes.
- `pica_first_irq_remaining_words`: sum of words remaining in the current buffer
  immediately after its first IRQ header/parameter has been consumed. This can
  include extra parameters and padding; it is not a count of executed writes.

The probe observes the current engine's unconditional register-0x10 IRQ path.
It does not infer hardware comparison/mask/AUTOSTOP state. No tail is scanned or
executed just to collect data. Aggregate counters reset with FrameTiming; the
transient submission census resets at each logical entry and is not serialized.
Logs are emitted every 256 entries. `compare_guest_clocks.py` treats missing
probe fields as unavailable, and automatic winner selection rejects this mode.

Falsifiable test: reproduce the same fixed-100%-clock Stories route, audio and
checkpoint without title cheats. Zero post-IRQ draw requests rules out tail
draw execution as the source of excess draw calls for that capture. Nonzero
counts justify a bounded trace of first-IRQ value/mask, following register IDs,
list boundaries and jumps before any AUTOSTOP behavior is changed. Remaining
padding words alone do not establish a bug. Repeat in Luigi, Zelda and Xenoblade.

## Pinned upstream source and attribution

Reference commit: Azahar
`5ddbaeae237954d1f9c1843e6cf75d8b160c2601`, merged via
[#2095](https://github.com/azahar-emu/azahar/pull/2095). The inspected files at
this revision explicitly carry Citra Emulator Project / Azahar Emulator Project
copyright and GPLv2-or-later notices:

- [PICA timing estimate and serialized accumulator](https://github.com/azahar-emu/azahar/blob/5ddbaeae237954d1f9c1843e6cf75d8b160c2601/src/video_core/pica/pica_core.h).
- [PICA command/vertex accounting and conditional stop](https://github.com/azahar-emu/azahar/blob/5ddbaeae237954d1f9c1843e6cf75d8b160c2601/src/video_core/pica/pica_core.cpp).
- [GSP pending completion events and relay queue](https://github.com/azahar-emu/azahar/blob/5ddbaeae237954d1f9c1843e6cf75d8b160c2601/src/core/hle/service/gsp/gsp_gpu.cpp).
- [Memory-transfer completion estimates](https://github.com/azahar-emu/azahar/blob/5ddbaeae237954d1f9c1843e6cf75d8b160c2601/src/video_core/gpu.cpp).

The probe is new Cara diagnostic code; the approximate timing model and relay
policy retain attribution to the pinned GPLv2-or-later implementation.
Current upstream master has later license notices; do not substitute it silently.

## Implemented: approximate P3D-only guest-time completion

Set `CARA_EXPERIMENTAL_P3D_COMPLETION=1`, or benchmark configuration
`experimental_p3d_completion: true`. Runtime, timing and benchmark reports expose
the mode; the benchmark independently checks it and refuses automatic winner
selection. This models guest interrupt visibility, not actual host GPU execution.

The implementation uses the pinned constants below with original PICA register
writes, original `pipeline.num_vertices` and primitive topology. Geometry-shader
output expansion and immediate-mode geometry are not modeled; these are explicit
accuracy limits inherited from the narrow heuristic. Integer fifth-nanosecond
arithmetic preserves the constants without floating-point accumulation drift.

With only this flag, only P3D is delayed. Original draw commands and guest-visible writes execute in
the existing order. Each completion deadline is the prior serial render deadline
or current guest time, whichever is later, plus modeled duration. Nanoseconds are
converted to 268,111,856-Hz guest ticks using checked quotient/remainder arithmetic
and ceiling rounding. No universal 16-ms delay or host-time measurement is used.

The queue holds at most 256 records with unique tokens, captured recipient thread
and registration generation. Register, unregister and client disconnect retire
prior generations. A due event for a retired generation never signals a replacement
waiter. FIFO/early-event/overflow violations and pending-record exhaustion stop
the experiment explicitly through `Core::System::ErrorUnknown`; they do not
deliver early or silently discard accepted work. Relay capacity is checked before
consuming a valid pending token, then delivery occurs on the same emulation thread.

The pinned relay policy is also integrated with the existing 64-byte shared ABI:
52 slots, PDC suppression at 32 queued entries, config bit-0 ignore behavior,
sticky full error, and framebuffer updates even for ignored/missed PDC. It does
not overwrite a full queue. Invalid headers stop explicitly without normalization.
This policy correction is independent of whether experimental timing is enabled.

The PICA estimate, serial render deadline, pending records, next token and recipient
generations serialize with the existing event name/userdata/FIFO ordering. Restored
callbacks bind to the restored service. Destruction unschedules only tokens owned
by that service while it still owns the current callback binding; late destruction
of a replaced service cannot cancel restored tokens with matching IDs. Teardown
clears the callback so it cannot retain a dead service. Loading an off-mode
checkpoint with an empty queue into on-mode is allowed. Loading pending on-mode
completions with timing disabled is rejected.

FrameTiming exports scheduled/delivered/retired counts, summed modeled nanoseconds
and summed enqueue-to-deadline ticks. Queued ticks are a guest-time estimate,
not measured host blocking or GPU time. Missing fields remain unavailable in the
clock comparison tool.

Native validation passes original-work estimates, checked conversion, serial
completion order, stale recipient rejection, record exhaustion and timeline
overflow. A real CoreTiming/bundled-Boost archive fixture saves active pending
events and partial PICA work, restores them with rebound callbacks, verifies FIFO
including an unrelated equal-deadline event, and checks owned-token cancellation.
The relay fixture exercises 1,835,257 policy/ABI cases plus one million FIFO
operations. This is not a physical-device or full guest/kernel save-state proof.
The P3D fixture also passes AddressSanitizer (leak checking unsupported here).
The combined undefined-behavior run reports bundled Boost serialization singleton
null-reference bindings during archive type registration; it is not a clean
UBSan result. Linked existing CoreTiming/Boost archives are not instrumented.

## Remaining accuracy and admission work

1. Upstream's estimate combines 22.4 ns/register write and
   a triangle estimate with 400 ns/triangle, 35% assumed culled at 20% cost.
   Those assumptions are explicitly approximate; the command cost is unmeasured.
   Original PICA counts/topology must be used, never expanded GLES `gpu_vertices`.
   Inspect modeled delays before drawing performance conclusions. Immediate-mode
   vertices and geometry output need measured extensions beyond this prototype.
2. The default-off `CaraExperimentalP3DCompletion` CoreTiming event uses stable
   pending tokens storing original recipient thread and recipient registration
   generation; the event type fixes the interrupt ID to P3D. PDC/VBlank retains
   its existing event. P3D schedules in guest cycles with checked conversion.
   DMA/PPF/fill remain immediate.
   Do not use host execution time or a universal per-frame delay. Reject stale
   tokens after unregister/re-register; do not deliver to a reused thread slot.
3. Extend engine ordering only with evidence: operation completion in a serial
   engine cannot overtake older work; independent transfer/render engines may
   overlap. A simple per-operation timer can reorder dependent submissions.
   Trace actual guest submission/dependency chains to distinguish these cases.
   Upstream's 300 µs service estimate is included for comparison with its P3D
   heuristic, but is not a measured universal backpressure constant. The
   game-specific transfer hack is excluded.
4. The old relay path incremented its byte count and overwrote a modulo-52 slot
   without a capacity check; the pinned policy replaces it. Hardware may drop VBlank events near
   its threshold and reports full queues. Preserve the guest ABI and distinguish
   hardware queue overflow from internal pending-event exhaustion. Never resolve
   internal capacity by silently dropping a draw or advancing an interrupt early.
5. Validate full guest/kernel save/load with timing accumulators, pending tokens/generations, engine deadlines
   and ordering state together with existing CoreTiming event names/userdata/FIFO
   order. Rebind restored callbacks to the restored service object, cancel on
   teardown, and exercise a checkpoint taken with pending P3D and PPF events.
   This changes save-state schema and requires a new compatible checkpoint.
6. Keep AUTOSTOP/byte matching independent of latency. Hardware allows IRQ-only
   continuation when AUTOSTOP bit 0 is clear. Support masks and CPU-only control
   register writes before changing command termination. Do not stop every list
   on every nonzero register-0x10 write.

Native fixtures cover equal-deadline FIFO, different P3D durations, recipient
removal/reuse, relay full/near-full behavior, pending-token exhaustion,
zero/extreme timing conversion, active-pending archive restore and teardown.
Full guest/kernel save-load, dependency ordering with immediate PPF/DMA/fill,
and real-game behavior remain unverified.

An upstream timing feature being available is not proof of a performance fix:
[#2274](https://github.com/azahar-emu/azahar/pull/2274) defaults it off because it
remains experimental and can reduce guest frame rates. Validate Stories, Luigi,
Zelda and Xenoblade at fixed 100% CPU and audio, with game progress, event ordering,
work counts and presentation cadence separated from wall-time throughput. Applied
top swaps at VBlank can coalesce multiple pending guest updates, so one swap per
refresh cannot by itself exclude guest render overproduction.


## PPF dependency extension after the Luigi falsification

The first physical-iPad Luigi P3D-only probe failed with pending-record exhaustion
at about 1.84 seconds: 365 scheduled completions in the first 60-refresh window.
The default-off control measured 16.722 ms/refresh, 11.569 ms CPU thread and 666
GPU draws/refresh. Zero post-IRQ draws in that capture excludes that particular
stale-tail explanation. P3D-only delay did not bound the producer; this does not
prove which guest waiter or other interrupt allowed more submissions.

The separately default-off `CARA_EXPERIMENTAL_PPF_DEPENDENCY=1` flag requires P3D
mode. Benchmark configuration is `experimental_p3d_completion: true` plus
`experimental_ppf_dependency: true`; reports independently expose both modes.
The extension shares the existing 256 records, recipient generation, stable
event type and unique tokens. Capacity was not increased. Records additionally
serialize their P3D/PPF kind and the transfer engine's deadline. Pending PPF state
cannot load with the dependency mode disabled. This changes the prototype state
schema again; make a fresh compatible checkpoint.

The pinned GPU transfer model reports hardware-measured TEX rates of 450 MiB/s
RAM-to-RAM, 3100 RAM-to-VRAM, and 5400 VRAM-to-RAM or VRAM-to-VRAM. Display-transfer
size uses input width times height times input format bytes/pixel; texture copy
uses the requested byte size. Duration uses checked integer upward rounding plus
the same approximate 300 microsecond service estimate. These are not DMA rates.
No title-specific extra texture-copy delay is included.

PPF completion starts after the maximum of current guest time, preceding render
deadline and preceding transfer deadline, then adds its transfer duration. Later
P3D work remains independent of an earlier transfer and may finish before it.
Consume/restore validation enforces each engine's order and the earlier-render
fence for PPF without requiring global token order across independent engines.
Transfer commands and memory writes still execute synchronously in the original
host path; the extension changes completion visibility only. DMA/fills remain
immediate. PDC continues on its existing VBlank event.

This render-to-transfer fence is an explicit conservative ordering hypothesis.
PR2095 supplies transfer measurements but schedules independent operation delays;
it does not establish this hardware dependency. Waiting for every earlier render
may overconstrain unrelated transfers. Conversely, a later render that consumes a
pending transfer is not modeled as dependent. Do not call this complete hardware
accuracy or claim that it fixes Luigi until guest/device evidence confirms it.

Native fixtures verify PPF cannot wake its modeled waiter before earlier render,
allow a later render to complete before earlier PPF, keep a synthetic PPF-gated
producer bounded for 10,000 iterations, reject arithmetic overflow, and preserve
mixed pending P3D/PPF kind/deadlines through the actual CoreTiming/Boost archive.
This synthetic producer assumes the PPF wait; it does not prove Luigi waits there.
Per-window `experimental_ppf_scheduled`, `experimental_ppf_delivered`,
`experimental_ppf_modeled_ns` and `experimental_ppf_queued_ticks` distinguish its
work from P3D. The existing retired counter covers retired events of both kinds.

Admission: fixed-100%-clock/audio matched-route controls on all four titles;
check bounded outstanding scheduled minus delivered minus retired, relay health,
game progress, pixels, original work counts and presentation cadence. Repeat
active-pending full guest/kernel saves with both kinds, unregister/re-register,
and teardown. Failure must remain explicit; no capacity increase, host sleep,
draw skip, early completion or universal frame delay is an accepted workaround.


## One CoreTiming owner for experimental completions

All P3D/PPF completion events now schedule explicitly on timer 0, relative to that
owner's tick count. Submission time is the maximum of submitting CPU time and
owner time. Callbacks consume using owner time, regardless of the last selected
CPU timer. This avoids cross-heap dependency reordering and the stale
`current_timer` selection during `KernelSystem::Advance`. Cross-timer ingress
can add scheduler lateness until timer 0 next advances; it cannot complete early.
No global CoreTiming or kernel timer-selection behavior is changed.

The native regression submits from CPU1/CPU2 at different times, then advances
only timer 0 while current_timer remains CPU1. A real archive off-mode checkpoint
with selected CPU2 is restored with enabled binding and `FinishStateLoad`, then
mixed render/transfer completions execute correctly on timer 0. Mutation checks
fail for both former implementations: callback current-timer clock fires early;
separate submitter event heaps fail to drain. Active pending checkpoints made by
the older split-timer prototype must not be reused; create a fresh compatible
checkpoint with this binary. Full kernel/service save-load remains unverified.

Off-mode checkpoints may contain legitimately completed relay entries and a
signaled guest event; restoring preserves these and may cause a startup burst
until drained. The model only finalizes accumulated render work at trigger_irq.
A no-IRQ draw list followed by transfer is not represented by the render fence;
compare logical-list and with-IRQ probe counts before assuming every submission
is finalized. Delayed completion does not block synchronous GX queue draining;
a guest producer waiting on another interrupt or polling may still outrun it.

## Physical-device rejection

The M4 Luigi checkpoint exhausted the 256 pending records with P3D-only timing.
Adding PPF dependencies and a single owner timer still exhausted the queue about
2.9 seconds after launch. Both modes remain disabled. Increasing queue capacity
would hide producer overrun rather than establish hardware behavior. Next work
must identify which guest wait/event gates production and whether render work
without a terminal IRQ bypasses the modeled fence. No performance gain or
cross-title compatibility is claimed for this model.
