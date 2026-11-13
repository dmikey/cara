# Internal host PC sampling proposal

Source/SDK review, November 13, 2026. **Not implemented or validated on a device.**
This is a fallback diagnostic for failed Instruments attachment, not an engine
optimization or evidence of current bottleneck shares.

## Scope and safety ordering

Use one dedicated sampler thread targeting only the application's registered
emulation-thread Mach port. An initial experiment should record PC/LR/SP/FP only,
without walking or dereferencing the target stack. A bounded, jittered schedule
around 100 Hz for 30–60 seconds is a starting proposal, not a validated overhead
budget.

Before sampling, allocate and touch a fixed-capacity ring, register/retain the
thread port, and capture image metadata outside the suspension window. For each
sample: query any natural thread state before suspension; suspend the target;
fetch `ARM_THREAD_STATE64`; always balance a successful suspension with its own
resume; then publish the record and process errors after resumption. Track failed
calls, dropped records, elapsed capture time and suspension duration. Stop/join
the sampler before the target exits and release the held port right. Serialize
remote captures and avoid conflicting crash/hang capture sessions.

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
attachment and `task_for_pid`; runtime permission/success still needs checking
on the actual signed build/device. Public API declarations and upstream use are
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

Make the entire module a default-off compile option for explicit internal
optimized diagnostic builds. Exclude its source, sampler thread, registrations
and hot-path hooks from normal shipping builds; a runtime environment switch
alone does not establish zero release overhead. Keep normal optimization flags
and add matching debug information rather than profiling an unoptimized build.
A first prototype must demonstrate balanced resume/error handling, bounded
memory and clean teardown, then measure perturbation and output/workload
agreement before its samples guide an optimization.
