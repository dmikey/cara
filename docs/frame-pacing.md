# iOS frame pacing

Cara targets 100% emulated speed by default. Guest CPU clock and physical display
submission remain separate policies. The iOS pacer keeps at most one target
refresh of lateness (about 16.71 ms at 100%) so a light refresh can recover time
lost by the preceding heavy refresh. Long stalls cannot accumulate unlimited
catch-up debt. Pause, state-load and speed changes reset the pacing timeline.
Physical presentation remains limited by display-link tickets.

The previous policy discarded all lateness. Alternating 20 ms and zero-cost
refreshes therefore ran at 54.48 Hz despite averaging only 10 ms of work. The
bounded policy reaches the target 59.834 Hz in the deterministic regression.
Tests also cover sustained overload, long stalls, resets, speed changes and
extreme finite clock values. This changes host sleeping, not guest execution or
hardware event timing, and does not use JIT.

## M4 device validation, 2026-11-13

A Xenoblade opening-battle checkpoint was replayed for 1,200 refreshes with
identical scripted A/movement input, sound, native GLES and 100% guest CPU.
Measurements exclude the first 300 refreshes. Legacy/bounded/legacy pacing
averaged 18.698 / 16.713 / 18.583 ms per refresh. All final RGBA captures matched,
as did vertex/draw workloads. Audio output remained active without output errors;
DSP underrun callback counts were 199 / 18 / 181 over the complete runs. This
establishes the pacing improvement for this replay, not every game or scene.

Bounded pacing is the iOS default. `CARA_BOUNDED_FRAME_PACING=0` selects the legacy
policy for diagnosis; `1` selects bounded pacing. The comparison harness accepts
`bounded_pacing` and verifies the runtime status. Use `--paced` for these tests.
The standalone policy constructor retains its legacy default for other callers.
