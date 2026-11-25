# RG Slide: persistent uniform buffer reuse

On the RG Slide (Unisoc T820, Mali-G57), a signed private Android Atlas build ran a matched warm Gadd lab replay from a stock restored state: JIT, 1×, Performance Mode, guest clock fixed at 100%, adaptive frame skip disabled. We ran OFF/ON/ON/OFF with three 120-refresh windows per arm. Every arm admitted 165,994 draws over the scored 360 refreshes.

| Metric (mean across scored windows) | OFF | ON |
| --- | ---: | ---: |
| Graphics-command CPU time per refresh | 26.253 ms | 24.411 ms |
| Uniform CPU span per refresh | 1.834 ms | 0.701 ms |
| Refresh median | 35.343 ms | 33.658 ms |
| Refresh p99 | 40.063 ms | 38.200 ms |

Graphics-command CPU time fell 1.843 ms (7.02%), beyond OFF control drift in all three windows. Each ON arm had 165,994 persistent writes, 77 fenced page rotations and 77 reuses, with no page waits or sync fallbacks. Uniform time is part of graphics-command time and is not additive. Both screens were visually checked; selected screenshots were not exact guest-phase pixel comparisons. This is a narrow warm-scene result, not a full-speed claim, cold-cache result or cross-device proof. Texture lookup reuse showed no supported average improvement and remains disabled.

The underlying persistent buffer ring is unchanged. The Android Atlas frontend now requests it by default only for Performance Mode on an identified RG Slide (`ums9620_2h10_native`), actual ARM/Mali-G57 GLES context with the buffer-storage and fence entry points, and no ordered-worker request. Unknown devices, unsupported drivers and Accuracy Mode stay off automatically. The renderer still checks allocation/mapping success and fences before use; it retains its original fallbacks.

An existing explicit `[Debug] atlas_persistent_uniforms = False` opts out; `True` retains explicit diagnostic opt-in subject to existing renderer checks. The host policy test checks scope and override handling. Physical startup must confirm that the shipping build's Android product properties and actual GLES context match the retained device identification; the original performance bracket used an explicit enable.
