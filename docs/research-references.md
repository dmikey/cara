# Research references

Cara builds on Citra, Citra MMJ and Azahar FastInterp. Their code provenance and
licenses are recorded in [NOTICE](../NOTICE) and
[source provenance](../emulator/VENDORED_SOURCE.json). The following additional
projects and publications inform development research. A research reference is
not a claim that its implementation was imported or its performance reproduced.

| Reference and contributors | Contribution to the investigation | Source review |
| --- | --- | --- |
| Dolphin Emulator Project | Cached interpretation, proven idle recognition, GPU staging and synchronization. | [Performance prior art](performance-prior-art.md) |
| PPSSPP / Henrik Rydgård and contributors | Interpreter dispatch and optimization passes, deferred draws, upload buffering and batching boundaries. | [Performance prior art](performance-prior-art.md) |
| Panda3DS contributors | Native vertex-attribute transport, index rebasing and accelerated renderer comparisons. | [Performance prior art](performance-prior-art.md) |
| Mikage contributors | Explicit resource ownership, guest memory hooks and fence handling, including documented incomplete paths. | [GPU residency review](gpu-residency-prior-art.md) |
| V8 authors / DrumBrake | Static interpreter handlers, superinstructions and register modes. | [CPU interpreter prior art](performance-prior-art.md#cpu-interpreter-prior-art-and-bounded-candidates) |
| fearkov / 3dsrecomp and Zakuro | Guest-image inspection, verification approaches, vertex input planning and GPU command research. | [Pinned source comparison](zakuro-recomp-prior-art.md) |
| Nguyen Anh Quynh, COSEINC and Capstone contributors | Offline ARM/Thumb disassembly for private guest-code inspection. | [Tool versions and credits](inspection-tool-attributions.md) |
| DMP / PICA200 publications, including Kazakov and Ohbuchi | Vertex/primitive pipelines, caches, lighting circuitry and memory interfaces. | [Hardware source audit](hardware-source-audit.md) |
| 3dbrew community, GBATEK / Martin Korth, JayFoxRox and 3ds-tests contributors | Reverse-engineered registers, interrupt behavior and hardware correctness-test approaches. | [Hardware source audit](hardware-source-audit.md) |
| Apple documentation | GLES context ownership, buffered resources and synchronization guidance. | [Performance prior art](performance-prior-art.md) |
| iFixit and Nintendo-submitted FCC photographs | Physical board evidence, with explicit limits on what photographs establish. | [Hardware source audit](hardware-source-audit.md) |

The linked reviews carry primary-source links, pinned revisions where recorded,
measurement conditions and limitations. Public hardware material has not supplied
a calibrated retail-console GPU completion model. Emulator source review, physical
board evidence, interpreter-reference tests and real-hardware measurements remain
distinct forms of evidence.

The [November 16 development update](updates/2026-11-16.md) summarizes the latest
M1 checked GPU-residency gain, save/replay checks, prototype worker results and
remaining Stories work. [Frame-production findings](gpu-frame-production.md#retained-m1-causal-traces-2026-11-15)
also connect the retained P3D/descriptor trace to the original Capcom field
research and Citra timing explanation without treating those sources as proof
of an early-completion bug.

Cara's current findings are collected in the
[Stories dependency trace](stories-gpu-trace-2026-11-13.md),
[tick-poll audit](stories-tick-poll-audit.md),
[GPU alias results](gpu-surface-aliases.md),
[host pacing results](frame-pacing.md), and
[used-input staging experiment](vertex-input-staging.md).

The [November 20 Metal/FastInterp source review](metal-upstream-research-2026-11-20.md)
adds pinned 3DS and Metal renderer comparisons, Apple GPU optimization guidance,
and interpreter papers. The accompanying [local parity investigation](metal-fastinterp-parity-2026-11-20.md)
records measured discriminators and rejected configurations separately from that
research. Upstream mechanisms are inspiration; only local matched-work runs can
establish a performance improvement in Cara.

### Deferred Metal raster-only fallback packets (2026-11-21)

`CARA_METAL_DEFER_RASTER_FALLBACKS=1` is a **default-off experiment**, with no
measured throughput gain yet. It retains an already-recognized unique/raw draw
as an immutable raster-only packet when the existing compute admission rejects
it. Such packets never enter compute groups. They replay the original
pipeline, indices, raster corner order and quaternion handling between the
computed draws in their original order. The existing 64-packet and 8,192-output
limits remain; maximum compute output storage is still 64 MiB.

The current implementation already collects different compute programs in one
encoder before replaying raster draws; changing vertex programs does not itself
flush. The narrower opportunity is avoiding an unnecessary compute/raster split
at a valid raster-only draw. In the retained v25 admission diagnostic, refreshes
120–600 contain 14 draw submissions and 26 raster encoder starts per refresh,
with four invalid-unique and four oversized-transform rejections per refresh,
in addition to four depth-clear boundaries. These counters motivate testing;
they do not establish saved CPU time or an eventual speed improvement.

The ordering audit is restricted to known raw programs and the existing draw
group. `PicaMetalDrawGroup::Append` rejects material reads overlapping the current
color/depth attachments and requires the same attachment backing. Overlapping
vertex-source guest reads and CPU invalidations publish pending work first.
`UploadDraw` snapshots uniforms, source data and indices into retained command
resources/arena leases before enqueueing a packet. Compute reads those immutable
inputs and writes separate output ranges; raster replay preserves framebuffer
ordering. Attachment/publication, depth-clear, SIMD, unretained-command, legacy uniform
reuse and unknown-program boundaries remain immediate. This does not authorize
cross-command reordering or stale FCRAM reads.

The production draw encoder and actual draw-group fixture passed **704 paired
GPU comparisons**: 44 cases in 16 fresh processes covering fallback packet,
transform-admission, deferred-uniform-reuse and transform-deduplication OFF/ON
combinations. Metal API and GPU validation were enabled. Checks include mixed
computed/fallback/computed order, changed uniforms/source/layout/indices and
quaternion golden channels, a material-attachment alias probe forcing the
original publication boundary, actual 65-packet group rollover, valid 8,192 and
8,193 unique-count boundaries, large list/strip/fan streams and the exact 64 MiB
output ceiling. The long-fan fixture initially had crossing vertex positions;
its baseline coverage error was corrected to perimeter order without weakening
the exact image comparison. Reproduce with
`tests/run_metal_unique_vertex_compute_test.py`; the local retained receipt is
`work/portable-metal-checks/unique-transform-admission/raster-fallback-results.json`.

### Apple scheduling and bundle-launch diagnostics (2026-11-21)

These are primary-source constraints and proposed diagnostics, **not explanations
for the normal/replay performance gap or accepted renderer improvements**.

- [Latency-critical activity](https://developer.apple.com/documentation/foundation/processinfo/activityoptions/latencycritical)
  requests timer/I/O precision; Apple cautions that few apps need it. The installed
  Xcode SDK defines `NSActivityUserInteractive` as user-initiated activity plus
  latency-critical, available at macOS 13/iOS 16. Neither this API nor a successful
  activity token guarantees a CPU clock or GPU priority. SDK comments warn against
  unnecessarily long activity lifetimes. Any experiment must remain scoped to
  active playback, independent of existing thread QoS, and default off.
- [Apple Silicon game scheduling guidance](https://developer.apple.com/videos/play/tech-talks/110147/)
  explains QoS, timer coalescing and OS-managed core/frequency choices; P-core
  availability is not guaranteed. Keep public QoS semantics rather than raw
  priority/core pinning. No documented GPU-priority setter or queue-creation/
  commit QoS-inheritance guarantee was found in the installed public
  `MTLCommandQueue`, its descriptor or `MTL4CommandQueue` headers. Creating our
  queue before applying thread QoS is an observed source ordering, not proof of
  permanently inherited lower GPU priority. Metal 4's documented
  [compilation QoS](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api)
  concerns compiler CPU work, not GPU command scheduling.
- [Game Mode requirements](https://support.apple.com/en-la/105118) include Apple
  Silicon macOS 14+ or iOS/iPadOS 18+ and supported fullscreen gameplay.
  [LSSupportsGameMode](https://developer.apple.com/documentation/bundleresources/information-property-list/lssupportsgamemode)
  is explicitly documented at macOS 26/iOS 18.6. The
  [macOS 26 release notes](https://developer.apple.com/documentation/macos-release-notes/macos-26-release-notes)
  document that directly spawned Terminal binaries do not activate Game Mode;
  the public workaround is launching the bundle through `open`. This is separate
  from backend portability at the macOS 13/iOS 16 floors.
- [Sustained Execution](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.sustained-execution)
  limits initial burst performance for consistency on applicable devices. It
  requires an entitlement/provisioning and newer OS support; it is not a boost.

An optional future `--launch-mode bundle` must retain the direct default and use
an exact built bundle path with `open -n -W --stdout PATH --stderr PATH`, explicit
`--env` overrides and `--args ROM FRAMES`. Local `open(1)` documents environment
inheritance. `open`'s PID/status cannot substitute for the app's PID/real exit.
Require a nonce-bound early PID/executable/output receipt, safe process-exit
monitoring registered before an acknowledgment lets the runner continue,
bounded timeout/verified-PID cleanup, isolated output files and all existing
RESULT/title/backend/guest100%/pixel/boundary checks. Installed SDK `NOTE_EXITSTATUS`
permits child or signal-authorized processes, while older `kqueue(2)` wording
says child-only: validate that mechanism before relying on nonchild exit status,
and fail closed if unavailable. Record launch mode and exit evidence separately;
fullscreen/Game Mode activation is a separate manual diagnostic. No bundle-launch
implementation or activation was performed for this review.

For the remaining GPU/off-CPU ambiguity, the installed Xcode tools list **Metal
System Trace**, **Game Performance** and **Power Profiler**. A future short,
all-processes Metal System Trace should correlate the verified runner PID's
submission/encoder events and CPU thread states with GPU work from other
processes, thermal state and any available GPU performance-state/counter tracks.
[Apple's analysis guide](https://developer.apple.com/documentation/xcode/analyzing-the-performance-of-your-metal-app)
combines system/thread information with Metal tracing;
[Apple's tools session](https://developer.apple.com/videos/play/wwdc2021/10157/)
explains OS-managed GPU performance state. First observe the natural state;
induced fixed-state recordings must be separately labeled. Our non-recording
`xctrace ... --show-recording-options` query exposed only Hangs/Time Profiler
options, so counter/state controls must not be invented as CLI flags.
[GPU start time](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime)
marks actual execution start: start-to-end excludes initial scheduling wait but
is not exclusive shader occupancy. Distinguish submit-to-start delay, execution
span, interleaving/stalls and CPU blocking. Trace overhead makes these diagnostic
runs unsuitable as uninstrumented throughput acceptance results.

#### Instruments 27.2 performance-state export caveat

The retained two-second capability probe's TOC says
`requested-consistent-state=0`, while exported `gpu-performance-state-intervals`
rows say `is-induced=1` and "Minimum GPU Performance state due to active device
conditions". **Those row labels do not establish an active external condition.**
The installed Instruments model at
`/Applications/Xcode.app/Contents/Applications/Instruments.app/Contents/Packages/GPU.instrdst/Contents/Extensions/com.apple.gpu-performance-state.dtac/modeling-rules/rules-0002.clp`
unconditionally sets `is-induced=1` for ordinary device-state rows at lines
104–127, without requiring a ConditionInducer fact. Its fallback narrative also
mentions active conditions whenever desired state is zero/unknown (or is not a
different positive state). The immediate-mode counterpart repeats this behavior
at lines 161–181. This is an implementation-specific export quirk, not a public
API contract or evidence that the machine is deliberately clock-limited.

The same model maps raw state values 0/1/2/3 to Unknown/Minimum/Medium/Maximum
at lines 7–11. Actual condition facts are separately derived from
`ConditionsStateSignpost` messages beginning `Active Conditions:` and
`induce-setup`/`induce`/`induce-teardown` events at lines 23–91. The probe TOC
contains a narrow `os-signpost` table with category `InduceCondition`, subsystem
`com.apple.ConditionInducer.LowSeverity`; that table and read-only Instruments
condition UI are appropriate evidence sources before attributing a forced state.
Neither TOC request zero nor the derived row label alone settles that question.

Probe raw device-state rows all contain state 1 and desired state 0, but their
48 intervals total only **69.726 ms within the 2.891-second trace**. Do not
extend Minimum across unrecorded gaps or infer a constant clock from this sparse
idle/compositor probe. Preserve the raw XML exports alongside any summaries.

For future parsing, export each schema separately: the local multi-table export
omitted schemas after the first table. Resolve XML id/ref cells, use raw
nanoseconds and schema mnemonics, join command IDs with process identity, and
report overlapping interval unions separately from duration sums. Installed
`com.apple.metalsystrace-app.dtac/modeling-rules/rules-0004.clp` lines 259–261
define submission duration as commit time minus creation time, so exported
`start + duration` is the commit boundary; creation time alone must not be used
to claim GPU queue wait. No device conditions were changed during this audit.
