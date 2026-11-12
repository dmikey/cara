# Adaptive guest CPU

**Menu → Enable adaptive guest CPU (50–100%)** enables a remembered, optional host policy. It is off by default. The HUD displays the current percentage while enabled. Disabling it restores 100% immediately.

The controller measures CPU and renderer work between emulated display refreshes, excluding the frontend's pacing sleep. It waits through startup, requires sustained overload, then lowers the guest CPU budget in small steps, down to 50%. Sustained spare time gradually raises it to 100%. Pauses and save-state operations reset the measurement window. Loading a state keeps the current adaptive choice and reapplies its current clock.

This changes how much CPU work the game can execute per emulated second. It keeps GPU, audio and other scheduled hardware events at their normal clock. A game can respond by reducing its own internal frame rate, changing timing, or behaving differently. GPU-bound effects may still be slow. A 100% speed reading with a reduced guest CPU clock does not mean full original CPU performance; the HUD shows both values.

The policy is independent of UIKit and the engine. Clock changes are applied on the emulation thread, consistently across all four CPU timers. Fractional cycles are retained instead of discarded, and the 100% path keeps the original accounting. No executable memory or JIT is involved.

## Validation and diagnostics

`cara-clock-self-test` checks four-core budgets, fractional cycle accounting, peripheral event timing, transitions, disabled behavior, sustained overload, recovery, transient stalls and pacing. The state regression also runs New 3DS at 70% to check saved fractional timing and deterministic continuation.

Controlled benchmarks ignore the remembered adaptive preference. Set `CARA_ADAPTIVE_GUEST_CPU=1` explicitly to measure adaptive mode. Runtime and benchmark reports include `adaptive_guest_cpu` and `guest_cpu_percent`; frame-timing windows report their ending guest clock. Compare those values along with visible game behavior, rather than interpreting a higher display-refresh count alone as better gameplay.
