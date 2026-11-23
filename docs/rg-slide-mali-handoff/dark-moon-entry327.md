# Reducing indexed geometry work in Dark Moon

Measurements used the physical RG Slide with a Mali-G57, Dark Moon USA, normal boot, Dynarmic JIT, ordered GLES at 1×, Performance Mode and fixed CLK 100%. Fog and lighting texture generations, async mailbox publication, and checked remap were held constant. The timing rows use compile-free lab windows ending at refreshes 3121, 3241, and 3361, and separate uninstrumented GPU sampling during the held lab scene.

## Native strip and fan test

The existing native element option gives GLES the original index stream when the packed unlit path can use a strip or fan. The app recorded zero eligible draws after enabling it. The stable position-shading request count confirms that this option removed no work in the tested scene. The alternate boot bracketed by controls had similar frame times despite the lack of activation. [Same APK](https://github.com/dmikey/cara/actions/runs/45277898425)

| Run | Median range, ms | p99 range, ms | Worst frame | Position requests per 1,000 primitives |
| --- | ---: | ---: | ---: | ---: |
| First control | 29.89–30.77 | 44.48–45.43 | 88.78 ms @ 3004 | 538.597 |
| Native elements enabled | 29.77–30.31 | 42.12–43.18 | 84.65 ms @ 3071 | 538.604 |
| Repeat control | 30.12–30.61 | 37.23–44.20 | 84.70 ms @ 3148 | 538.519 |

A separate trace with native elements enabled still attributed 3.814 ms per refresh of producer overlap to unlit `glDrawElements` at entry 327, including 3.811 ms while the remap result was pending. An entry offset identifies a vertex program entry point and can be shared by multiple programs. The counters report the scene's GPU workload, including other draws and compositor work. [Frame windows](dark-moon-entry327-windows.tsv) · [Hardware counters](dark-moon-entry327-counters.tsv) · [Queue trace](dark-moon-entry327-native-backlog.txt) · [Arm G57 counter definitions](https://arm-software.github.io/libGPUCounters/mali-g57_counter_reference.html)

## Packed float decode in the vertex shader

The optional packed-fetch decoder now reinterprets four aligned float words as a vector. Four fixed swizzles cover all aligned starting word offsets, and the existing scalar path handles unaligned floats and integers. Missing components keep the existing zero and one defaults. The Mali-G57 EGL fixture matched 2,160 vectors against their original 32-bit input words across component counts, strides, a sixteen-byte texel boundary, and the end of a texture row. [Device fixture](https://github.com/dmikey/cara/actions/runs/45281742820)

The ARM64 game APK used separate on-device archives for the ordinary decoder and the optional packed-fetch decoder with the float fast path. The first run in each configuration generated its archive. The measured control, candidate and repeat control all restored 128 binaries with zero source links and had no measured compilation in the comparison windows. The candidate enables texel reuse per attribute as well as the aligned-float fast path, so this comparison measures the combined decoder. [Exact game build](https://github.com/dmikey/cara/actions/runs/45281631205)

| Warm Gadd run | Median range, ms | p99 range, ms | Worst frame | Shader-core cycles per primitive |
| --- | ---: | ---: | ---: | ---: |
| First control | 30.14–30.76 | 39.63–44.96 | 68.81 ms @ 3088 | 821.83 |
| Packed-fetch decoder and float fast path | 28.07–28.85 | 35.37–41.88 | 85.13 ms @ 3022 and 3129 | 770.56 |
| Repeat control | 30.08–30.76 | 37.00–43.88 | 85.36 ms @ 3166 | 826.04 |

The candidate used 6.2–6.7% fewer shader-core cycles per primitive with nearly the same number of position requests. The position-shading stall fraction fell from 34.44–34.60% of GPU-active cycles to 32.08%. The measured warm median improved by about 2 ms. The worst frame remained comparable to the worst repeated control. The same matched title windows improved from approximately 37 ms to 35 ms at the median. [Full warm windows](dark-moon-entry327-vector-windows.tsv) · [Hardware counters](dark-moon-entry327-counters.tsv)

## Duplicate opaque draws

A separate build used the existing PICA packet and GPU-resource independence proof to test skipping a redundant second host draw. It required a real writable depth attachment, strict LESS or GREATER depth testing, and disabled blending and stencil. Guest packets and timing still ran in order. The eligibility check saw 13,104 pairs in the three matched lab windows and elided zero. Shader-core cycles per primitive were 818.06 in the trial and 817.13 in the first control. The zero-coverage switch was removed. [Trial APK](https://github.com/dmikey/cara/actions/runs/45283922019) · [Per-window measurements](dark-moon-entry327-elision-windows.tsv)

## Entry 327 after the decoder change

Two separately instrumented normal boots on the same APK compared the warm windows ending at 2641, 2761 and 2881. The raw-decoder boot had the duplicate-depth trial enabled and elided zero draws. The optimized-decoder boot used its warm archive and ran the ordinary draw path with the packed-fetch decoder and aligned-float fast path. Both restored 128 binaries. The traced producer waited **4.025 to 3.245 ms per refresh** behind entry 327 indexed draws before the remap result. Total pre-call wait moved from 9.650 to 8.507 ms per refresh. These overlap figures locate the part of the producer's critical path that improved. The hardware counters above separately measure scene-wide GPU work. [Matched backlog traces](dark-moon-entry327-vector-backlog.txt)

The optimized decoder remains controlled by `gpu_packed_fetch_reuse` for Android. Frame times still exceed the 16.7 ms budget. The Slide's original signed APK, configuration, and shader archive were restored after all three test sets.
