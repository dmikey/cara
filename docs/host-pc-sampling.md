# Internal host PC sampler

November 13, 2026. **Implemented internal prototype, default OFF, with one
successful iPad capture.** This is a fallback diagnostic for failed Instruments
attachment. It records wall-time PC residency; it is not an engine optimization
or a measurement of on-CPU bottleneck shares.

`CARA_INTERNAL_HOST_PC_SAMPLER=ON` includes the module and guarded frontend hooks;
normal builds exclude them. Within an internal benchmark build,
`CARA_HOST_PC_SAMPLER=1` enables capture. Defaults are frames 300–1200, nominal
100 Hz with 8–12 ms jittered sleep intervals, at most 6,000 records and a 60-second
capture limit. Actual sample frequency depends on scheduling. The JSON reports
compiled availability, configuration, stop reason and capture failures.

## Scope and safety ordering

The implementation uses one dedicated sampler thread targeting only the
application's registered emulation-thread Mach port. It records PC/LR/SP/FP
without walking or dereferencing the target stack. A process-wide ownership flag
serializes sampler sessions; this does not coordinate unrelated crash/hang tools.
The default bounds are safeguards, not a validated overhead budget.

Before sampling, the module allocates and touches a fixed-capacity record buffer,
retains the current emulation thread's Mach port, and captures image paths, UUIDs,
load addresses and slides. It resolves Mach callable addresses outside capture to
avoid first-call dyld binding during suspension, initializes its clock, and
preflights `thread_get_state` on the sampler thread itself to warm the Mach RPC /
reply-port TLS path. Preflight failure aborts capture without suspending the target.

For each sample it queries natural thread state before suspension, suspends the
target, fetches `ARM_THREAD_STATE64`, and issues its own balancing resume for every
successful suspension, including state-fetch failures. Register extraction,
record publication and failure accounting occur after that resume attempt.
Records retain each Mach result, pre-suspend run state, monotonic nanosecond
timestamps and measured suspension duration; capture metadata includes a Unix
wall-clock anchor. A failed resume terminates sampling and remains an explicit
failure, rather than claiming successful recovery. No error injection has proved
all rare OS-owned Mach/MIG failure paths safe.

The frontend joins before core teardown, including scope-exit cleanup; port
rights are then released. JSON serialization and offline symbolication happen
outside suspension. Avoid simultaneous unrelated crash/hang captures.

While the target is suspended, do not allocate, log, call Objective-C, resolve
symbols, inspect dyld images, or acquire application/runtime locks: the stopped
thread may hold those locks. Do not call `thread_abort`, change thread state,
install exception handlers or send sampling signals. This avoids deliberately
introducing a signal-handler/EINTR path, but does not guarantee zero scheduling,
audio, timing or system-call perturbation. Instrumented captures must remain
separate from performance comparisons.

## Public API evidence and SDK caveat

The installed iPhoneOS 27.2 SDK publicly declares `thread_get_state`,
`thread_suspend` and `thread_resume` in `usr/include/mach/thread_act.h`, and
`arm_thread_state64_get_pc/get_lr/get_sp/get_fp` in
`usr/include/mach/arm/thread_status.h`. Use these accessors for pointer-authentication
handling rather than hardcoded address masks. SDK root:
`/Volumes/Fast/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.2.sdk`.

That SDK marks the old suspend/resume calls for future deprecation and declares
`thread_suspend2` / `thread_resume2`, available from iOS 26.4. Their suspend-token
consumption and release contract must be verified before use; do not guess at
extra port deallocation or automatic-resume semantics. The legacy calls remain
public in this SDK. Self-process thread-port access avoids external debugger
attachment and `task_for_pid`. The first signed iPad build succeeded with these
legacy APIs; that result does not establish permission or behavior for every
OS/device/signing configuration. Public API declarations and upstream use are
not an App Review approval guarantee.

Primary sources:

