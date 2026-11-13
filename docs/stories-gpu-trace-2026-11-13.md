# Stories slowdown: guest GPU dependency trace

Two complete 16-VBlank iPad captures compare the lighter opening 3D sequence
(stage 2, local frames 1560–1575) with the heavier dialogue sequence (stage 3,
3960–3975). Both use FastInterp, 100% guest CPU, 100% speed cap, active sound,
GLES, GPU byte remapping and selective cleanup. Experimental P3D/PPF delays and
texture prefilter are disabled. No guest timing behavior was changed by the probe.

Engine fingerprint: `cd7947a46c374f1cec4c93c58059f4c10f7c10238b54c293d55c592fa8856927`.
Private evidence: `profiles/stories-causal-trace-20261113/stage{2,3}` under the
local build root. Game data and command traces are not checked into the repository.

## Observed work

| Across 16 refresh intervals | Lighter | Heavier |
| --- | ---: | ---: |
| GPU command lists | 241 | 241 |
| P3D completion requests | 241 | 241 |
| PPF completion requests | 80 | 80 |
| Top swaps | 16 | 16 |
| Draws, including CPU vertex path | 8,097 | 18,361 |
| GPU-path vertices | 16,311,879 | 44,862,261 |
| Submitted list bytes | 8,229,024 | 10,762,432 |

The exact draw-count distribution changes from 107 lists × 55 draws and
53 × 29 to 107 × 143 and 53 × 45. The remaining 54 × 1 and 27 × 23
are unchanged. Thus the additional 10,264 draws occur inside the same-frequency
list classes, rather than through an increased submission/completion rate.
These classes describe observed draw counts, not proven equivalent render passes.

Both traces are complete, untruncated, have no foreign-thread drops and use
current timer domain 0. All captured ring heads/selected slots are zero and
command counts are one. The ring-index contract discrepancy is not exercised here.

P3D/PPF requests and relays occur inside GX execution at the same recorded guest
tick in both scenes. This documents current HLE behavior; it does not establish
real hardware completion latency. Waking an event waiter makes it ready, not
necessarily immediately running. Direct relay/acquire-or-wake/run/same-producer
next-GX observations are 54 in the lighter trace and 53 in the heavier one.
They do not show an increased wake/submission loop in the heavier scene.

## Cost and limits

The containing 60-refresh windows measure 16.704 versus 25.031 ms wall time,
and 13.324 versus 21.502 ms emulation-thread CPU time per refresh. Draw-scope
wall time rises from 6.046 to 12.142 ms; draw submission from 2.054 to 5.085 ms;
CPU shader execution from 0.270 to 1.468 ms. Scopes overlap and must not be summed.
Neither window reports shader compilation or surface readbacks. These are
instrumented diagnostic observations, not a speedup benchmark.

The iPad surface-coherence fixture passed before the replay. Each capture saved
and restored a compatible checkpoint, completed successfully, and recorded active,
unmuted audio without an audio API error. This is not a listening-quality verdict.

Conclusion: the observed slowdown is associated with substantially heavier work
per list, not more swaps, lists or completion events. This does not prove all
rendering necessary or the existing completion timing hardware-accurate. Repeated
addresses do not prove identical command contents. Next, attribute the expanded
lists by framebuffer, shader, geometry and render state to distinguish legitimate
passes from repeated equivalent work; compare against hardware evidence before
changing completion timing or eliminating draws.

## Scripted dialogue advance and input-polling hypothesis

A follow-up restores the same heavy checkpoint and presses A at refresh 120,
then every 180 refreshes through 1380, with the same fixed clock and sound.
The window ending 120 has 68,345 draws and 26.13 ms/refresh; the next window
has 37,255 draws and 21.38 ms/refresh. Advancing changes substantial rendering
work, so this is not an isolated measurement of input-polling overhead.

The light, heavy and advanced 16-refresh traces contain identical signal counts
for objects 92 and 93 (63 each), and objects 297 and 79 (55 each). Neither trace
contains a WaitPoll record. These object IDs alone do not establish their service
identity; explicit HID labeling is being added. Guest shared-memory polling does
not call the kernel wait API and remains unmeasured by these observations.
Thread 1 timeout wake counts are 430, 481 and 478 respectively; these must not be
called HID polling without identifying the preceding sleep/wait request.

