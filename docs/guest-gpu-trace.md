# Bounded guest GPU dependency trace

Diagnostic only; default disabled. Neither completion experiment is enabled by
this probe. It changes no commands, waits, interrupts, scheduler decisions or guest time.

Set `CARA_GPU_TRACE=1`. The iOS frontend arms the trace after checkpoint restore.
`CARA_GPU_TRACE_START_FRAME` selects a zero-based post-arm guest VBlank boundary
(default 0); the next requested intervals are captured (default 16). Output defaults to
`Documents/cara/guest-gpu-trace.csv`; `CARA_GPU_TRACE_PATH` overrides it.
An explicit later state load re-arms the trace from that restored state.

The fixed buffer holds 32,768 records. Exhaustion stops capture and exports
`truncated=1`. No allocation, clocks, guest-memory reads or file writes occur in
disabled hooks. Enabled hooks buffer observations; export happens after RunLoop
or at shutdown. Export is attempted once; failure is logged once by the frontend.
An atomic enabled gate and thread-local owner marker restrict capture to the
emulation thread that armed it. Foreign host-thread hooks only increment an
atomic dropped-observation counter and never read CoreTiming or the trace buffer.
`foreign_events_dropped` in the export makes that limitation explicit.
`complete=0` means shutdown interrupted the requested window.
The trace observes all kernel events/waits during the bounded window so that a
GSP handler forwarding work to another thread remains visible. This can exhaust
the buffer: use a shorter checkpoint-to-window offset rather than interpreting a
truncated chain as a completed capture.

Every record retains observation order, timer-0 ticks, current timer ticks,
global ticks and current timer ID. Do not sort by guest ticks: different timer
domains can differ. The `core` column always identifies the current CoreTiming owner. Scheduler Run
records separately identify their thread-manager core in payload `b`. No host timestamp or
GPU completion fence is implied by these fields.

CSV fields `a,b,c,d` depend on `kind`:

| Kind | thread / object | a | b | c | d |
| --- | --- | --- | --- | --- | --- |
| VBlank | zero | zero | zero | zero | zero |
| GXBegin | caller kernel thread / GSP recipient | command ID | raw ring header | chosen slot in high32, active recipient low32 | command payload words 0/1 packed high/low32 (list address/size for submit) |
| GXEnd | caller kernel thread / GSP recipient | draw delta | GPU vertex delta | counters enabled | raw ring header |
| IRQRequest | GSP recipient / kernel event ID | IRQ ID | relay head before | relay count before | relay config byte |
| Relay | GSP recipient / kernel event ID | IRQ ID | relay outcome enum | head high32, count low32 after | error high32, event-signal flag low32 |
| Binding | GSP recipient / kernel event ID | zero | zero | zero | zero |
| PicaIRQ | zero | raw register write value | byte mask | zero | zero |
| EventSignal, EventClear | zero / kernel object ID | zero | zero | zero | zero |
| EventAcquire | acquiring thread / kernel object ID | zero | zero | zero | zero |
| WaitObject | waiting thread / kernel object ID | object index | object count | signed timeout ns reinterpreted as u64 | wait-all (N only) |
| WaitReady, WaitBlock | waiting thread / selected object or zero for N/all | wait-all where supplied | zero | zero | zero |
| WaitPoll | waiting thread / object or zero for N | zero | zero | zero | zero |
| Wake | resumed thread / signal object or zero | 0 signal, 1 timeout | zero | zero | zero |
| Run | new running thread or zero for idle / zero | preceding thread | scheduler core | zero | zero |
| Swap | zero | screen ID | left address | right address | active framebuffer |

`submission` increments for each GX command; an IRQ emitted during that command
inherits its ID. Outside command execution it is the most recent GX command,
not a claimed causal dependency. Binding records are emitted on captured IRQ
delivery from live session objects, so checkpoint restoration needs no boot-only
registration replay. Relay outcomes follow `RelayQueuePolicy::Outcome`.