- [Apple thread_get_state documentation](https://developer.apple.com/documentation/kernel/1418576-thread_get_state)
  and [Apple XNU interface semantics](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/thread_act.defs).
- [KSCrash KSBacktrace.c](https://github.com/kstenerud/KSCrash/blob/master/Sources/KSCrashRecordingCore/KSBacktrace.c):
  serialized remote suspend/state capture/resume, distinct from symbolication.
- [PLCrashReporter thread-state capture](https://github.com/microsoft/plcrashreporter/blob/master/Source/PLCrashAsyncThread.c):
  explains why a running thread's fetched state may be inconsistent.
- [Mozilla macOS sampler](https://github.com/mozilla-firefox/firefox/blob/main/tools/profiler/core/platform-macos.cpp):
  suspension critical-section and CPU-time accounting precedent; not iOS proof.
- [Apple App Review guidelines](https://developer.apple.com/app-store/review/guidelines/), section 2.5.1.

## First iPad validation

Evidence is under
`/Volumes/Fast/CaraBuild/profiles/stories-lunch-grass-20261113/host-profile/`:
`host_profile-host-pc-samples.json`, `analyzed.json`, and the matching frame timing,
benchmark and runtime reports. The exact app/dSYM UUID is
`E3CED360-2660-3274-9729-C09B93BF742C`; source/build fingerprint is
`52ffaea00ec2ef9c4907020b3178d50360f7fd6dde9777c02280474eb224e242`.
The recorded runtime used fastinterp at 100% guest CPU with adaptive guest CPU
disabled. The sampler does not change guest clocks or hardware timing policy.

The capture began at frame 300 and stopped at frame 1200: 1,353 valid records in
16.931562042 seconds, or 79.91 actual samples/second. All preflight, per-record
Mach results, resume balance, capacity, drop-count and timestamp checks passed;
all four failure counters and dropped records were zero. Natural pre-suspend
states were 1,215 RUNNING, 116 WAITING and 22 UNINTERRUPTIBLE. RUNNING means
runnable, not proof of executing on a CPU.

Measured suspension intervals summed to 4,534,441 ns (4.534 ms), with a maximum
of 14,667 ns (14.667 microseconds). That sum is about 0.0268% of capture wall time,
**not a total profiler overhead measurement**: it excludes sampling-thread work,
suspend-entry latency, scheduling disruption, cache effects and possible audio
or timing perturbation. No matched sampler-on/off experiment establishes those
costs yet. A separate native ARM64 own-process smoke test exercised a spinning /
waiting registered worker, capacity and time limits, normal and early-destructor
teardown, and continued worker progress after stopping. Successful capture does
not prove every error path or demonstrate a performance improvement.

Exact app executable ranges classified 820 PCs inside the app and 533 outside.
Unrecognized shared-cache PCs remained unresolved; they were not assigned to the
nearest image header, since shared-cache executable mappings can be relocated.
App LR values supplied parent-call clues for some non-app PCs; LR alone is not
an inclusive stack or proof of the sampled callee's identity. The first dSYM
matched the app UUID but lacked fastinterp/rasterizer source-line tables, so
function/offset attribution did not reliably identify computed-goto handlers.

## Interpretation and release boundary

Periodic suspension yields **wall-time PC residency**, including blocked or
descheduled states; it is not equivalent to Time Profiler's on-CPU sampling.
Natural `thread_info` state and cumulative user/system CPU deltas can provide
context, but runnable does not mean executing on a CPU, and assigning an entire
CPU delta to its final sampled PC does not establish precise CPU attribution.
After suspension, reported stopped state may be caused by the sampler itself.
Keep waiting/failed/unknown records visible and do not relabel sample percentages
as CPU shares. Jitter helps reduce periodic aliasing but does not establish
independent samples or confidence intervals.

Retain the exact optimized binary/dSYM UUIDs, source fingerprint and runtime
image load addresses/slides for offline `atos` or DWARF/disassembly mapping.
Function-only names cannot distinguish computed-goto handlers inside
`ExecuteBlock`; line/address mapping may, subject to inlining and shared machine
code. Missing driver symbols must remain image+offset/unknown. LR alone is not
an inclusive stack, and reading a live stack after resume is not a consistent
snapshot.

The entire module is a default-off compile option for explicit internal
optimized diagnostic builds. Its source, sampler thread, registrations and
hot-path hooks are excluded from normal shipping builds; a runtime environment
switch alone would not establish zero release overhead. Normal optimization
flags remain enabled with matching debug information. The initial capture can
guide further investigation, while causal performance claims still require
matched workload and perturbation checks. Sampler residency percentages must
not be presented as CPU shares or as a performance winner.

## Retaining optimized ThinLTO line tables

The next-link fix is implemented in the internal iOS target's CMake configuration:
`-object_path_lto` retains ThinLTO backend objects in the build's `src/cara-host-pc-lto-objects` directory.
Apple's linker creates a directory for ThinLTO even though its manual calls the
argument a filename. Keep the existing optimization/LTO flags and
`-gline-tables-only`; preserving these generated objects lets `dsymutil` recover
optimized source lines without recompiling the program with LTO disabled.
Run `dsymutil` after the link and before deleting or replacing the retained
objects. Check the dSYM's `--debug-line` output for the relevant translation
units, rather than assuming a matching UUID guarantees complete line tables.
Archive each capture's exact binary and generated dSYM with their matching UUID;
a new relink produces a new artifact and its dSYM must not be used for an older
capture. The retained-object linker option is absent when the internal sampler
build option is off.

This retention fix passed a private native ThinLTO link/dSYM/source-line smoke
test with the installed Xcode toolchain. It has not yet been relinked and
validated on the iPad; it does not restore missing line tables in the first
capture's existing dSYM. Verify the next exact binary/dSYM before using its lines.

## Offline app-only analysis

Archive the capture with its exact app binary and dSYM, then run on a Mac with
Xcode command-line tools available:

```sh
python3 tools/symbolize_host_pc.py capture.json \
  --binary archive/Cara.app/cara-ios \
  --dsym archive/Cara.dSYM \
  --output analysis.json
```

Optionally pass `--expected-uuid UUID` to pin a known capture artifact. The tool
requires exact UUID and architecture agreement, checks the app's Mach-O segment
bounds and runtime slide, and sends only app executable-range PCs to batched
`atos`. It retains non-app/unknown addresses without assigning shared-cache
symbols from the host OS. The output separates all valid wall PCs from natural
RUNNING records and reports Mach failures, resume balance, clock validity,
capacity/drop checks and measured suspension duration. A matching dSYM may still
lack source lines; function/offset output must not be mistaken for handler-level
source attribution. Capture-quality warnings remain visible in the JSON.

Focused tests mock Apple tools and need no device or Xcode:

```sh
python3 -m unittest discover -s tools -p test_symbolize_host_pc.py
```

The next iPad capture verified optimized line tables after the ThinLTO retention
fix: UUID `C0B3D341-AE30-391D-BECC-879067902F8D`, 1,297 valid samples over
16.270 seconds, no Mach failures and no quality warnings. Explicit disassembly
resolved ambiguous source line zero and function-entry line records: 70 were
dispatch-table loads before indirect branches and only two were actual prologue
instructions. Function/source-line labels alone therefore cannot justify
assigning samples to entry overhead. Block lookup, operand/page access and
Morton conversion remain concrete targets; these are wall-residency clues.

## Native M1 Time Profiler verification (2026-11-13)

A separate 20-second Time Profiler attachment completed on native M1 macOS,
using the Stories Secret Path checkpoint after the uninstrumented comparisons
finished. Guest clock and speed cap were 100%, sound was enabled, and the VLDR
pair, Mac GPU depth reinterpretation, and GL state-reuse experiments were off.
This was desktop OpenGL, not an iPad or Metal-backend measurement.

The capture tool archived the executable with SHA-256
`805638fc154044d4fb5539049734175dabc7de40aea2acaee5edbc0238f54d04`, verified its
`arm64` UUID `6E7CB762-9C85-3623-8D90-DB5330F3E9F6` against the target process's
exported image, and confirmed unchanged PID executable/start identity before
and after recording. Its caller-declared engine fingerprint was
`1414da400d4eba11aabfe7dad3dcfc3dfd9545b26faa1d16c5005366fc3434ae`.
The raw trace, XML, executable and receipt are retained privately; no game data
is required by the profiler or its parser tests.

The export contains 17,507 sampled stacks, including 14,755 on the main thread.
Each row has a 1,000,000 ns exported weight. These weights are not measured
instruction durations or a speed comparison. All main-thread rows are labeled
`Running`, including 3,323 `mach_msg2_trap` samples (22.52% of main-thread weight).
That label does not establish how much of the syscall attribution is blocking;
these records remain included rather than being discarded to inflate CPU shares.
`ExecuteBlock` has 4,355 self samples (29.52% of main-thread weight).

Individual leaf locations now remain distinct even when their function names
are identical. The leading interpreter locations were checked against this
exact executable's disassembly and 598-entry dispatch table:

| Containing host instruction | Samples | Main-thread weight |
| --- | ---: | ---: |
| `LdrImm_al`: load decoded base-register operand | 272 | 1.843% |
| Shared dispatch-table pointer load before indirect branch | 238 | 1.613% |
| `AddImm_al`: load decoded source-register operand | 145 | 0.983% |
| `MovLslImm_al`: load decoded source-register operand | 125 | 0.847% |
| `MovImm_al`: load decoded immediate | 96 | 0.651% |
| `VldrS_al`: load decoded base-register operand | 86 | 0.583% |

The exported addresses lie one byte into their containing four-byte ARM64
instructions; raw exported values are preserved. Dispatch-table entries use
Mach-O `DYLD_CHAINED_PTR_64_OFFSET` fixups and were decoded accordingly. No usable
optimized DWARF line information was found in this archived executable, so these
labels come from symbols, exact dispatch targets and assembly, not source-line
attribution. Samples at handler entry do not by themselves prove operand-load
latency or cache misses; indirect-branch behavior and sampling skid remain
possible explanations. Hardware counters would be a separate measurement.

Named lookup-table work was small in this capture: `SyncAndUploadLUTsLF` had
11 inclusive and seven self samples (0.075% and 0.047% of main-thread weight),
while `SyncAndUploadLUTs` had two inclusive/self samples (0.014%).
`UploadIOSLUT` does not appear in this native desktop capture. Inlining limits
negative conclusions, and these results do not establish iPad or Android LUT
costs. `WritePicaReg` had 9,321 inclusive samples but only 151 self samples;
nested rendering must not be counted again as independent command-processing
CPU work.