Cara currently schedules HID pad updates at 234 Hz and signals both pad/touch
handles after each update, even without a changed button. HID update notification
is distinct from a button-change event. The reverse-engineered interface documents
shared-memory update notifications and a sampled input ring:
[HID handles](https://www.3dbrew.org/wiki/HID:GetIPCHandles),
[HID shared memory](https://www.3dbrew.org/wiki/HID_Shared_Memory).
The code's 234 Hz rate is not independently validated by those interface pages.

## Hardware-facing reuse and synchronization prior art

Pinned [citro3d drawElements](https://github.com/devkitPro/citro3d/blob/9f21cf7b380ce6f9e01a0420f19f0763e5443ca7/source/drawElements.c#L29-L43)
explicitly clears the post-transform vertex cache after drawing. Its
[FrameBegin](https://github.com/devkitPro/citro3d/blob/9f21cf7b380ce6f9e01a0420f19f0763e5443ca7/source/renderqueue.c#L163-L175)
waits on the preceding GPU queue. These show explicit cache handling and software
backpressure in a hardware-facing library. They do not prove Stories uses the
same strategy or that hardware discards duplicate draw commands. Even equal
geometry can produce distinct depth, stencil or blended-color effects.

## Draw partition follow-up

The additive draw/HID captures are complete four-refresh windows at post-restore
VBlanks 1560–1563 and 3960–3963: 7,761 and 16,160 records, respectively, with no
truncation or dropped foreign-thread observations. Each guest indexed/nonindexed
draw trigger records its target, vertex count, capture-local shader-change
generations, selected render state and input/texture addresses. Raw trigger
vertices below differ from the renderer's expanded GPU-vertex counter above.

| Captured work over four refreshes | Lighter scene | Heavier scene |
| --- | ---: | ---: |
| Guest draw triggers | 2,037 | 4,809 |
| Raw trigger vertices | 1,377,118 | 3,929,838 |
| Draws to color `0x180d4800`, depth `0x183fd400`, 256 × 512 | 1,652 | 4,403 |
| Raw vertices to that target | 1,375,578 | 3,928,214 |
| Draws across the other nine target/dimension partitions | 385 | 406 |

The dominant target accounts for 2,751 of the additional 2,772 draws (99.24%)
and 2,552,636 of the additional 2,552,720 raw vertices (99.997%). Remaining
partitions use four-vertex draws. All ten partitions share the recorded depth
address; one color address appears with both 256 × 512 and 64 × 128 dimensions,
showing why address equality alone cannot identify a render pass. These shorter
windows contain 61 versus 63 command lists due to their sampled phases; the
paired 16-refresh captures above remain the stronger list-frequency comparison.

Exact equality of all captured draw/state/input fields produces 54 two-draw
groups in the lighter capture and 28 three-draw groups in the heavier capture.
Every such group consists of adjacent 382-vertex draws inside one GX list; none
crosses a list or refresh boundary. If every extra member of the heavier groups
were eventually proven redundant, those particular candidates would account for
56 draws: 1.16% of heavy work, or 2.02% of the measured draw increase. This is
not an upper bound on all possible redundancy: shader generations identify
observed changes within a trace and cannot identify equal shaders across reloads.

The candidates are not established duplicates. Attribute-loader offsets,
component mappings and strides can select different geometry from the same
recorded vertex base/index range. Uniforms, viewport/scissor and buffer contents
are also absent. A next bounded probe can record actual uniform-value-change
and vertex-layout-change generations at existing write points, without hashing
buffers per draw. No candidate draw has been removed and no completion timing
behavior has been changed for this investigation.

## Explicit HID bindings and sleep requests

Both four-refresh captures identify HID pad/touch events as kernel objects 92
and 93. Each capture contains 16 HID updates with neutral pad state zero and
16 signals **per event**, but no waits, acquisitions or signal wakes on either
HID event. Neither capture contains a WaitPoll record. HID callback lateness is
0–28 guest ticks in the lighter window and 0–10 in the heavier window. These
observations do not show an increased HID notification or kernel-wait loop in
the heavy scene. Direct guest polling of shared input memory remains unmeasured.

Threads 1 and 11 have 110/66 versus 122/70 scheduled timeout wakes, while neither
thread issues a SleepRequest in either capture. Those wakes must not be labeled
sleep or HID polling: other mechanisms, including scheduled HLE IPC wakeups,
can produce them. The observed explicit sleeps belong to other threads (100 ms
requests by threads 4 and 12, and 50 ms requests by thread 14). A timeout Wake
record alone does not establish the reason a thread was suspended.

## User-selected Slot 2: Secret Path gameplay

A subsequent user-selected slow scene was preserved from Slot 2 before any
restart. The original screenshot shows 18% speed at 100% guest CPU. The private
backup and checksum are under `profiles/stories-user-slot2-trace-20261113`.
The slot itself was not overwritten. Engine fingerprint remains
`ca034c202d576062cba9e79ef4d12d51ac55b3cba73eb552fc75d546b21169cb`.

Restoring this state with normal renderer defaults, sound and fixed 100% guest
CPU did not reproduce the 18% reading. A 2,400-refresh replay with no scripted
input averaged 20.719 ms wall and 18.830 ms emulation-thread CPU per refresh over
windows ending 120–2280. Window means ranged 19.915–21.866 ms. Those windows had
zero surface readbacks and zero shader compilations. Exclude startup and the
checkpoint-save operation at 2340; save overhead is not steady rendering cost.

Early/late four-refresh draw traces are complete: 3,973/4,270 draws, of which
3,765/4,046 target the main framebuffer. These short windows have different
phase alignment and are not proof of accumulating work. The severe original
slowdown remains unresolved: restoration resets host-side state and this replay
cannot prove what caused the original live-session reading.

Normal interactive play was restored from the original state with continuous
60-refresh timing and audio diagnostics enabled, so a later severe dip can be
observed before restarting. No timing constants, input semantics or rendering
work were changed for this investigation.

## HUD pause-accounting limitation

Source review found a separate reporting issue in the GLES frontend. The HUD
updates every 60 presented refreshes using `(frame - lastHUDFrame) /
(now - lastHUDTime)` in `emulator/src/ios/main.mm`. Modal menu pauses and manual
state operations stop or interrupt emulation, but their resume path resets the
emulation frame timer and pacer without resetting or compensating these HUD
anchors. The foreground path does compensate `lastHUDTime` for background time.
Consequently, the first HUD window after a modal pause/load can include time
when the guest was not running and temporarily understate running speed.

This source-confirmed possibility does **not** establish that it caused the
original 18% screenshot or the reported slowdown. That live session lacked a
matching continuous timing capture; it remains unresolved. Steady replay
measurements after restart cannot diagnose the original host-side state.

Proposed fix, not implemented here: invalidate the HUD sample across modal
pause/resume and manual state transitions, then establish fresh frame/time
anchors on the main thread from an accepted frame in the current session. Show
an unmeasured/collecting state until a new running interval is available. Verify
with a simulated long menu pause and state-load interruption followed by a
known running cadence, while preserving continuous guest timing measurements
and their real compilation, readback and other running-work stalls.

## Slot 2 CPU phases and reuse candidates

A separate instrumented run attributes approximately 10.09 ms/refresh to guest
CPU execution (including memory), 10.17 ms to GPU command processing, 0.27 ms to
HLE SVC and 0.02 ms to DSP HLE ticks. This adds instrumentation overhead and is
not an uninstrumented throughput comparison. Nested rendering scopes include
about 1.88 ms texture acquisition, 0.83 ms vertex preparation and 0.70 ms reuse
byte checking in the final window. Sampled draw submission includes substantial
host CPU execution, not exclusively GPU waiting.

Matched 1,200-refresh runs, first 300 excluded, same original Slot 2, 100% guest
CPU, sound and pacing, detailed phase diagnostics disabled:

| Candidate | Mean ms/refresh |
| --- | ---: |
| Baseline A | 20.726846 |
| Exact texture prefilter | 20.804931 |
| GL layout-state reuse | 20.352743 |
| Baseline B | 20.797735 |

All final
RGBA captures have SHA-256
`df88fa5a326ceffab7ce874efc665732c1f4f196f75839f17b72e1227e293fa2`.
The layout candidate is a small single-comparison result, not a validated default
or a four-millisecond solution. Prefiltering showed no gain. Neither default was
changed. Evidence: `profiles/stories-user-slot2-trace-20261113/reuse-ab`.

A separate source change replaces bounded per-draw vertex-range heap vectors
with a 16-entry stack array, retaining merge, copy, address and fallback semantics.
Native byte-coverage/capacity/edge tests and sanitizers pass; the iOS build passes.
It is not installed in the measured build and has no device speed result yet.

After the user's iPad reboot, devicectl reconnected over USB and reported usable,
compatible developer services, but Instruments continued listing the device
offline. Name, PID and CoreDevice-ID attach attempts produced no usable stack
capture. Built-in profiling was used instead; do not cite these attempts as CPU
stack evidence.

## Guest function recovery and unchanged-timing baseline

A fresh original Slot 2 replay at 100% guest CPU with sound measured 20.543 ms
per refresh (about 81% of the native refresh rate). Its final RGBA hash matches
the earlier Slot 2 comparisons (`df88fa5a326ceffab7ce874efc665732c1f4f196f75839f17b72e1227e293fa2`).
This repeats the roughly 20.7 ms result, not the transient 18% HUD observation.

Private static inspection verified all 128 captured census blocks against the
matching decoded image. Recovered behavioral ranges include the forward-copy
helper `0x29ae38..0x29aeac`, sphere-versus-six-planes classification
`0x83d524..0x83d5d8`, and signed 64-bit integer to double conversion
`0xa8ee34..0xa8eee8`. These are behavioral identifications, not recovered symbols.

The pacing routine `0x858230..0x858874` offers a coarse SleepThread path, then
a fine tick-poll loop at `0x8586f0..0x8587c4`. The loop calls GetSystemTick
through SVC 0x28 and invokes the conversion helper twice per iteration. Five
retained loop blocks each report 1,907,862 executions; the corresponding two
helper calls explain much of that helper's retained execution count. This
establishes tick polling, not HID polling, as a source of hot numeric work.
Resident counters are neither complete dynamic call counts nor CPU time.

No timing or loop-skipping change follows from this observation. The next probe
should attribute tick calls by thread/caller and measure CPU cost while
preserving Cara's existing tick charge, scheduling and observable effects. See
[offline mapping and measurement limitations](guest-hot-range-mapping.md).
Guest images and disassembly remain private and outside this repository.
