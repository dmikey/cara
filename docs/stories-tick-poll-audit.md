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
