# ARM emulation without host JIT

Cara runs its existing ARM interpreter as signed, ahead-of-time-compiled C++ code.
The 3DS ARM/Thumb instruction bytes cannot execute as AArch64 instructions in an
iOS app. Guest instructions are decoded into data, not executable memory.

This candidate retains exact guest instruction budgets, predication, flags,
address-space switches, and GPU memory-coherence fallbacks. It adds:

- Direct unsigned CMP flag calculation instead of a general addition helper call.
- Rotated CMP immediate operands decoded once into existing ARM64 struct padding.
- Inline normal 32-bit RAM reads, avoiding a nested call and an opaque-state lookup.
  Null page pointers use the existing unmapped/GPU-cached-memory handling.
- Regressions for register CMP flags, every predicate and initial flag combination,
  all 4096 rotated-immediate encodings, remapping, and little-endian RAM reads.

Performance is validated on the device rather than inferred from instruction counts.
The private benchmark replays 9000 display refreshes with a fixed guest clock,
scripted A presses at frames 900 and 1500, and 300-refresh timing windows. Compare
windows after 1500 only if guest/core tick counts and GPU work match exactly.
No game data, audio recordings, or ROM instructions are distributed with these tests.

## Audio

Native iOS output is stereo PCM16 at 32728 Hz, converted by Audio Queue to the
hardware output rate. Full-speed direct playback avoids time-stretch artifacts.
A 2048-frame source reserve absorbs pacing jitter; underruns fade out and recovery
fades in. This adds about 63 ms of source buffering plus the output queue latency.
The saved menu preference can enable stretching for slow scenes instead.

Opt-in CARA_AUDIO_DIAGNOSTICS reports supplied/requested samples, underrun counts,
and callback cost. Intentional startup/recovery buffering is included in shortfall
counts. The audio callback writes no files. CARA_AUDIO_DISABLED selects the silent
sink for matched comparisons. CARA_AUDIO_NO_STRETCH forces direct playback for tests.