Draw/vertex deltas require `CARA_FRAME_TIMING`. GPU vertex counts exclude vertices
handled by the CPU path. The base dependency mode adds no per-draw hooks or command-stream hashes;
optional draw-pass capture is described below.
P3D/PPF events are completion notifications, not necessarily guest frame boundaries.
Pair the trace with existing refresh counts, pixels and fixed-input progress;
repeated submissions alone do not prove redundant work. This first probe does
not capture experimental deferred-completion tokens/deadlines or GPU register
polling and must not be used to validate those contracts.

## Optional guest draw-pass records

Set `CARA_GPU_TRACE_DRAWS=1` as well as `CARA_GPU_TRACE=1` to capture each guest
indexed/nonindexed draw trigger. `CARA_GPU_TRACE_DURATION_VBLANKS` accepts 1–16
(default 16). Use **4** for the Stories heavy scene: each trigger adds three
adjacent records, so a 16-interval heavy capture would exceed the fixed capacity.
The whole triplet is reserved or capture stops with explicit truncation. Draw
records do not include immediate-mode vertices. Neither shader nor vertex/texture
buffers are hashed or copied by the draw hook.

| Kind | a | b | c | d |
| --- | --- | --- | --- | --- |
| Draw (17) | color physical address high32, depth low32 | vertex count low32; upper32 flags: indexed bit0, topology bits1–2, useGS bits3–4, variable primitive bit5 | VS generation | GS generation |
| DrawState (18) | width bits0–15, height16–31, color format32–35, depth format36–39, color-write mask40–43, depth/stencil-write mask44–45, cull mode48–49 | depth-test/color-mask raw register low32, stencil-test raw function high32 | VS main offset low32, GS main offset high32 | fragment operation/blend-mode raw register low32, alpha-blend raw register high32 |
| DrawInput (19) | vertex physical base high32, raw index offset/format register low32 | vertex offset low32, texture main configuration register high32 | texture0 physical address high32, texture1 low32 | texture2 physical address low32; upper32: formats0/1/2 in bits0–3/4–7/8–11 and texture0 type12–14 |

These kinds reserve `thread` and `object` as zero. Adjacent records share the
same frame and GX submission. Texture registers are recorded even for disabled
units: use the texture main configuration to interpret them. An index register
is recorded for nonindexed draws too, but its contents are inactive in that mode.
VS/GS generations start at zero when armed and increment only for observed actual
program/swizzle-word changes during capture. Initial zero means the restored
shader content has not changed since capture began; it does **not** identify that
content. Generations are meaningful within one trace and are not cross-run shader
hashes. Entry offsets are recorded separately.

Matching addresses and captured state form an inspection group, not proof of
redundant work. Uniforms, viewport/scissor, attribute-loader offsets/layout,
vertex/index/texture contents and content changes are not captured. Distinct
passes may deliberately reuse all these recorded fields.

## HID and sleep observations

`HIDUpdate` (20) records one existing pad service update before its event signals.
`object` is the PadOrTouch1 kernel event ID, `a` is PadOrTouch2 event ID, `b` packs
actual pad-ring index high32 and pad state low32, `c` is signed callback lateness
reinterpreted as u64, and `d` is the original signed next scheduling delay
reinterpreted as u64. A periodic update is not necessarily a button change: compare
successive low32 pad states. Event signals can make waiters ready; a subsequent
Run record separately establishes when a thread actually resumes.

`SleepRequest` (21) records the calling kernel thread in `thread` and the signed
requested nanoseconds reinterpreted as u64 in `a`, before the existing sleep
handler. A zero-duration request may simply return or yield without sleeping.
The request alone does not establish suspension or a scheduled timeout. Match it
with observed timeout Wake/Run records where possible.

Analyze either older dependency-only traces or the additive draw schema with:

```sh
python3 tools/analyze_guest_gpu_trace.py /path/to/guest-gpu-trace.csv --output analysis.json
```

The analyzer rejects incomplete, truncated, foreign-observation-dropped or
malformed captures by default. `--allow-partial` permits observed aggregates while
retaining `usable_as_complete_capture=false`. It preserves timer domains and
validates adjacent draw triplets. `draw_passes` reports framebuffer partitions,
trigger vertices and equality groups of the captured fields, with the missing
content/state evidence explicitly retained as a limitation.
