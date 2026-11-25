# G Cloud unused-quaternion experiment — 25 November 2026

Private opt-in experiment, not an enabled production policy. The exact target is Logitech GR0006, Qualcomm Adreno (TM) 618, OpenGL ES 3.2 V@0502.0 (GIT@46d1463393, I43876eb25d, 1638292465), GLSL ES 3.20, monolithic GLES. Every other target retains the existing defaults, including all validated RG Slide/Mali UBO, state, layout and packed-fetch policies.

## What changed

Only raw packed-input mode 2 may select the specialization. Cara first builds the exact current fragment configuration and rejects lighting consumers, guest geometry shaders, unsupported fragment forms and conflicting diagnostic shader policies. The complete generated fragment source must contain only the known declaration of `normquat` and no read of it. Its hash is checked against the actually selected fragment stage. A conservative cache compares full configuration bytes on a hash hit.

Before compilation, the candidate vertex source is independently compared with the baseline generated from the same live PICA setup. The accepted delta removes only the otherwise unused triangle-reference quaternion evaluation and its unused output assignment. The actual vertex program, every other output, source functions, guest input decoding and mutable-register resets remain byte identical. Unknown forms retain the baseline. Consuming fragments keep the original triangle-reference sign correction. The on/off choice is part of a separate whole-store identity and filename before cache decoding.

The implementation is not a native-attribute, geometry-shader, CPU-vertex, skipped-draw, asynchronous-fallback, or guest-timing change.

## Validation

Strict C++17 host tests passed sixteen retained source positives and 128 independent negative mutations plus target/fragment guards. These test known source deltas, not a PICA-register generator replay; each actual runtime specialization also performs the generator comparison.

The private APK `46b59a944d7bc041cce93fee7c3de3eeb5eb3c4780c166494c0c48e432af8140` was built from `0443ac708a7dbef13e4e9267e6621f205a0fa36b`. Both correctness arms used an ordinary boot of the same local Dark Moon USA dump, JIT, GLES, 1×, Performance Mode, fixed guest CLK 100%, frame skipping off, the same initial guest files, empty Cara shader caches, synchronous GL debug, source census and canonical LCD captures. The driver's private cache was preserved and its warmth is unknown.

OFF and ON matched exactly at refreshes 481,515,523,601,721 and1201: guest ticks, all 768,000 RGBA bytes per canonical 400×480 two-screen image, and retained workload vectors through 371,353 PICA draws. There were no synchronous GL errors, failed shader compilations/links, CPU-vertex draws or reported phase failures. ON selected eight specialized vertex sources with positive live source-delta proofs. Both archives held 47 program entries and 17 distinct vertex sources; all fragment sources were identical, nine vertex sources were unchanged and eight changed.

The canonical observer initially failed because it allocated a renderbuffer without explicitly binding it. That OFF attempt was stopped at the first GL error before ON ran. The failed APK, logs and receipt remain retained; the diagnostic-only bind/restore correction was rebuilt and all successful results above use the corrected APK. No result from the failed observer was scored. A stock title save-menu entry was unavailable; its optional failure is recorded and no save-state engine hook or foreign state was used.

## Evidence and limits

Artifacts are retained under `outputs/cara-gcloud-unused-quaternion-20261125/`, including source/component/build receipts, `CORRECTNESS-GATE.json`, exact LCD bytes, raw shader logs and program archives. The source-bound timing summary is in [the result receipt](g-cloud-unused-quaternion-result-20261125.json). Synchronous debug, census and readback are diagnostic overhead; these runs do not establish quiet gameplay FPS, cold-driver time, universal correctness or a complete shader-stutter fix.

## Measured shader-build result

One OFF → ON → OFF sequence, with a fresh empty Cara cache for each arm and identical diagnostics, produced:

| Work | Initial OFF | ON | Later OFF |
| --- | ---: | ---: | ---: |
| Eight changed vertex stages, compile total | 18.262 s | 9.896 s | 18.265 s |
| Twelve programs using those stages, link total | 11.276 s | 5.096 s | 10.230 s |
| Nine unchanged vertex stages, compile total | 20.543 s | 19.935 s | 20.425 s |
| Thirty-five unchanged programs, link total | 74.993 s | 70.678 s | 72.866 s |
| Refresh 515 elapsed | 85.076 s | 70.861 s | 81.555 s |

The later OFF also matched every canonical LCD byte, tick and retained workload vector. Eight changed stages took about 8.37 seconds less compiler time in ON than either OFF. The twelve affected links took at least 5.13 seconds less. Refresh 515 was 10.694 seconds below the faster OFF, versus 3.521 seconds between OFF controls. These are limited descriptive results from one order, not statistical certainty. Some unchanged work also became faster; whole-run changes must not all be assigned to the specialization.

This preserves output while reducing one reproduced cold-Cara-cache shader cost. It does **not** make first play smooth: consuming fragments still require the original quaternion work, and their thirty-five links alone occupied about 70.7 seconds in ON. No quiet gameplay or warm timing verdict was made. The implementation remains private, default off and exact-driver gated; it has not been promoted to a general Adreno or Mali policy.
