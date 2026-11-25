# RG Slide verification of unused-quaternion specialization

The same guarded source specialization verified on the G Cloud also passed the physical RG Slide normal-boot comparison. Both measured device/driver tuples now request it by default in Performance Mode. Accuracy Mode, an explicit false preference, unsupported hardware/driver versions and unknown source consumers retain the baseline.

The tested Slide identifies as model `RG Slide`, product name `ums9620_2h10_native`, vendor `ARM`, renderer `Mali-G57`, version `OpenGL ES 3.2 v1.r40p0-01eac0.b0251c048237dcd59e6be15fba11a31a` and GLSL ES 3.20. The shortened `ro.product.device` value `ums9620_2h10` is not the product-name gate. The GPU is the Mali-G57 in the Unisoc T820 SoC, not an older Mali-T820 GPU.

## Physical result

A single OFF → ON → OFF sequence booted the same local Dark Moon USA cartridge normally, without inputs, through the loading sequence and smoky title. Every arm used JIT, GLES Atlas, 1×, Performance Mode, fixed guest CLK 100%, frame skip off, and the existing UBO, layout, raster-state and packed-fetch optimizations on. Guest files began from the same fresh local profile. Cara's cache began empty each time; the driver's cache was preserved and its warmth is unknown.

| Diagnostic cost | Initial OFF | ON | Later OFF |
| --- | ---: | ---: | ---: |
| Eight changed vertex stages, compile total | 3.959 s | 1.978 s | 3.930 s |
| Twelve programs using those stages, link total | 11.090 s | 9.592 s | 11.019 s |
| Nine unchanged vertex stages, compile total | 4.296 s | 4.251 s | 4.287 s |
| Thirty-five unchanged programs, link total | 63.684 s | 62.074 s | 62.406 s |
| Refresh 515 elapsed | 49.872 s | 45.948 s | 48.788 s |

ON was 2.840 seconds below the faster OFF at refresh 515, versus 1.083 seconds between the OFF controls. The affected stages saved at least 1.952 seconds of compiler time and affected links at least 1.427 seconds relative to either control. The unchanged work also varied; the whole-run change must not all be attributed to this specialization. This is one bounded order and not statistical certainty or a quiet gameplay benchmark. Substantial lit shader stutters remain.

All six canonical 400×480 two-screen RGBA captures at refreshes 481, 515, 523, 601, 721 and 1201 matched byte for byte across all three arms, with identical guest ticks. Retained workload vectors matched through 371,353 draws. All three arms reported zero synchronous GL errors, shader compile/link failures, CPU vertex draws and phase failures. ON recorded eight positive independent live full-source delta proofs. No driver program archive was emitted on this Slide, so source grouping uses the emitted census IDs and full-source proofs rather than claiming retained raw shader files. Shader object names were resolved to their sources at each link; a final object-name map would misattribute early links when this driver reuses names.

The frozen runtime APK was `770c8e5571a4c16a36606fe58b0445e88cf44a7b5ded50083fc7a931b514cf92`, source `2d3fe6d7c09af199c03a3b2e4f81a4d8bee20530`, engine fingerprint `0722168b86d334483130b70982a75512745dcafb05102d7db1fc85cb219c7711`. The live consumer/delta algorithm is byte-identical to the G Cloud implementation. Subsequent source changes only select the narrow automatic policy and preserve baseline cache names; the final compiled default is witnessed separately, not described as another full comparison.

## Policy and cache ownership

`[Debug] android_unused_quaternion_v1 = False` explicitly opts out. `True` cannot bypass the exact hardware/driver, Performance Mode, raw packed-input mode 2, fragment-consumer or source-delta checks. Consuming fragments keep the original triangle-reference correction and guest vertex work. No CPU fallback, omitted draw, altered timer, reduced guest clock or title override is introduced.

G Cloud retains the physically measured `.adreno618-uq-v1-off/on` suffixes and policy identity. The Slide's OFF path retains its baseline cache name and identity; only its ON path gets a distinct `rgslide-malig57` policy namespace. The normal source signature changes with source revisions as before. Existing unrelated Mali reuse defaults are unchanged.

The private app now reads **Cara Performance Test**, uses the same painted artwork as main, and exposes Citrus, Silver, Amber, Lime and Onyx in its actual App Icon picker. All five decoded artwork assets matched main's resource pixels and the installed launcher resolved to the Citrus alias. Existing `org.citra.emu` was not modified. The Slide's original `security.perf_harden=0` was recorded and preserved; a copied G Cloud preflight assumption of 1 failed before launch and was corrected without changing the device setting.

## Retained evidence

[Machine-readable result](rg-slide-unused-quaternion-result-20261125.json). Local logs, raw canonical captures, source/component/build/provisioning receipts, icon picker image and preserved preflight failures are under `outputs/cara-android-shared-uq-20261125/`. This comparison does not establish warm lab performance, all-title coverage, a cold driver cache or universal compatibility.
